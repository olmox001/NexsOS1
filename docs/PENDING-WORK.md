# NEXS — Registro del lavoro NON terminato (trasparenza totale)

Questo documento elenca **tutto** ciò che è stato richiesto/pianificato ma **non
concluso**, emerso dalla sessione di lavoro attorno a TIMER-UAF-01, alla notifica,
alla rinomina `OS1_`, alla conformità HAL (#140) e al decoupling del compositor.
Niente è omesso: include parziali, rinviati, parcheggiati e follow-up.

Legenda stato: **PARZIALE** (iniziato, non chiuso) · **NON INIZIATO** (catturato,
non implementato) · **PARCHEGGIATO** (rinviato per scelta) · **FOLLOW-UP**
(conseguenza di una modifica fatta).

Per il contesto di ciò che È stato chiuso vedi `docs/report/TIMER-UAF-01-CERTIFIED-FIX.md`
e i doc di direzione `docs/direction/DIR-01..06`.

---

## 1. Rinomina API `OS1_` di massa — **PARZIALE** (invariato) · DIR-01 / #137
- **Fatto:** solo il pilota `sleep → OS1_sleep` (commit `9a01bfe`), su tutti i caller (escluso nxfilem sospeso).
- **Resta (richiesto come "prossimo task"):** prefissare `OS1_` a TUTTE le funzioni base non-POSIX:
  `yield, spawn, spawn_args, spawn_caps, spawn_level, kill_process, wait,
  create_window, destroy_window, window_draw, window_blit, set_window_flags,
  compositor_render, set_focus, flush, notify, registry_read, registry_write,
  get_time, get_pid, print, printf_win, draw, …`
- **Resta:** shim POSIX/libc coi nomi nudi dove esiste l'equivalente; handle tipizzati
  a oggetti (`OS1_window_t`, `OS1_surface_t`, `OS1_process_t`, `OS1_file_t`,
  `OS1_socket_t`, `OS1_service_t`, `OS1_handle_t`).
- **Aggiornamento (2026-06-20, ASTRA §7.1):** la metà "tutto è un oggetto" è atterrata
  come **layer a capability reale** (handle non falsificabili + diritti separabili/
  attenuabili, `OS1low_handle_*`/`OS1_object_*`, `include/api/object.h`) — è l'`OS1_handle_t`
  generico sotto i tipi sopra. Resta il **refactor della call-surface** (#164): prefissare/
  unificare *tutte* le syscall/verbi legacy sul modello `OS1_`/`OS1low_` + capability.
- **Doc:** `docs/direction/DIR-01-naming-and-objects.md`.
**Aggiornamento (2026-07-02):** verificato invariato — `OS1_fs_write` resta sul
percorso ambient (blocco O_CREAT, `user/sys/lib/lib.c:370-375`); `OBJ_TYPE_PORT`
e `OS1low_vm_map/_unmap/_protect` non esistono. La rinomina di massa non è
iniziata. **Landato invece** (non era in questo item): fd table assorbita
nell'handle table (`kernel/fd.h` non esiste più) e registry/­`/proc` come
namespace tree — vedi `docs/ASTRA.md` §7.1/§7.6.

## 2. Conformità HAL completa — **PARZIALE** (avanzata) · DIR-06 / #140 (HAL-ARCH-01)
- **Fatto:** audit del core (pulito: no inline asm / ISA in `kernel/core|sched|mm|irq`);
  EOI già dietro contratto `irq_chip_end()→chip->end()`; `elf.c` ora usa profili VMM
  arch-neutrali (`PAGE_USER`/`_DATA`/`_RX`/`_RO`), niente `#ifdef` arch (commit `93dc586`).
- **Resta:**
  - `kernel/main.c`: signature di `kernel_main` diversa per arch (multiboot magic+mbi
    vs x0/FDT) → eventuale shim `arch_boot_args()`.
  - `kernel/drivers/virtio/virtio_input.c`: `#ifdef ARCH_AMD64` da rivedere.
  - `kernel/drivers/ps2/ps2.c`: gate amd64 (PS/2 è HW x86 → legittimo, da confermare).
  - `platform.c` (ARCH-03: `timer_get_us` dummy su amd64) — congelato fino a B4.
  - Obiettivo dichiarato: *TUTTO il kernel solo primitive HAL, non vede il layer arch*.
- **Doc:** `docs/direction/DIR-06-hal-conformance.md` · `docs/ASTRA.md` (regola 5).
- **Aggiornamento (2026-07-02):** ✓ **fatto** — unificato il fault-reporting
  utente fra le due arch: `fault_handle_user_or_panic()` (`kernel/core/fault.c:56-88`)
  è ora l'UNICA funzione, chiamata identica da tutti e 7 i punti di ingresso
  fault amd64/aarch64, zero `#ifdef` interni (commit `7d3a209`). Resto invariato
  (main.c signature, ps2.c gate, platform.c congelato fino a B4).

## 3. Decoupling compositor COMPLETO — **PARZIALE** · DIR-02 (#83/#67/#69 chiusi)
- **Fatto:** #83 (scheduler non chiama più il compositor), #67 (focus single-owner via
  `sched_set_focus_pid`), #69 (close via seam `window_request_close`).
- **Resta (la visione piena DIR-02):**
  - compositor **guida sé stesso**: frame pacing / vsync / dirty regions proprie,
    indipendenti dalle app → API `window_present()` / `window_commit()` al posto di
    `window_blit()` + `compositor_render()`;
  - API a **finestre, non a PID**: `window_focus(win)` / `window_activate(win)`,
    `process_get_primary_window(pid)` / `window_get_owner(win)` invece di
    `set_focus(get_pid())` / `window_of_pid(pid)`;
  - **compositor come componente della HAL** (un core-restart preserva framebuffer e
    stato grafico);
  - ~~**`compositor_get_focus_pid()` ora morta**~~ ✓ **fatto (verificato 2026-07-02)**:
    non esiste più in `compositor.c` (solo un commento a `compositor.c:934` ne
    documenta la rimozione); sopravvive solo nei due file di riferimento
    `compositor.c.old`/`.c.new` (non referenziati da alcuna build rule).
- **Doc:** `docs/direction/DIR-02-compositor-decoupling.md`.
- **Aggiornamento (2026-07-02):** ✓ anche `compositor_update_mouse` (era l'unico
  mutatore della window-list senza lock) ora prende `compositor_lock`
  (`kernel/graphics/compositor.c:1356-1466`, commit `492e5ec`) — chiude una race
  drag/resize. Resto della visione DIR-02 (window-centric API, HAL component)
  invariato.

## 4. Modello eventi unico `event_wait()` — **PARZIALE** · DIR-03 / #138
- `OS1_event_wait(&ev)` che unifica `EVENT_KEY/MOUSE/IPC/TIMER/WINDOW/PROCESS`;
  un solo event loop, niente busy-poll per costruzione; generalizza il
  recv-with-timeout (#135). Porting di notify_srv e shell come adopter di riferimento.
- **Fatto (2026-06-20, ASTRA §7.5):** la gamba **input** è unificata —
  `input_poll_event(input_event_t*)` copre tastiera/mouse/resize in un solo evento
  (`INPUT_TYPE_KEYBOARD/MOUSE/RESIZE` dai transport IPC `IPC_TYPE_INPUT/MOUSE/RESIZE`),
  costanti centralizzate in `include/api/input.h` (`MOUSE_BTN_*`, `KEY_*`).
- **Resta:** il `OS1_event_wait` bloccante completo (anche IPC/timer/window/process,
  ~0% idle); consegna mouse oltre la finestra in focus; broadcast desktop-resize.
- **Doc:** `docs/direction/DIR-03-unified-events.md`.

## 5. Capabilities / servizi / no-fork / App-Model vs Kernel-ABI — **PARZIALE** · DIR-04
- **Fatto (2026-06-20, ASTRA §7):** **layer a capability reale** — handle non
  falsificabili a oggetti del kernel con diritti separabili/attenuabili
  (`OS1low_handle_*`/`OS1_object_*`, syscall 235..243, `include/api/object.h`; §7.1);
  **preset di privilegio per percorso** (`/sys/bin`=ROOT, `/bin`=USER, creator-clamp
  monotono, `/sys/bin` write-protected; §7.2); **servizi SRL stratificati** sicuri-per-
  chiamante (helper riusabile + frontend sottile: `nxres`, `nxproc`/`nxproc.h`; §7.4).
  `fork()` resta inesistente (solo `spawn*`/`spawn_caps`).
- **Resta:** rifinitura **per-servizio** delle capability (oggi i servizi `/sys/bin`
  partono tutti al preset ROOT); famiglie di syscall coerenti `proc_*` / `fs_*` /
  `window_*` / `input_*` (legato al refactor della call-surface DIR-01); separazione
  piena **Application-Model** (userlib stabile) vs **Kernel-ABI** (interno, minimale);
  servizi pianificati `nxinfo` / `nxperms`.
- Estende #79 / #95 / #120. **Doc:** `docs/direction/DIR-04-capabilities-and-services.md`.

## 6. Trace debugger / recovery — **PARZIALE** (era NON INIZIATO) · DIR-05 / #139
- risoluzione **linea C esatta** dai backtrace (DWARF `.debug_line`) — **ancora NON
  INIZIATO**, verificato 2026-07-02 (nessuna logica `debug_line`/`addr2line` in
  `kernel/lib/backtrace.c`/`fault_print.c`);
- **modalità recovery**: quiescere/resettare il sottosistema o il core invece di `panic()`
  (in coppia col core-restart nella HAL) — **ancora NON INIZIATO**, verificato
  2026-07-02 (nessun simbolo `recovery_mode`/`kernel_recover` nel kernel);
- ~~**panel di panico a schermo** via blit HAL minimale (oltre alla UART), per utenti GUI/UTM~~
  ✓ **FATTO (2026-07-02, F0.0.4.2, commit `dbbd80d`)**: `panic_screen()`
  (`kernel/graphics/graphics.c:171-179`, rosso `0xFFB91C1C`) scrive direttamente
  sul framebuffer GPU primario, bypassando il compositor — UART-indipendente
  come richiesto.
- **Landato in aggiunta (non nello scope originale di questo item, ma coerente
  con DIR-05):** watchdog reboot ~10s dopo panic (`kernel/lib/printk.c:243-247`,
  commit `cc29695`); crash utente → notifica rossa via `fault_notify_user()`
  (`kernel/core/fault.c:35-49`, commit `fb94873`), distinta dal path di panic
  kernel.
- **Doc:** `docs/direction/DIR-05-fault-recovery-and-debugger.md`.

## 7. Notifiche — parte 2 — **QUASI CHIUSO** (era PARZIALE) · #119
- **Fatto (2026-06-20):** popup appare al boot, render una volta, auto-hide 2s, finestra passiva
  click-through (commit `82a4cb4` + flag passive nel compositor).
- ~~**Resta:** *warning/errori del kernel e dello userspace devono diventare notifiche
  visualizzabili*~~ ✓ **FATTO (2026-07-02)**: crash utente → notifica rossa
  automatica (`kernel/core/fault.c:35-49`, severità `data1=2`); panic kernel →
  schermo rosso + watchdog (vedi item 6). Modello dati rifatto: rename
  `notification_server` → `nxntfy_srv` + CLI `nxnotify` (commit `27cf792`);
  modello a messaggi in registry con raggruppamento per processo (`from`),
  severità e stato di lettura (`U`/`R`) (commit `eb08ab1`); `init` possiede
  `srv.notify_pid` (non più il server stesso, commit `48435f5`).
- **Regressione utente da segnalare:** il builtin `notify` della shell è stato
  **rimosso** nello stesso pass di rename (dopo un breve ripristino in
  `465ada0`, tolto di nuovo in `27cf792` "drop shell builtin"). Ad oggi
  `nxshell` non ha alcun dispatch `"notify"` — solo la CLI esterna `nxnotify`
  funziona. Verificato su `user/sys/bin/nxshell.c` a HEAD.

## 8. nxfilem — **PARCHEGGIATO/INCOMPLETO**
- Pushato come **non funzionante** (commit `dc80001`), su tua indicazione
  ("è da pushare ma non funziona, ci lavoreremo in seguito"). Da completare.

## 9. compositor.c.new — protocollo video — **PARCHEGGIATO**
- La modifica al protocollo video "non è ancora il momento di applicarla": resta parcheggiata
  in `kernel/graphics/compositor.c.new` (il `.c` è stato riportato allo stato stabile).
- **Cleanup pendente:** `kernel/graphics/compositor.c.old` è un backup da rimuovere quando non serve più.

## 10. Follow-up dei timer — **FOLLOW-UP**
- **TIMER-UAF-02** (hardening, non difetto): far cancellare `sleep_timer` a `kernel_ipc_send`
  (e ad ogni wake non-timer di un `PROC_SLEEPING`), così un risvegliato non porta un timer
  pending stale. Oggi innocuo (il fire spurio rispetta la guardia `state==PROC_SLEEPING`).
- **#135 TIMER-CAP-01:** oggetti timer/timeout a capability (stile seL4) sul clock monotono.
- **recv-with-timeout** (citato in #134/#135): unificherebbe i due rami di notify_srv → DIR-03.

## 11. Decoupling input/IRQ residuo — **FOLLOW-UP**
- **SCHED-03:** il kill da close-button gira ancora in **mouse-IRQ context** (ora localizzato
  dietro `window_request_close`); va deferito a un contesto sicuro (non-IRQ).
- **#68 (GFX-COMP-02):** `compositor_update_mouse` muta `windows[i].x/y` e i globali di damage
  in IRQ-context senza lock — NON affrontato in questa sessione, parte del cluster compositor.

## 12. Robustezza / "kernel a prova di fault" + stress — **PARZIALE**
- **Fatto:** TIMER-UAF-01 risolto; verifica 0 PANIC su 2 arch; amd64 retto sotto **101 processi**
  (carico forkbomb) via `make run`.
- **Resta:** un **harness di stress** completo con istanze multiple simultanee di
  doom + demo3d + top + forkbomb come richiesto ("va provato e stressato"), non costruito.

## 13. Accordi di processo (working agreement) — **DA RISPETTARE SEMPRE**
Salvati anche in memoria; ripetuti per trasparenza:
1. Con un trace di panic disponibile → **forensics dai log** (decode ESR/FAR + objdump del
   faulting address), NON riprodurre crash load-dependent.
2. **SEMPRE** build **+ boot di ENTRAMBE le arch** con i target ufficiali
   `make run` / `make run ARCH=amd64` prima di "fatto"/commit/push.
3. **Leggere i documenti** (README, `docs/`, ASTRA) PRIMA di iniziare a lavorare.

---

## Chiuso / aggiornato (sessione 2026-06-20) — batch capability/oggetti
Fonte autorevole: `docs/ASTRA.md` §7 (build `-Werror` su 2 arch, 0 panic; `captest` 9/9,
`capkill` 5/5).
- **Layer a capability reale** ✓ (§7.1) — object manager: handle non falsificabili a
  oggetti refcontati, diritti separabili/attenuabili; syscall 235..243 (`OS1low_handle_*`,
  `OS1_object_*`), `include/api/object.h`. Chiude la parte "oggetti" di DIR-01/#164 (item 1).
- **Window objects (`OBJ_TYPE_WINDOW`) + dock** ✓ (§7.3) — finestre come capability
  (`OS1_window_enum`/`_minimize`/`_restore`/`_focus`/`_close`, `SYS_WINDOW_ENUM` 202,
  `struct window_info`/`WININFO_*`); il **Window Server `/sys/bin/nxui`** come servizio
  ROOT userland supervisionato da init. Compositor lasciato minimale.
- **Preset di capability per percorso** ✓ (§7.2) — `/sys/bin`=ROOT, `/bin`=USER, sotto
  creator-clamp monotono; `/sys/bin` write-protected (binari immutabili).
- **Servizi SRL stratificati** ✓ (§7.4) e **ABI input unificata** ✓ (§7.5) — vedi item 4/5.
- **Resta (prossimi):** rifinitura cap **per-servizio**; **broadcast desktop-resize** alle
  app (DIR-03/DIR-07); servizi `nxinfo`/`nxperms`; **singleton `nxui`** (una sola istanza
  del dock); **refactor della call-surface** su `OS1_`/`OS1low_` + capability (DIR-01/#164).

## Chiuso (sessione 2026-06-18)
- **#71 LIB-SSP-01** ✓ — canary SSP randomizzato al boot da `entropy_u64()` unificato
  (HW RNG `arch_hw_random` RNDR/RDRAND + fallback cycle-counter splitmix64); commit `36fa344`.
- **#51 DRV-PCI-01** ✓ — già implementato: `pci_scan_and_register()` definita
  (`pci.c:272`) e chiamata su aarch64 (`hal.c:123`); amd64 via callback virtio + BAR firmware.
- **#52 DRV-UART-01** ✓ — ring RX del PL011 (aarch64) ora sotto `rx_lock` dedicato
  (spinlock+irqsave) in produttore e consumatori; idle fuori dal lock; commit `f4b1458`.

## Backlog di progetto (fuori dallo scope di questa sessione, qui per completezza)
Tracciato in GitHub e in `docs/review/REVIEW.md` (#19, ~220 finding). Epic e cluster aperti:
- **#19** Comprehensive Code Review (tracking, ~220 finding).
- **#94** amd64 boot parity & 4GB · **#95** Service isolation (seL4) & Plan 9 namespace ·
  **#96** SMP correctness & data races · **#120** Userland a cipolla (libc POSIX/toolkit/3D).
- **Grafica:** #118 damage/redraw incompleto · #121 GFX-DYN (de-hardcode risoluzione) ·
  #128 caret per-frame · #131 GFX-PERF cursore · #133 cursore UTM assoluto.
- **Input/Driver:** #125 pointer assoluto UTM · #129 scancode mancanti · #130 USB UTM ·
  #124 PS/2 senza 8042 · #48 GIC affinity · #53/#54/#49/#45 virtio/gpu/pci (#51 ✓) · UART lock (#52 ✓).
- **FS:** #126 creazione file ext4 · #127 truncate mancante.
- **Userland:** #123 ereditarietà stdout/TTY · #76 init.cfg · #81 regedit recv · #82 nxfont.
- **Sched/arch/mm:** #84 AB-BA lock · #38 CPU-AMD64-01 FPU su preempt · #122 quota figli
  (fork-bomb) · #39/#40/#41/#43 TOCTOU uaccess · #30/#31/#32 ARCH/HAL boot · #20 PMM multi-region ·
  #72 registry (#71 SSP ✓) · #106 doom fire button.

> Questo registro va aggiornato man mano che gli item vengono chiusi.


## 14. Diagnostica timer/SMP e compositor — 2026-11-03 **PARZIALE**
Dettagli, log e misure: `docs/report/TIMER-SMP-DIAGNOSTIC-2026-11-03.md`.
- `nxbar` usa `get_time()` monotono in millisecondi e `OS1_sleep(33)`; il diagnostico aggiornato conferma clock coerente su entrambe le architetture: 160 campioni, 0 regressioni/mismatch e `failures=0` per arch.
- AArch64: corretti due difetti distinti: `SEV` dopo il rilascio degli spinlock AArch64; ack SMP pubblicato prima di abilitare gli IRQ in `kernel_secondary_main()` (race di preemption tra IRQ-enable e ack). Boot SMP=4 successivo: BSP conferma CPU1/2/3 online, nessun timeout.
- AMD64: SMP=4 confermato (CPU1/2/3 online), incluso il boot dopo la modifica condivisa dell'ordine ack/IRQ.
- `SYS_COMPOSITOR_RENDER` è single-owner PID 1 (nxinit); le altre chiamate CAP_WINDOW sono hint. Modifica compilata e boot-testata su entrambe le architetture.
- Timer dopo il fix SMP: AMD64 OS1_sleep media 42.15 ms/max 84.32 ms; nanosleep 39.41/61.94 ms. AArch64 OS1_sleep 46.95/132.91 ms; nanosleep 49.25/121.63 ms. `get_time()` gap finale 1/0 ms, 0 regressioni e mismatch.
- KTEST 8/8, timerdiag PASS, NXShell responsiva e faults=0 su entrambe. Soak NXShell di circa 60 s per arch completato con sei comandi distanziati; entrambi terminati con shell=yes e faults=0. VM arrestate.
- Nuovi `GFX-TIMING`: `render_hold` 34.7–72.3 ms AMD64 e 40.7–130.0 ms AArch64. `compositor_render()` mantiene `compositor_lock` con IRQ locali disabilitati durante tutto il rendering sincrono; serve refactor con snapshot/ownership e gestione IRQ, non basta rimuovere irqsave.
- **GFX-LEGACY-TERMINAL-01 — OPEN / INTERMITTENTE:** una clean AArch64 test-all del 2026-11-03 ha registrato SSP: Stack Smash al ritorno da `compositor_window_write()` (PID 8, dopo KTEST 11/11); simbolo ed epilogo verificati con addr2line/disassembly. Un run diagnostico successivo ha passato 30 s con faults=0: non è una chiusura. Il percorso `window_text_write → compositor_window_write → term_write → gl_draw_char` rasterizza mentre `compositor_lock` è acquisito con irqsave. La causa della sovrascrittura canary non è ancora provata. Ipotesi da discriminare: timer IRQ pending subito dopo irqrestore, perché kernel_timer_tick() può schedulare anche frame EL1. Auditare il context-switch e i vincoli pending_reenqueue senza presupporre che sia la causa. Non rimuovere irqsave né rilasciare la lock senza un protocollo di pin/lifetime e serializzazione terminale. Piano, evidenze e gate: `docs/report/GFX-LEGACY-TERMINAL-CONCURRENCY-2026-11-03.md`; chiudere come preflight GFX-NG-0 prima di GFX-NG-1.
- `make ARCH={amd64,aarch64} all` e `boot-iso` sequenziali PASS anche dopo la modifica condivisa SMP. Restano stress combinato, riduzione critical section/jitter e bug POSIX `time()` ms/sec.


## 15. Migrazione grafica in userland / surface manager kernel — **PIANIFICATO** · NEXS-NG

Decisione architetturale 2026-11-03: compositor, window/scene policy e librerie
grafiche devono essere servizi/librerie ring-3. Il kernel conserva soltanto il
gestore capability di buffer/surface e il relativo controllo: memoria,
metadati, validazione, mapping, diritti, lifetime, pinning, sincronizzazione,
commit e hand-off sicuro alla presentazione. Nessun raster/compositing/GL/font
nel kernel NG.

- **Piano dettagliato:** `docs/architecture/GRAPHICS-USERLAND-MIGRATION.md`.
- **Documenti allineati:** NEXS-NG-CORE-DESIGN, NEXS-NG-CLEANROOM, DIR-02,
  DIR-07, graphics-port README/STATE. La vecchia frase "compositor nella HAL"
  è esplicitamente superata per NG.
- **Inventario sorgente:** `compositor.c` → `services/compositord` (da
  spezzare in scene/window policy, input/event handling, frame scheduling e
  composizione); `gl.c`, `draw2d.c`, `draw3d.c`, `font.c`, `chrome.c`,
  `compositor_style.c`, `region.c`, `term.c` → userland; `surface.c`
  va diviso tra gestione oggetti/mapping nel kernel e primitive pixel in
  userland; driver GPU/display diventano servizi isolati con MMIO/IRQ/DMA
  capability. Il kernel mantiene una piccola console panic indipendente.
- **Punto critico:** il `gfx_surface` attuale è un wrapper di rendering
  basato su `gl_surface`/puntatori, non un gestore di oggetti capability.
  Non va semplicemente spostato in userland o rinominato: va sostituito nel
  kernel NG da veri oggetti buffer/surface con lifetime e mapping verificabili.
- **Ordine:** GFX-NG-0 inventario ABI/callers → GFX-NG-1 buffer/surface manager
  nel tree parallelo `kernel-ng` → GFX-NG-2 display/GPU service minimo →
  GFX-NG-3 compositord + broker ABI v1 → GFX-NG-4 librerie grafiche →
  GFX-NG-5 accelerazione e stress.
- **Vincolo di migrazione:** non spostare massivamente i file del kernel legacy
  né interrompere il sistema attuale. La vecchia implementazione resta il
  riferimento bootabile finché il nuovo kernel e la compatibilità ABI non
  superano i gate G0–G6. Nessuna nuova syscall grafica ambientale; riusare
  object/VM/port capabilities e aggiungere solo primitive indispensabili.
- **Stato attuale (2026-11-08):** `nxcompositor` è stato aggiunto come
  processo ring-3 supervisionato e possiede il frame loop; una libreria
  userland `os1_gfx` contiene primitive ARGB8888 e `OS1_window_draw` le usa
  prima della presentazione v1. Il renderer, la scena, il window manager,
  l'input e i driver restano nel kernel legacy; `SYS_COMPOSITOR_RENDER` è
  ancora il backend di compatibilità. Non esistono ancora oggetti
  buffer/surface capability né mapping condivisi. Questo è uno scaffold
  eseguibile, non il completamento della migrazione. Prossimo gate: completare
  l'inventario semantico GFX-NG-0 e definire i descriptor ABI rispetto agli
  oggetti/VM/port effettivi prima di introdurre il surface manager.


**Avanzamento GFX-NG-0 (2026-11-03):** primo inventario statico aggiunto
in §10 di `GRAPHICS-USERLAND-MIGRATION.md`: syscall grafiche 200/202,
210–215, 217–219, 253, 270–275; oggetti/capability esistenti; seam
`os1_video_platform`; footprint sorgente (7.232 righe nel perimetro
misurato); dipendenze principali e destinazioni. Non è ancora l'inventario
semantico completo dei caller e dei flussi IRQ/input; nessun ABI buffer/surface
è stato assegnato. L'incremento runtime descritto nello stato aggiornato
aggiunge il frame service e primitive userland, ma non estrae ancora i
componenti legacy né implementa il surface manager.


## 16. NEXS-NG integrated hardening / boot / device / graphics programme — **DOCUMENTED, NOT IMPLEMENTED**

Nuovi documenti di riferimento (2026-11-03):
- `docs/architecture/NEXS-NG-INTEGRATED-IMPLEMENTATION-PLAN-2026-11-03.md`
- `docs/architecture/NEXS-NG-DIAGNOSTICS-AND-TESTING.md`
- `docs/architecture/NEXS-NG-DEVICE-LIFECYCLE-AND-HOTPLUG.md`
- `docs/architecture/GRAPHICS-USERLAND-MIGRATION.md`

Punti emersi dall'audit:
- ASTRA §7.10 descrive render in process context, non estrazione ring-3;
  correzione aggiunta in ASTRA.
- Object manager legacy: handle slot index, 32 slot, refcount, rights
  attenuabili; generation/stale-handle model da verificare e formalizzare.
- Programme R mantiene la sequenza R1/R1b/R2/R3/R4/R5/R6; la nuova migrazione
  grafica deve integrarsi in R3, non duplicarlo.
- `hal_bus.c` è registry statico boot-only (MAX 64); hotplug dinamico non
  implementato. PIANO-DRIVER-MATURITY Fase 2 e Programme G5 restano la fonte
  per il comportamento attach/detach.
- **BOOT-1 FDT hardening completato:** parser bounded, reservation-map, `/reserved-memory` statico e DTB reservation; host gate 61/61 PASS sul DTB QEMU; clean build, boot GRUB BIOS AMD64 e GRUB arm64-efi AArch64, EFI x0/x1, legacy search, KTEST, shell/nxwins, soak 30 s e fault scan 0 verificati su entrambe le architetture. Restano fuori scope fuzzing continuo, discovery device generica e certificazione hardware.
- AArch64 EFI stub è presente, AMD64 ISO attuale è BIOS GRUB: UEFI dual-arch
  non è completo né certificato.
- `nxadb.py` mantiene il default headless e supporta `--display default`; screenshot QMP, manifest, input e round-trip visuali sono stati implementati e verificati nei run documentati sotto.
- Flag attuali sono già rigorosi; NX_STRICT è opzionale. Nuovi warning e
  sanitizer saranno introdotti in profili misurabili, uno step per volta.

**Sequenza:** baseline e strumenti → syscall/object/registry crosswalk +
boot/FDT/device audit → hardening → device lifecycle → surface manager +
diagnostica → display/compositor userland. Entrambe le arch obbligatorie.
BOOT-1 è il primo hardening runtime del percorso legacy avviato da questo programma; le fasi NEXS-NG (object/device/surface manager) restano non implementate e vincolate ai gate successivi.


**Crosswalk syscall/object completato come prima ricognizione:** vedere
`docs/architecture/SYSCALL-OBJECT-CAPABILITY-CROSSWALK-2026-11-03.md`.
Conferma: file POSIX già passa da FILE handles; registry mantiene SYS_REGISTRY
diretto + REGKEY inutilizzato + /reg; PID IPC coesiste con PORT; kill/wait
coesistono con PROCESS object; le window syscall restano collegate direttamente
al compositor kernel. La sequenza R2–R5 resta vincolante.


**Osservabilità reale implementata e provata:** `nxadb screenshot` ora
cattura esplicitamente `nxgpu0/head 0`, valida PNG e scrive manifest;
`nxadb mouse dx dy --click` controlla il puntatore; `nxadb start
--display default` abilita finestra QEMU visibile; delay tastiera portato a
75ms dopo test di perdita caratteri. Screenshot + command round-trip verificati
su amd64 e aarch64; click launcher e nxsettings verificato su amd64. Artefatti
in `build/runs/<arch>/`. Vedere
`docs/architecture/NEXS-NG-DIAGNOSTICS-AND-TESTING.md`.


**Audit hardware approfondito:** `pci.c` abilita BusMaster per ogni device
prima del binding/DMA, mappa BAR con span fisso 128 KiB e non dimensiona BAR
64-bit completi; USB core dichiara esplicitamente hotplug runtime come Fase 2
non implementata; GPU registry non drena refcount su unregister. Il nuovo
device manager non deve riutilizzare queste semantiche. BOOT-1 ora trasmette
anche x1/DtbSpan nel percorso EFI AArch64 e normalizza il record BootInfo v1
con `fdt_phys + fdt_size`; il record comune multi-arch e gli altri dati EFI
restano lavoro futuro.


**2026-11-03 — boot capture / RAM map / hardening baseline (pre-BOOT-1):** nxadb now keeps
per-run UART/QEMU stdout/stderr and manifests, complete bootlog, screenshots,
QMP input, and 30-second make test gates. AMD64 and AArch64 QEMU runs both pass
KTEST 8/8, nxwins enumerate+focus, two screenshots and zero faults. Kernel
default strict/warning gates expanded. BOOT-MEM logs distinguish usable sum,
usable span and reserved descriptors. See
docs/architecture/NEXS-NG-DIAGNOSTICS-AND-TESTING.md.

**BOOT-1 — COMPLETATO (2026-11-03):** il handoff EFI conserva x1/DtbSpan;
`fdt_init(base, available_length)` usa il parser bounded condiviso; reservation
map, `/reserved-memory` statico e DTB entrano nei descrittori RESERVED del PMM.
Il test host ASan/UBSan ha superato 61 controlli includendo il DTB QEMU reale
(1 MiB, 4 CPU). Clean build AMD64+AArch64, ISO GRUB BIOS/arm64-efi, boot EFI
AArch64, KTEST (10/10 AMD64, 11/11 AArch64), fallback legacy page-by-page,
NXShell, nxwins focus, soak 30 s, screenshot 1280×800 e fault scan 0 sono PASS.
Log completi e dettagli in `docs/report/BOOT-1-FDT-HARDENING-2026-11-03.md` e
`docs/report/logs/`. DEV-1/DEV-2/DIAG-1 restano le fasi successive e non sono
state avviate da questo gate.


## 17. BOOT-1 — DTB/FDT handoff, ricerca e ownership PMM (COMPLETATO)

BOOT-1 è stato completato sul kernel legacy esistente: non rimuove Image+DTB/GRUB,
non sostituisce Multiboot2/PVH su AMD64 e non avvia la migrazione a devmgr o ad
altri servizi NEXS-NG.

**Implementato e verificato:** handoff EFI AArch64 x0/x1; BootInfo v1; adapter
legacy x1=0; parser FDT bounded; reservation map, /reserved-memory statico e
DTB RESERVED; region union/overlap/hole handling nel PMM; usable-only VMM map;
ricerca legacy page-by-page entro la finestra originaria da 1 GiB. Il test host
è in `kernel/test/fdt_parser_test.c`; `make test-fdt` genera il DTB QEMU come
input obbligatorio e ha superato 61 controlli ASan/UBSan. I test runtime sono
PASS su AMD64 e AArch64, incluso il ramo legacy x1=0 esercitato da KTEST.

**Restano fuori scope e aperti per fasi dedicate:** discovery generica device,
interrupti e phandle; riserve dinamiche size-based; fuzzing continuo/libFuzzer;
certificazione su hardware fisico. Non sono gate bloccanti per chiudere BOOT-1.

Fonte di dettaglio e risultati: `docs/report/BOOT-1-FDT-HARDENING-2026-11-03.md`.

## 18. DEV-1 — Device discovery catalogue and lifecycle audit (AUDIT COMPLETATO, IMPLEMENTAZIONE NON AVVIATA)

La ricognizione source-level è stata completata su HAL registry, driver binding,
FDT parser, PCI AMD64/AArch64, USB HCD/core e GPU registry. Il report conserva
i riferimenti e distingue codice legacy effettivo, decisioni architetturali
NEXS-NG e lavoro ancora da implementare:

`docs/report/DEV-1-DEVICE-DISCOVERY-AUDIT-2026-11-03.md`

Conferme principali:
- il parser FDT BOOT-1 non esporta ancora nodi device generici; ECAM, finestre
  PCI, slot VirtIO-MMIO e IRQ AArch64 sono ancora configurati da costanti;
- il registro HAL legacy è statico/append-only (64 elementi), senza remove,
  generation o refcount; `hal_device_t` e `struct hal_device` sono due
  descrittori distinti;
- il matching driver legacy è una scansione esplicita chiamata da `usb_init`,
  non un attach/hotplug runtime;
- entrambi i percorsi PCI abilitano BusMaster prima del binding/DMA ownership;
  BAR sizing/mapping non è ancora completo;
- USB runtime disconnect/reconnect e GPU lifetime drain non sono implementati.

**Sequenza DEV-1:** parser FDT generic node/property bounded → compatible,
status, phandle e raw resource tuples → traduzione `reg` tramite `ranges`
validati → decode interrupt parent/cells → host tests ASan/UBSan → confronto
con le costanti QEMU-virt attuali. Il parser produce metadata non privilegiati:
nessuna capability MMIO/IRQ/DMA viene concessa dalla discovery.

DEV-2 (PCI resource ownership/BAR lifecycle) e DIAG-1 (snapshot/eventi device)
restano gate distinti. USB hotplug, GPU refcount/quiescence e il servizio
userland `devmgrd` non vanno accorpati alla modifica parser.


## 19. Input-time SSP / kernel context fault (OPEN — 2026-11-04)

Una sessione interattiva AArch64 ha riprodotto un panic mentre NXShell stava ancora ricevendo caratteri: lo screenshot prova che cat /reg/system.version non è stato eseguito. Non classificare il fault come VFS/registry read. La seriale riporta instruction abort a ELR/FAR zero, PID 32002 (idle CPU2 nella tabella nxproc) e SSP return address 0xffff0000400a0ab4, simbolizzato come sys_registry+0x3b4. Il ramo del syscall e il writer della corruzione non sono noti.

Evidenze complete: docs/report/GFX-INPUT-SSP-FAULT-2026-11-04.md e docs/report/logs/GFX-runtime-fault-aarch64-20261104-001123/. È un evento distinto dal precedente SSP in compositor_window_write().

**Prossimo gate:** aggiungere diagnostica SSP sicura con caller FP/LR e stack bounds, preservare il primo pt_regs, aggiungere trace per-CPU delle transizioni scheduler (prev/next, frame EL0/EL1, stack, pending_reenqueue/deferred reap) e trace input a sequenza limitata. Poi riprodurre digitazione QMP, focus/mouse e registry reads. Non modificare VFS/registry né togliere lock compositor prima di aver identificato il writer. Clean build e test-all AMD64+AArch64 dopo ogni fix.
