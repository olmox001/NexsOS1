# PIANO — Maturità driver/HAL → decoupling → pulizia → chiusura B3

> **Documento di ripresa-contesto.** Leggere questo file per riprendere il lavoro a
> perdita di contesto. È il piano *vivo*: aggiornarlo man mano (stato fasi, decisioni).
> Branch di lavoro: `comprehensive-review`. Data apertura: 2026-06-14.

## 0. Stato sintetico

- **Appena atterrato** (commit `d0791db`, `14f2744`): device-manager + stack USB
  (xHCI/EHCI/UHCI/hub/HID) + input unificato (`input_report`) + uniformità aarch64
  (ECAM PCI + assegnazione BAR). Funzionante su entrambe le arch.
- **In corso**: questo piano (Fase 0 → Fase 6).
- **Bug live risolto in Fase 0**: su UTM (amd64) il boot si impiantava a `PS/2: write
  timeout` perché [ps2.c:112](../kernel/drivers/ps2/ps2.c) aveva un flush loop
  illimitato (`while (inb(0x64) & 1) inb(0x60);`) — senza 8042 il bus legge `0xFF`,
  bit0 sempre 1 → hang. Reso non-bloccante: presence-gate `0xFF`, self-test `0xAA→0x55`,
  loop limitato, wait con stato. (commit `1a3acc4`, issue #124)
- **Puntatore assoluto (DRV-INPUT-01 #125)**: su UTM il cursore (relativo, sia USB che PS/2)
  si inchioda al bordo destro — desync host-assoluto-senza-grab. Introdotto il contratto
  `EV_ABS` normalizzato `[0, INPUT_ABS_MAX]` + scaling→pixel nel compositor + fix del clobber
  d'asse (la sentinella `-1` ora è onorata). **virtio-tablet verificato su QEMU** (scaling
  matematicamente corretto: x=0x7FFF→719, x=0x4000→359, niente azzeramento d'asse).
  **FATTO**: USB-HID assoluto (report-protocol) — `usb_parse_hid` accetta HID non-boot, parser
  del report descriptor (X/Y assoluti + bottoni), handler tablet → `EV_ABS` normalizzato.
  Verificato su QEMU **xHCI (USB3) ed EHCI (USB2)** con `usb-tablet` (parse: X@8 Y@24 16bit max
  32767; decodifica esatta). Verificato anche su **UHCI** (il controller di UTM).
- **USB su UHCI/UTM (DRV-USB-UTM-01 #130)**: il report descriptor va richiesto alla
  **lunghezza esatta** (`wDescriptorLength` dal descriptor HID 0x21) — l'over-request (256B)
  terminava con uno short packet e la control UHCI ritornava -1 (su xHCI/EHCI era tollerato).
  Con il fix il tablet enumera anche su UHCI (`rl=74 abs=1`). Aggiunto anche, in
  `usb_register_hcd`, un **settle + poll bounded** del connect per porta + log `connected=N`
  (UTM/HVF alza il connect in ritardo; il re-scan a runtime è la Fase 2).
- **Freeze mono-core + modello timer per-CPU (SCHED-01 #83)**: con 1 core, aprire un 2°
  programma CPU-bound *focalizzato* freezava tutto lo userspace (shell/init compresi) — il
  focus boost sceglieva il PID focalizzato ad OGNI `schedule()` → monopolio del core; in più
  init/notify busy-spinano (il `sleep()` libc era un yield-spin). Fix in due parti:
  1) **focus boost limitato** (`FOCUS_BOOST_MAX=4` per-CPU; commit `0218414`) — nessun processo
     può bloccare un core, focus boost mantenuto;
  2) **timer software PER-CPU** (lista+lock in `struct cpu_info`, fired dal tick di ogni CPU su
     `jiffies` globale; `t->cpu` ricorda il core) + **sleep bloccante reale** (`SYS_NANOSLEEP`,
     timer per-processo armato sul core corrente, risveglio locale via `enqueue_task`,
     `timer_del` nel teardown anti-UAF; `usleep` in `unistd.h`, `sleep` reale in `lib.c`; init
     usa `sleep(50)`) — commit `00a83e6`. Conforme HAL/ASTRA (arch=tick, core=logica timer).
  Verificato QEMU `-smp 1`: init da 200/400 schedule (spin) a ~0 (dorme); `top` focalizzato
  CPU-bound non freeza più (top 80%, init/notify il loro share). **Follow-up**: `notify_srv`
  busy-spin ancora (applicare `sleep`/`recv` bloccante — file del maintainer).
- **PS/2 mouse drift (DRV-INPUT-01 #125)**: l'handler non sincronizzava il pacchetto → stream
  disallineato → `dx` leggeva il byte sbagliato → cursore trascinato di lato (anche con grab).
  Fix: resync sul bit 3 dello status + drain a fine init + scarto pacchetti con overflow.
  Verificato su QEMU (`sync=1`, dx esatto). Commit `5a68d45`.

## 1. Decisioni del maintainer (vincolanti)

- **DTB fallback su amd64**: *documentare ora, implementare in B4*. Il parser
  `kernel/lib/fdt.c` è arch-neutrale (gated `#ifndef ARCH_AMD64`); amd64 usa Multiboot2 +
  scan PCI CF8/CFC. Implementarlo tocca `kernel/arch/amd64/platform/platform.c` + `start.S`
  (FROZEN fino a B4) → **non** toccare ora; solo doc + issue.
- **Plug-and-play**: *anche hotplug kernel*. Oggi discovery one-shot al boot, registro HAL
  immutabile/lockless dopo SMP. Fase 2 lo rende mutabile a runtime + recognition/dispatch
  userland.
- **Stub PS/2 su aarch64**: *opzione A* — lasciarlo ora; rimuoverlo "per bene" in **Fase 2**
  introducendo un registro di input-provider con auto-registrazione (PS/2 si registra solo su
  amd64; `keyboard_init` non nomina più i driver). Motivo dello stub: `inb`/`outb` esistono
  solo su amd64 (istruzioni x86); `ps2.c` è in sorgenti condivise e `keyboard_init` chiama
  `ps2_init()` sempre → serve un simbolo no-op su aarch64. QEMU `virt` non ha l'8042.

## 2. Risposte ai quesiti (dall'analisi del codice reale)

- **IPC su aarch64 = GIÀ COMPLETO.** `sys_ipc_send/recv/try_recv` in `kernel/sched/process.c`
  (syscall 230/231/233), nessun `#ifdef` arch, IPC-01 (#85) chiuso in `225e294`, enforcement
  `CAP_IPC_ANY`. Unificato e verificato su entrambe le arch. Nessun lavoro funzionale; solo
  eventuale raffinamento messaggistica (Fase 5).
- **Gestione errori non bloccante**: mancava convenzione; Fase 0 (PS/2 esemplare) + Fase 1
  (generalizzazione: helper bounded-wait, audit busy-wait di USB/virtio/pci).

## 3. Vincoli operativi (NON derogabili)

- Commit **solo** su `comprehensive-review`; mai push/merge su `main` (lo fa il maintainer).
- **Chiedere conferma prima di OGNI commit** e prima di cambiare fase.
- `kernel/arch/amd64/platform/platform.c` **FROZEN** fino a B4 — non toccare.
- Mai committare file untracked del maintainer (`*.old`, `busybox/`).
- Testare su **entrambe le arch** ad ogni modifica; toolchain aarch64 pin GCC 7.2.0
  (usare `pr_info("%s", "...")` per stringhe singole).
- Issue GitHub: **aprire** liberamente; **chiudere/spuntare** solo con autorizzazione.
- Niente codice d'esempio: solo implementazioni reali. Rispondere in italiano. Max 2 agent.

## 4. Fasi (esecuzione incrementale; test + conferma-commit per fase)

- **Fase 0 — Stabilizzazione + doc** *(in corso)*: (0.1) questo doc; (0.2) fix PS/2
  non-bloccante ✅ codice fatto; (0.3) aggiornare `docs/MANUAL.md` §8–10 (USB, device-manager,
  input unificato, ECAM aarch64, ramdisk) + nota render/bug-di-disegno (`docs/review/analysis/06-graphics.md`,
  #118) + nota "IPC completo su entrambe le arch"; `docs/review/HANDOFF.md`. Test: boot amd64
  con PS/2 (regressione) + matrice. Issue: aprire "PS/2 bring-up bloccante se controller assente".
- **Fase 1 — Errori non-bloccanti generalizzati + hardening driver** *(FATTA)*: contratto
  `kernel/io_poll.h` (`poll_until`/`spin_until`, macro do/while ISO `-Wpedantic`-safe). Audit
  kernel-wide: gli HCD USB (xhci/ehci/uhci) erano **già** limitati (`for s<N`); bonificati i
  wait TX UART (16550 + pl011) che erano illimitati e adottato il contratto in PS/2. **Nessun
  wait hardware illimitato residuo** (verificato: 0 `while(1)`/`for(;;)`, 0 spin su registro
  senza cap). Boot OK entrambe le arch. **Rinviato** (valore basso — input duplicato raro — e
  rischio comportamentale durante il test PS/2 del maintainer su UTM): `usb_hid_present()` +
  skip PS/2 se USB HID attivo.
- **Fase 2 — Plug-and-play: recognition + hotplug kernel + dispatch userland**: registro HAL
  mutabile a runtime (lock, `hal_register_device_dynamic`, `hal_unregister_device`, refcount,
  `device_driver.remove`, `driver_match_one`); eventi hotplug HCD USB → HAL → IPC verso
  userland; syscall enumerazione registro; servizio userland `devmgrd` (recognition→dispatch);
  **+ rimozione stub PS/2 via registro input-provider (decisione A)**. (sconfina B5/#120)
- **Fase 3 — Decoupling compositor↔process** (#83, #67, #69): lo scheduler non chiama più
  `compositor_get_focus_pid()` ([process.c:1134](../kernel/sched/process.c)); il compositor
  *spinge* il focus a un setter sched sotto lock; `keyboard_focus_pid` reso safe; niente
  chiamate dirette `process_terminate`/`kernel_ipc_send` dal compositor.
- **Fase 4 — Pulizia userland/API morto/legacy** *(chiedere per-file)*: app test/demo,
  backend doom morti, servizi rotti (nxfont #82, regedit #81), stub lib (USR-LIB-04).
  Escludere `*.old` e `busybox/`.
- **Fase 5 — Chiusura B3 reale**: race SMP (GFX-COMP-02, #84 deadlock AB-BA), quote
  (SCHED-DOS-01 #122, per-window/IPC), sicurezza (GFX-FONT-01 #100, LIB-SSP-01 #71), verifica
  capability/4-livelli end-to-end; confine B5 esplicito (namespace/seL4 #95, multiutente #120).
- **Fase 6 — DTB-amd64 (solo doc)**: fattibilità + design + issue; implementazione in B4.

## 5. Seam esistenti da RIUSARE (non reinventare)

- Input: `input_report(type,code,value)` ([keyboard.c:251](../kernel/drivers/keyboard/keyboard.c)).
- GPU: `gpu_register`/`gpu_get_primary` + `gpu_ops` (`kernel/include/drivers/gpu/gpu.h`).
- Device registry: `hal_register_device`/`hal_device_find`/`hal_device_find_class`
  (`kernel/core/hal_bus.c`).
- Driver binding: `driver_register`/`driver_match_all` (`kernel/include/kernel/driver.h`).
- Block: `block_register`/`block_read`/`block_write`. IRQ: `irq_register(vector,h,data)`,
  amd64 vettori PCI = 32 + `pci_get_interrupt(bdf)`.
- IPC: `kernel_ipc_send` / `sys_ipc_*` (`kernel/sched/process.c`).
- DTB: `fdt_init`/`fdt_get_mem_regions` (`kernel/lib/fdt.c`).

## 6. Verifica (end-to-end)

- `make ARCH=amd64 all` e `make ARCH=aarch64 all`; per aarch64 headless serve
  `build/aarch64/virt.dtb`.
- Matrice QMP headless (`/tmp/inputmatrix.py`): amd64 {virtio, PS/2, USB-xHCI, EHCI, xhci-hub},
  aarch64 {virtio, USB-xHCI}. Criterio: boot fino a "supervisor loop", tastiera (`ls`+invio →
  output con `bin`/`fonts`), mouse (eventi `input: type=2/3`), **nessun hang/panic**.
- Regressione chiave Fase 0: la release amd64 (ISO ramdisk) deve bootare su UTM **senza
  impiantarsi** sul PS/2.
