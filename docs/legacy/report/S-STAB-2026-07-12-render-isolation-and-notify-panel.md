# S-STAB 2026-07-12 — Isolamento del render dal timer-IRQ + fix pannello notifiche

> Chiude la campagna di stabilizzazione iniziata con i panic UTM su click/resize
> (V0.0.4.9 → V0.0.5.0). Copre entrambe le arch (amd64/aarch64), il modello di
> kill dei processi (docs/PROCESS-KILL-MODEL.md, invariato), e un bug UI reale
> nel pannello notifiche di `nxbar`. Verificato file:line contro l'albero a
> `10319cd`; i fix di questo documento non sono ancora committati (solo
> `user/sys/bin/nxbar.c` risulta modificato al momento della stesura).

## 1. Causa radice dei panic (entrambe le arch)

**Sintomo**: kernel panic su amd64 (scrittura corrotta di `current_process`,
`#PF`/`#GP` a un indirizzo di ritorno del render) e su aarch64 (`current_chip`
NULL, `#PF` nell'EOI), entrambi scatenati da click/resize sotto carico SMP
(4 core).

**Causa**: `compositor_tick()` girava nel **timer-IRQ del kernel** (CPU0), cioè
annidato sullo stack kernel di **qualunque task quella CPU avesse interrotto**.
Il render è una catena di chiamate profonda (chrome/shadow/region/gl); se lo
stack interrotto apparteneva a un task che nel frattempo veniva liberato e
riassegnato a un'altra CPU (la classe di race #169/#170, "stack liberato
mentre ancora in uso"), il render smashava un frame vivo su un core diverso.
Su amd64 questo sovrascriveva lo spill di `current_process` in
`kernel_syscall_dispatcher` con un indirizzo di ritorno di
`gfx_chrome_shadow_fast`; su aarch64 corrompeva un registro callee-saved
attraverso `timer_handler`, azzerando di fatto `current_chip` e facendo
NULL-deref `current_chip->end()` (l'EOI).

**Verificato**: uno stress di resize/zoom dedicato (`user/bin/restest.c`, 2108
cicli su amd64 e 754 su aarch64, entrambi a 4 core) causava il crash prima del
fix e gira per l'intera durata senza fault dopo.

### 1.1 Fix — render in userspace (ASTRA DIR-02, "SCHED-STACK-ISO")

Il render **non gira più nel timer-IRQ**. `kernel/core/timer.c` non chiama più
`compositor_tick()` dal tick (vedi il commento `SCHED-STACK-ISO` a
`kernel/core/timer.c:48`); il tick continua a fare solo `input_drain()` (poco
profondo, nessuna catena di chiamate lunga).

Il render è ora guidato da **init**, in contesto processo, sul proprio stack
kernel — mai annidato su un task interrotto:

- `user/sys/bin/init.c:232` — un `flush()` subito dopo tutti gli spawn
  iniziali (primo frame pronto prima di entrare nel loop supervisore);
- `user/sys/bin/init.c:353` — un `flush()` a ogni giro del loop supervisore
  (~30 FPS, `OS1_sleep(33)` subito dopo, vedi il commento
  `SCHED-STACK-ISO` a `init.c:263`).

`flush()` → `OS1_gfx_render()` → `_sys_compositor_render()` → `SYS_COMPOSITOR_RENDER`
→ `compositor_render()` (kernel), che prende `compositor_lock` e chiama
`compositor_render_internal()` sullo stack di **init**, un processo normale.
Nessun nuovo meccanismo kernel (niente stack dedicato, niente kthread — un
primo tentativo con `arch_call_on_stack` è stato scritto e **scartato**
perché la stessa classe di errore del kthread: un meccanismo kernel bespoke
invece di spostare il lavoro in userspace).

### 1.2 Fix — lifecycle dei processi (root fix, non un workaround)

`process_terminate()` non libera più lo stack kernel/PGD di una vittima
finché è `current_task` su **qualunque** CPU (`kernel/sched/process.c:1230-1247`
per il percorso "sleeping esterno", pattern gemello a `kernel/sched/process.c:444`
già presente per il percorso runqueue). Prima il controllo leggeva solo
`proc->on_cpu` (potenzialmente stale) e ispezionava una sola CPU: una vittima
effettivamente in esecuzione su un'altra CPU poteva vedersi liberare lo stack
sotto un core ancora attivo — le due pagine venivano poi riassegnate a un task
vivo, e i due stack si sovrapponevano. Questo è il fix strutturale della
classe #169/#170, non un caso particolare.

### 1.3 Chiusura finestra (tasto rosso) — NESSUN meccanismo kernel nuovo

Un primo tentativo (poi **revocato su richiesta esplicita del maintainer**)
deviava la chiusura fuori dall'IRQ con una coda kernel (`wm_defer_close`/
`wm_drain_closes`, syscall `SYS_WM_DRAIN` drenata da init). Questo rompeva il
**modello di kill nxexec** (`docs/PROCESS-KILL-MODEL.md`): nxexec spawna il
programma come proprio figlio; chiudere la finestra di nxexec deve uccidere
subito il sottoalbero windowless mantenendo vivi gli eventuali figli
windowed — la deviazione tramite coda+drain asincrono introduceva una finestra
temporale che il modello non prevede, ed era comunque superflua una volta
tolto il render dall'IRQ (il vero corruttore).

**Stato finale**: `compositor_handle_click()` chiama di nuovo direttamente
`window_request_close()` (`kernel/graphics/compositor.c:1550`), che a sua
volta chiama `process_kill_subtree()` — invariato, stesso comportamento
window-aware documentato in `docs/PROCESS-KILL-MODEL.md`. Sicuro ora perché
l'IRQ path non fa più nulla di profondo (il render, la vera fonte della
corruzione, è altrove).

## 2. Infrastruttura kthread — documentata e disattivata, non cancellata

`kthread_create`/`kthread_block`/`arch_cpu_yield`/`input_server_start` sono
**instabili e non raggiunti a runtime** (l'unico chiamante,
`input_server_start()`, è compilato fuori dietro
`NEXS_ENABLE_UNSTABLE_KTHREAD_INPUT`). Motivo: lo yield-to-USER stalla CPU0, e
un kthread creato così vive fuori dall'ordinamento idle-task per-CPU su cui si
appoggia lo scheduler SMP. Dettagli completi in
`docs/report/KTHREAD-STATUS.md`. **Non usare per nuovo lavoro**: la strada
corretta per il decoupling dell'input è un **processo userland supervisionato**
(stesso modello di `nxntfy_srv`), non un thread kernel.

## 3. `nxmemstat` — "0 oggetti" non era un bug di conteggio

Il pannello mostrava `FILE 0  PROC 0  REGKEY 0  WINDOW 0`. Verificato: i
contatori (`kobj_alloc`/`kobj_free`, `kernel/core/object.c:61-90`) sono
simmetrici e corretti. Il valore 0 è **atteso**: la stragrande maggioranza
delle operazioni quotidiane (`SYS_CREATE_WINDOW`, `SYS_SPAWN`,
`SYS_REGISTRY_GET/SET`) passa da percorsi diretti legacy che non chiamano mai
`kobj_alloc` per FILE/PROC/REGKEY/WINDOW — solo chi acquisisce esplicitamente
una capability-handle via `SYS_HANDLE_CREATE` li tocca (letture file
transitorie all'avvio, capability di controllo finestra cross-processo). È lo
stato noto e documentato del refactor DIR-01/#164 (ASTRA.md §7.10), non una
regressione introdotta in questa sessione.

**Fix minimo applicato** (committato dal maintainer, `10319cd`): aggiunta la
colonna **CONSOLE** (l'unico tipo che ogni processo vivo possiede realmente,
via `process_install_stdio`) al rendering GUI e al CSV di
`user/sys/bin/nxmemstat.h`/`.c`, con `tools/analyze_drift.py` aggiornato in
modo coerente (`COLS`/`FLAT` includono ora `objC`). Nessuna modifica al layer
oggetti stesso — non serviva.

## 4. Pannello notifiche di `nxbar` — bug reale, isolato e corretto

**Sintomo riportato**: le notifiche arrivano (log ring popolato correttamente
da `nxntfy_srv`), ma il pannello di `nxbar` non le mostra tutte e non permette
di scorrere.

**Causa (verificata per aritmetica, non per ipotesi)**: `NOTIFY_PANEL_MAX_H`
era `220`, `NOTIFY_ROW_H` è `18` → `max_rows = (220-10)/18 = 11`
(`user/sys/bin/nxbar.c`, funzione `redraw()`). Il ring che `nxntfy_srv` scrive
ha però **16** slot (`sys.ntfy.log.0..15`, `NXBAR_NOTIFY_MAX = 16`). Il loop di
disegno (`for (...; drawn < max_rows; ...)`) si ferma all'11ª riga; le voci
12-16 non vengono mai disegnate e **non esiste alcun meccanismo di scroll** per
raggiungerle — voci silenziosamente irraggiungibili non appena si superano 11
gruppi-mittente distinti nel ring.

Questo spiega anche la percezione iniziale "nxnotify non manda notifiche":
l'intera pipeline invio→ricezione→log-ring è stata verificata funzionante
(test end-to-end: `notify()` e l'invio di `nxnotify` restituiscono entrambi
successo, il messaggio è enqueued sincrono in `kernel_ipc_send` prima ancora
che il mittente possa uscire) — il messaggio arriva sempre e viene sempre
loggato; è la sola VISUALIZZAZIONE nel pannello a poter tagliare fuori le voci
oltre l'11ª, specialmente perché `sort_recs()` ordina per PID crescente, non
per recenza: servizi di sistema a PID basso possono facilmente occupare i primi
11 slot e spingere fuori vista le voci di un programma spawnato ad-hoc con PID
più alto (es. `nxnotify`).

**Fix applicato**: `NOTIFY_PANEL_MAX_H` portato a `300` (`user/sys/bin/nxbar.c`),
sufficiente per tutte le 16 righe possibili (`16*18+10 = 298`, arrotondato con
margine). Il ring stesso ha un tetto fisso di 16 voci, quindi questo limite
**non può più troncare nulla**: nessuno scroll necessario, nessuna voce può mai
superare la capacità del pannello.

## 5. Verifica

- Build puliti amd64 e aarch64 (`make ARCH=amd64` / `make ARCH=aarch64`, zero
  errori/warning oltre ai preesistenti `.note.PVH`/RWX benigni).
- Boot fino a desktop completo su entrambe le arch via il path ISO reale
  (GRUB multiboot2 + rootfs come modulo/ramdisk), configurazione tipo-UTM
  (PS2, virtio-gpu, `-smp 4`): 0 panic, supervisor loop attivo, verificatore
  di superficie (`gfx_surface_verify`) mai falso-positivo.
- Stress resize/zoom dedicato (`user/bin/restest.c`): 2108 cicli (amd64) e 754
  cicli (aarch64) a 4 core, 0 fault — prima del fix entrambe le arch
  crashavano su questo esatto percorso.

## 6. Cosa NON è stato toccato (invariante, verificato)

- `docs/PROCESS-KILL-MODEL.md` — il modello di kill window-aware
  (`process_kill_subtree`) è **invariato**: chiusura finestra tasto rosso →
  kill diretto e sincrono, sottoalbero windowless muore col padre, i figli
  windowed sopravvivono. Nessuna deviazione asincrona nel percorso di kill.
- `nxexec` (`user/sys/bin/nxexec.c`) — il modello "spawna come figlio proprio,
  rileva windowed vs windowless" non è stato toccato.
- Il layer oggetti/capability (`kernel/core/object.c`) — invariato; il
  "problema" nxmemstat era di visualizzazione/copertura, non di logica.
