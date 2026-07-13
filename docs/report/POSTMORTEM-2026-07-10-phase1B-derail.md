# POST-MORTEM 2026-07-10 — Sessione precedente: Phase 1.A/1.B + deriva di debug

> Scopo: registrare **cosa l'agente della sessione precedente ha provato a fare**,
> diff per diff, prima del restore del working tree e della sincronizzazione con
> origin (branch `comprehensive-review`, 18 commit indietro al momento
> dell'analisi). Base locale: `f3c9cb7`. Nessuna di queste modifiche era
> committata; erano divise fra index (staged) e working tree (unstaged), a
> indicare due ondate di lavoro distinte.

## Sintesi

La sessione precedente stava eseguendo la roadmap ASTRA
(`docs/ROADMAP-ASTRA-SERVERIZATION.md`) **Phase 1.A** (unificazione
dell'epilogo di context-switch) e **Phase 1.B** (attivazione dell'input server
thread). La 1.A è stata completata in modo conforme al piano su entrambe le
arch. Attivando la 1.B ha incontrato un hang all'interazione (click) e, invece
di fermarsi a fare root-cause del problema noto e già isolato dal piano (lo
stallo del ritorno-a-USER di `arch_cpu_yield`, Phase 0 "Known-open"), è
**derivata in una campagna di strumentazione di debug invasiva** — watchdog
soft-lockup, deadlock-detector dentro `spinlock_t`, click sintetico iniettato
nel timer tick — che:

1. **sdoppia la logica di lock sopra la HAL**: il nuovo `spin_lock()` generico
   non chiama più `arch_spin_lock()` ma reimplementa lo spin con un loop su
   `arch_spin_trylock()`; su aarch64 questo scavalca e rende morto il percorso
   WFE di `arch_impl_spin_lock` (attesa a basso consumo). Due implementazioni
   dello stesso spin, quella arch non più raggiunta — violazione diretta di
   DIR-06 ("tutto il core attraverso la HAL") e del principio n.2 della roadmap
   ("no hole left open / nessun secondo percorso parallelo").
2. **cambia il layout ABI di `spinlock_t`** (da 4 byte a 16: `owner`,
   `owner_pc`) per pura diagnostica, con scritture non-atomiche dei campi
   diagnostici e una fwd-decl di `panic()` dentro l'header.
3. **lascia codice di test temporaneo nel core**: `[TEMP-CLICK-TEST]` in
   `kernel/core/timer.c` inietta un BTN_LEFT sintetico al tick 400 a ogni boot.
4. applica **fix asimmetrici per arch** senza la controparte o la verifica
   sull'altra arch (MMIO nel PGD idle solo aarch64; azzeramento FP/LR solo
   aarch64; allineamento RSP solo amd64 — quest'ultimo probabilmente corretto
   perché aarch64 ha semantica diversa, ma non documentato come deciso).
5. non ha rispettato il ciclo di validazione vincolante (`make run` grafico su
   ENTRAMBE le arch + conferma del maintainer) prima di stratificare altra
   roba: il tree è rimasto con staged+unstaged mescolati e non testati insieme.

## Cosa c'era di BUONO (da rifare/ri-applicare in modo pulito, non da buttare)

Queste parti sono conformi al piano e vanno **riportate** nella
re-implementazione (migrate, non ricopiate a occhi chiusi — vanno ri-validate):

- **Phase 1.A step 1 (staged, testato secondo la memoria di sessione)** —
  `restore_context` condiviso:
  - `kernel/arch/aarch64/cpu/exception.S`: le TRE copie dell'epilogo di
    restore (vector_stub, irq_stub, arch_cpu_yield) collassate in un unico
    `restore_context` globale raggiunto con `b`. Layout del frame da 816 byte
    in un solo posto. Esattamente il punto 1.A.1 della roadmap.
  - `kernel/arch/amd64/cpu/isr_stubs.S`: identico concetto — epilogo unico
    (pop GP + swapgs-se-ring3 + iretq) raggiunto con `jmp` da
    `common_isr_entry` e `arch_cpu_yield`.
- **`kernel/arch/amd64/include/arch/pt_regs.h`** (staged):
  `pt_regs_init_kernel_task` ora produce `rsp ≡ 8 (mod 16)` all'entry del
  thread, riproducendo l'effetto di una CALL reale (SysV). Fix reale e
  motivato (SSE spill `movaps` → #GP); è il punto 1.A.2 "16-byte rsp
  alignment" della roadmap.
- **Phase 1.B (staged)** — attivazione dell'input server thread:
  - `kernel/main.c`: `input_server_start()` chiamato da `init_scheduler`.
  - `kernel/core/timer.c`: ritiro del bottom-half `input_drain()` dal tick
    (la roadmap 1.B: "il tick-drain si elimina, nessun percorso parallelo").
  - `kernel/drivers/keyboard/keyboard.c`: `wake_up(&input_wait_queue)` in
    `input_report` + **fix del predicato di `kthread_block`**: il vecchio
    `input_ring_nonempty` era logicamente INVERTITO rispetto al contratto
    "keep blocking?" — rinominato `input_should_block` (blocca solo se ring
    vuoto). Questo è un bug-fix reale del substrato.
  - `kernel/sched/process.c`: banda PID dedicata e steal-guard per
    `PROC_PRIO_SYSTEM` (il server thread non migra fra CPU), pin del service
    thread a CPU0 finché il compositor non è SMP-safe (coerente con l'ordine
    Phase 2 → Phase 3 della roadmap).
- **`kernel/sched/elf.c`** (staged): hardening del loader — validazione
  `e_phentsize`/`e_phnum` (ELF-03), range-check di `e_entry` nella finestra
  utente (ELF-01b), `p_filesz <= p_memsz` (ELF-04), bound-check totale del
  payload argv con fallimento pulito dello spawn (ELF-ARGS-01), errore invece
  di successo silenzioso quando `proc->context == NULL`. Lavoro valido in sé
  (linea "security backlog" della cross-cutting track) ma **fuori scope** per
  la Phase 1.B ed eseguito con sciatteria: commento duplicato, churn di
  riformattazione, newline finale del file rimossa. Nota aperta nel diff:
  `TODO(ELF-CLEANUP-01)` sul possibile leak di pagine fisiche nel path di
  fallimento (da verificare in una passata dedicata).
- **`kernel/sched/process.c`** (unstaged): `pmm_free_page(proc)` → 
  `kfree(proc)` nel path di errore di `process_create_caps` — se `proc` viene
  dal kmalloc pool questo era un vero bug di free-mismatch. DA VERIFICARE e,
  se confermato, ri-applicare come fix puntuale.
- **`kernel/arch/aarch64/cpu/cpu.c`** (unstaged): mappare la finestra MMIO
  (VirtIO a 0x0A000000, range TTBR0) dentro `aarch64_idle_user_pgd`, perché i
  kernel service thread girano con `page_table == NULL` → TTBR0 = PGD idle, e
  senza mapping un accesso MMIO dal thread fault-a silenziosamente. Questa è
  molto probabilmente **la vera root-cause (o una delle due) dell'hang
  dell'input thread su aarch64** — scoperta valida, da ri-derivare e validare,
  con la domanda simmetrica per amd64 (i kernel thread amd64 che page table
  vedono? serve la controparte o è già coperta?).

## Cosa era SBAGLIATO (il "casino" da non ripetere)

- **`kernel/include/kernel/spinlock.h`** (unstaged): deadlock-detector che
  riscrive `spin_lock`/`spin_trylock` in generico bypassando
  `arch_spin_lock` (WFE aarch64 morto), allarga `spinlock_t` a 16 byte,
  timeout arbitrario di 10 s con `panic()` in header. Strumento di debug
  trasformato in modifica strutturale permanente, mai testato su entrambe le
  arch. Se in futuro servirà un lock-debug, va fatto DENTRO il contratto HAL
  (o come build-flag `CONFIG_DEBUG_SPINLOCK`-style), non sopra.
- **`kernel/include/kernel/cpu.h` + `kernel/core/timer.c`** (unstaged):
  watchdog soft-lockup per-CPU (stamp + scan degli altri CPU nel tick).
  NOTA DI EQUITÀ: il watchdog era stato **richiesto dal maintainer** (il
  "fault system per gli hang", linea DIR-05) — l'idea è legittima e va
  ripresa. Lo sbaglio è stato nell'esecuzione: mai compilato né testato,
  stratificato insieme al deadlock-detector negli spinlock e al click-test
  sintetico nello stesso tree sporco, invece di essere un passo progettato,
  HAL-uniforme e validato 2-arch a sé stante.
- **`kernel/core/timer.c`** (unstaged): `[TEMP-CLICK-TEST]` — iniezione di un
  click sintetico al tick 400 marcata "Remove after verifying", cioè codice
  usa-e-getta lasciato nel core del kernel.
- **Metodo**: davanti all'hang (il problema era GIÀ noto e isolato nel piano:
  stallo dello switch-to-USER di `arch_cpu_yield`, roadmap Phase 0), la
  risposta giusta era root-cause chirurgico su quel percorso; la risposta
  data è stata stratificare tre sistemi diagnostici nuovi + un generatore di
  eventi finti, mescolando fix veri e strumentazione nello stesso working
  tree, senza mai chiudere il ciclo build+`make run` 2-arch+conferma.

## Stato del tree al momento del restore

- Staged: README.md (✅→⚠️ Debian/Arch con typo ";,"), exception.S,
  isr_stubs.S, pt_regs.h (amd64), timer.c, keyboard.c, main.c, elf.c,
  process.c.
- Unstaged: cpu.c (aarch64), pt_regs.h (aarch64), timer.c, keyboard.c,
  cpu.h, spinlock.h, process.c.
- Tutto preservato in uno stash git prima del restore (vedi sotto), quindi
  ogni singolo hunk resta recuperabile: la re-implementazione pulita deve
  ripartire dalla roadmap, riprendendo da qui solo ciò che viene ri-validato.

Comando di salvataggio: `git stash push --staged`-equivalente non usato;
usato un unico `git stash push` (include staged+unstaged) etichettato
`premorte-agente-2026-07-10-phase1B`. Recupero: `git stash list` /
`git stash show -p stash^{/premorte}`.

## Ripartenza corretta (per la prossima sessione)

1. Sync: `git pull --ff-only` su `comprehensive-review` (i 18 commit remoti
   toccano solo Makefile/Makefile.linux/README/tools — nessun overlap kernel).
2. Ri-applicare Phase 1.A step 1 (restore_context 2-arch) + fix RSP amd64,
   build `-Werror` 2 arch, `make run` grafico 2 arch, conferma maintainer.
3. Phase 1.B: PRIMA root-cause dello stallo switch-to-USER (ipotesi forte:
   MMIO mancante nel PGD idle su aarch64, più eventuale controparte amd64),
   POI attivazione del thread, un pezzo alla volta, test 2-arch a ogni passo.
4. Hardening ELF: passata separata e dichiarata (non mescolata alla 1.B),
   con verifica del TODO(ELF-CLEANUP-01).
5. Niente strumentazione diagnostica permanente fuori piano: se serve, si
   progetta dentro la HAL e si porta su entrambe le arch (DIR-06).
