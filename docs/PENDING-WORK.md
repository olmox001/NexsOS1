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
- **Fatto:** solo il pilota `sleep → OS1_sleep` (commit `9a01bfe`), su tutti i caller (escluso nexs-fm sospeso).
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

## 8. nexs-fm — **PARCHEGGIATO/INCOMPLETO**
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
- **Userland:** #123 ereditarietà stdout/TTY · #76 init.cfg · #81 regedit recv · #82 fontman.
- **Sched/arch/mm:** #84 AB-BA lock · #38 CPU-AMD64-01 FPU su preempt · #122 quota figli
  (fork-bomb) · #39/#40/#41/#43 TOCTOU uaccess · #30/#31/#32 ARCH/HAL boot · #20 PMM multi-region ·
  #72 registry (#71 SSP ✓) · #106 doom fire button.

> Questo registro va aggiornato man mano che gli item vengono chiusi.
