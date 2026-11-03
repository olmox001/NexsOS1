# NEXS-NG — Piano esecutivo integrato: microkernel, hardening, device e grafica

**Data revisione:** 2026-11-03
**Stato:** piano e gate di implementazione; non certifica come completati i componenti non ancora realizzati.
**Baseline:** kernel legacy presente in `kernel/`; target NG in `kernel-ng/` (da inizializzare solo dopo la verifica dei contratti).
**Vincolo:** conservare userland v1 e il boot legacy come riferimento regressivo; nessuna riscrittura massiva.

## 1. Direttiva e fonti di verità

Questo piano unifica e aggiorna, senza cancellare la storia, le decisioni di:
- `docs/ASTRA.md` — livelli, SRL/HAL, OS1low_/OS1_, object/capability/registry;
- `docs/PLAN-2026-07-23-HARDENING-SDK-RUST.md` — programmi A/N/C/D/E/F/G/R;
- `docs/PLAN-2026-07-17-STRATIFICATION.md` — HAL-0, librerie, servizi e call-surface;
- `docs/PIANO-DRIVER-MATURITY.md` — discovery e hotplug runtime;
- `docs/architecture/NEXS-NG-CORE-DESIGN.md` e `NEXS-NG-HARDWARE-AND-ISOLATION.md`;
- `docs/architecture/BOOT-PROTOCOL-INVENTORY.md`;
- `docs/architecture/GRAPHICS-USERLAND-MIGRATION.md`.

In caso di conflitto, valgono le decisioni più recenti e più specifiche, ma la contraddizione deve essere risolta esplicitamente nei documenti originari. Il codice reale prevale sulle dichiarazioni di stato obsolete. Nessuna sezione storica viene cancellata senza conservarne la provenienza.

## 2. Stato reale rilevato e correzioni di interpretazione

| Area | Fatto verificato nel tree | Conseguenza |
|---|---|---|
| Compositor | `kernel/graphics/compositor.c` è ancora compilato nel kernel; PID1 invoca `SYS_COMPOSITOR_RENDER` in contesto processo | “process context” non significa ring-3/userland. La migrazione microkernel resta da fare |
| Object manager | `kobject` refcount + tabella handle per-processo, 32 slot, handle integer slot senza generation visibile nel token | Riutilizzare la base, ma non assumere che risolva stale-handle ABA o oggetti remoti |
| Capability | rights attenuabili; namespace FS/PROC/REG/WIN/PORT; CAP_* ambientale usato per acquisizione | Distinguere rigorosamente privilegi d'acquisizione dai diritti persistenti sul singolo oggetto |
| Registry | albero `reg_node`, mount `/reg`, ACL/ownership e lock propri | Il registry è un namespace/servizio logico esistente, ma non è ancora un discovery protocol uniforme per ogni device/service |
| Syscall | legacy file syscalls già delegano a handle/object; molti verbi process/input/window/device restano percorsi dedicati | Non rinominare soltanto: fare mapping semantico e migrare ogni call site a object/port |
| Device discovery | FDT minimale su AArch64; PCI scan AMD64; HAL registry array fisso e boot-only | nessun hotplug generale; la discovery completa appartiene a devmgr |
| FDT | parser commenta esplicitamente bounds-check mancanti e reservation map non letta | hardening FDT prima di accettare DTB non fidato o usare FDT per risorse dinamiche |
| UEFI | AArch64 ISO EFI + GRUB AA64 + EFI stub embedded kernel/DTB; AMD64 ISO BIOS GRUB | non esiste ancora un percorso UEFI completo e verificato su entrambe le architetture |
| Diagnostica | UART/serial log e QMP input; `nxadb` attuale avvia QEMU headless `display=none` | aggiungere cattura schermo e sessione grafica controllata; seriale resta canale indipendente |
| Build hardening | `-Wall -Wextra -Werror -Wpedantic -Wshadow -Wwrite-strings -Wmissing-prototypes -Wstrict-prototypes`, freestanding, stack protector, frame pointers, O2/g | NX_STRICT è opzionale; aumentare controlli con profili espliciti, non con flag indiscriminati |

### 2.1 Correzione ASTRA §7.10

Il titolo storico “compositor render moved to userspace” descrive il passaggio da IRQ context a process context, non l'estrazione dal kernel. La documentazione deve dire chiaramente:
- rendering fuori dall'IRQ: completato nella legacy implementation;
- renderer ancora kernel-resident: confermato;
- compositor ring-3: obiettivo NG, non completato;
- il servizio `nxui` è window server/dock userland, non il compositor.

Questa distinzione impedisce di saltare la migrazione vera.

## 3. Ordine vincolante dei lavori

Non intrecciare contemporaneamente le riscritture di object ABI, boot protocol, FDT, hotplug e compositor. L'ordine riduce il numero di contratti instabili.

### Track 0 — Baseline, inventory e osservabilità (prima di runtime changes)
1. Congelare una baseline riproducibile: revisioni Git, submodule, toolchain, flags, build logs e boot logs completi per entrambe le arch.
2. Inventariare syscall per syscall: numero, argomenti, user pointer, capability gate, effetti, errori, lock, oggetti coinvolti, caller e wrapper.
3. Confrontare syscall legacy con `OS1low_*`, object verbs, registry namespace e port IPC. Per ogni verb decidere: primitiva kernel, object_ctl/read/write/wait, servizio IPC o wrapper libc.
4. Inventariare tutte le fonti di hardware discovery e i percorsi boot realmente usati, separando artefatti compilati da artefatti eseguiti.
5. Aggiungere test harness e acquisizione screenshot QEMU. Nessun refactor grafico prima che la baseline possa essere osservata.

**Gate T0:** build + boot + shell interattiva entrambe le arch; serial log integrale conservato; screenshot verificato; input QMP verificato; stato QEMU riproducibile.

### Track 1 — Hardening kernel e contratti comuni
1. Eseguire/aggiornare HAL-0 prima di codificare nuove astrazioni: uaccess, timer, context switch, FPU/SIMD, IRQ state, SMP semantics.
2. Rendere obbligatori i contract gates già presenti; aggiungere NX_STRICT e diagnostica statica progressivamente.
3. Correggere parser FDT e validare tutte le strutture boot (bounds, overlap, alignment, string termination, reservation map, overflow).
4. Verificare memory map, alias PA/VA e W^X per AMD64 e AArch64; i claim di hardening vanno certificati per arch separatamente.
5. Completare la diagnostica strutturata senza dipendenza dal compositor.

**Gate T1:** build strict entrambe le arch; unit/host tests parser e contratti; boot serial completo; nessun nuovo warning/error; test negative su input malformati.

### Track 2 — Object runtime, registry e call-surface R
1. Mappare le syscall legacy sui metodi object esistenti; identificare ogni bypass e duplicazione.
2. Verificare lifetime/refcount, lock order, close durante I/O, grant/transfer failure rollback, stale handle e process death.
3. Definire identificatori generazionali/epoch per evitare ABA; non cambiare formato v1 senza adapter e test di compatibilità.
4. Esporre discovery dinamica di tipi, rights, verbs, ABI version, limiti e servizi attraverso un bootstrap minimo + oggetti/registry.
5. Unificare registry, service ports e device namespace: il registry descrive e risolve servizi, la capability trasferita autorizza; un nome non è un'autorizzazione.
6. Migrare un dominio alla volta, iniziando da device enumeration e diagnostica, poi window/surface, input, process e VFS.

**Gate T2:** test cross-process transfer, rights attenuation, revoca, stale generation, port queue full, process/service crash, concurrent close/read/wait; vecchi binari v1 ancora funzionanti.

### Track 3 — Device tree, devmgr e hotplug
1. FDT/ACPI/PCI enumerano hardware e producono un grafo normalizzato, non autorizzazioni.
2. Kernel mantiene solo primitive per IRQ, MMIO mapping, DMA pinning/IOMMU, interrupt delivery, queue/wait/semaphore e revocation.
3. `devmgrd` in userland applica matching/driver policy, carica driver, concede capability minime e supervisiona restart.
4. Hotplug deve essere transazionale: DISCOVERED → PROBING → BOUND → ACTIVE → QUIESCING → DETACHED/FAILED. Ogni transizione ha generation e reason.
5. La rimozione prima disabilita nuove richieste, poi quiesce queue, cancella o drena work, maschera IRQ, ferma DMA e attende ack/fence, revoca mapping/capability, infine libera risorse.
6. Un device non può essere liberato mentre una coda, IRQ, DMA descriptor, callback o client mantiene un riferimento.
7. USB HCD produce eventi; PCI/ACPI/FDT provider notificano cambiamenti; userland riceve eventi attraverso port capability. Per bus non hotpluggable, il rescan è esplicito e bounded.

**Gate T3:** simulare add/remove/reset/timeout con richieste in flight; nessun UAF, DMA after free, IRQ su handler rimosso, queue use-after-free o capability stale; stessa semantica su entrambe le arch.

### Track 4 — Kernel Buffer/Surface Manager (GFX-NG-1)
1. ABI versionata per Buffer, Surface, Commit, Sync/Completion e Display resource.
2. Object types e rights definiti solo dopo audit del token/generation model e dell'object manager.
3. Buffer page-backed, checked arithmetic, mapping per-processo, owner, refcount, pin count, DMA leases, cache policy e revocation.
4. Surface è una vista tipizzata su buffer range con formato/stride/plane e usage; non contiene pixel methods.
5. Commit descrive buffer sequence + damage + generation; non contiene comandi di disegno.
6. Diagnostica per oggetto e per transizione integrata fin dal primo commit.
7. Test ostili: overflow, stride insufficiente, handle sbagliato, rights insufficienti, map alias, concurrent commit, destroy durante present, device unplug in flight.

**Gate T4:** nessuna routine di compositing/raster nel nuovo kernel; zero accessi raw pointer userland dopo syscall; test ASan/UBSan host e runtime dove disponibili; leak/refcount audit.

### Track 5 — Display/GPU service, compositor userland e librerie
1. Prima software scanout lineare e display service con panic console indipendente.
2. Compositord ring-3: scene/window policy, damage, clipping, z-order, focus, scaling, frame pacing e composition.
3. GPU provider ring-3 quando MMIO/IRQ/DMA/IOMMU lo consentono; fallback software.
4. Libos1-gfx e API SDL/Mesa/OpenGL/D3D consumano le stesse capability surface.
5. Broker ABI v1 traduce syscall legacy in RPC/capability operations senza chiamate servizio mentre kernel locks sono tenuti.
6. Estrarre fonts/terminal/chrome e rimuovere i bypass kernel solo dopo compatibilità.

**Gate T5:** compositor e GPU service kill/restart senza kernel crash; multi-window, resize, focus, screen capture, hotplug display, memory pressure e input verificati entrambe le arch.

### Track 6 — UEFI/BootInfo e driver maturity
1. Consolidare BootInfo versionato e adapter per Multiboot2/PVH, AArch64 Image+DTB, UEFI.
2. UEFI AMD64 e AArch64 testati separatamente con firmware QEMU OVMF/AAVMF; verificare Secure Boot solo se esplicitamente supportato.
3. EFI memory map: descriptor size/version, map key, retries ExitBootServices, ownership delle allocazioni, framebuffer GOP e runtime services.
4. Firmware DTB/ACPI selection deve essere esplicita e validata; non preferire blob embedded stale a firmware topology senza controllo di compatibilità.
5. Solo dopo boot protocol stabile: ACPI MADT/MCFG/SRAT, PCI hotplug, USB hotplug e driver AHCI/NVMe.

**Gate T6:** cold boot UEFI/legacy, memory map e framebuffer verificati; logs serial completi; reboot e shutdown; boot failure con diagnosi utile; entrambi gli ISA.

## 4. Modello unificato syscall → object → registry/port

Il modello finale non è “ogni syscall diventa un file”. La regola è:
- syscall low-level solo per bootstrap, VM, thread/process, IPC, interrupt, mapping, wait e capability operations indispensabili;
- risorse nominate da object handles; tutte le operazioni su risorse passano per object methods;
- `object_ctl` per operazioni atomiche tipizzate, non per trasportare intere API non correlate;
- `object_read/write/wait` per I/O e readiness coerenti con il tipo;
- registry/namespace per discovery/configurazione e introspezione;
- port capability per RPC/event delivery; capability grant per trasferire autorità;
- libc/POSIX resta userland e compone le primitive OS1.

La risoluzione di un nome nel registry non concede privilegi: il servizio restituisce o trasferisce una capability esplicita. PID numerici non sono endpoint autorizzativi. I legacy syscall restano adapter fino a migrazione dei caller.

## 5. Surface manager diagnostics e system observability

Ogni oggetto buffer/surface deve avere:
- object ID diagnostico non riutilizzato durante la sessione, generation, type, owner domain;
- dimensioni, format, stride, plane count, range e bytes;
- rights effettivi e provenienza della capability;
- map count, refcount, pin count, DMA lease, active commit e scanout refs;
- sequence numbers: create/map/unmap/commit/complete/revoke/destroy;
- ultimo errore, reason code e timestamp monotonic;
- counters per invalid handle, denied rights, overflow, stale generation, busy, timeout, queue full, map failure, revoke wait;
- high-water marks per bytes, objects, mappings, in-flight commits e queue depth.

Il percorso diagnostico è read-only e capability-gated: `/sys/devices`, `/sys/graphics`, `/sys/objects` o equivalente registry tree. Non esporre physical addresses, kernel pointers o contenuti pixel a utenti non autorizzati. Le metriche non devono richiedere lock globali lunghi né allocazione in IRQ. Event ring bounded con sequence/drop counters.

## 6. Device queues, semaphores e detach safety

Ogni device service/driver possiede bounded request/completion queues. Ogni request porta device generation, request ID, capability, deadline e cancellation token. IRQ handler fa il minimo: ack/mask, push evento preallocato, wakeup; non esegue policy o I/O bloccante.

La rimozione:
1. marca QUIESCING e incrementa generation;
2. rifiuta nuove richieste e notifica client;
3. cancella richieste non iniziate; completa quelle già accettate con status definito;
4. drena/annulla DMA e aspetta completion con timeout;
5. disabilita interrupt source e sincronizza handler/workqueue;
6. revoca mapping/capabilities e attende refcount a zero;
7. chiude code/semafori e libera memoria solo dopo quiescence;
8. pubblica DETACHED e conserva record diagnostico.

Un timeout non autorizza a liberare memoria ancora raggiungibile dal device: si mantiene il device in quarantena e si registra failure, eventualmente reset controllato.

## 7. Build flags e hardening profiles

Non aggiungere flag casuali al build default. Introdurre profili separati e misurabili:
- `debug`: flag attuali + symbols;
- `strict`: `NX_STRICT=1`, `-Werror` invariato, contract/lock checks;
- `sanitize-host`: ASan/UBSan per parser, object model e surface manager host tests;
- `diagnostic`: trace subsystem, allocator/object accounting, lock timing, assertions;
- `release`: invariants di sicurezza obbligatorie; rimuove solo tracing costoso, non checks di bounds/rights.

Valutare e documentare per arch supporto/compatibilità: `-Wconversion`, `-Wsign-conversion`, `-Wformat=2`, `-Wundef`, `-Wcast-align`, `-Wpointer-arith`, `-Wswitch-enum`, `-Werror=return-type`, stack usage reports, linker RWX check, section/relocation audit, disassembly checks. Attivare un gruppo alla volta, correggere i warning reali, poi renderlo gate. Non usare `-Wno-error` per kernel. NX_STRICT oggi opzionale: decidere se renderlo default dopo aver misurato il backlog.

## 8. UEFI and boot diagnostics — checklist non negoziabile

Per ogni run registrare: commit, toolchain, comando QEMU completo, firmware path/version, boot mode, ISO/kernel hash, CPU/RAM/SMP, DTB/ACPI provenance, memory map summary, framebuffer, BootInfo version, device enumeration, scheduler/SMP online, init/service readiness, shell-ready marker, panic/fault signature, exit reason.

Il log deve includere firmware/GRUB/stub e kernel, non solo la coda seriale kernel. Archiviare stdout, stderr, serial e QEMU debug log separatamente e correlati da run ID. Il boot success è il raggiungimento di shell con comando che ritorna output, non la sola riga “kernel initialized”.

## 9. Regole operative

- Esaminare file interi e call graph prima di cambiare contratti condivisi.
- Ogni ipotesi non verificata diventa una domanda di ricerca e una voce documentata.
- Fonti esterne solo per specifiche normative/versioni correnti; fissare revisione e licenza prima di importare codice.
- Un incremento per volta; build e test amd64 + aarch64 dopo ogni cambiamento runtime.
- Test sequenziali quando condividono output intermedi; nessuna build concorrente che scriva lo stesso rootfs/DTB.
- Conservare log, screenshot e report; mai dichiarare “testato” se è stato solo compilato.
- Staging solo di file coerenti e verificati; nessun commit/push senza richiesta esplicita.
- Non cancellare codice legacy o file storici prima che dipendenze, submodule e call sites siano censiti.

## 10. Primo incremento autorizzato dal piano

Il prossimo incremento non implementa ancora il surface manager. Completa il Track 0:
1. strumenti screenshot/QMP e sessione grafica separata dal test headless;
2. raccolta boot log integrale e manifest per entrambe le arch;
3. inventario semantico syscall/object/registry/port;
4. inventario FDT/ACPI/PCI/USB/GPU/console e UEFI per arch;
5. aggiornamento di ASTRA e piani legacy con lo stato reale e i link ai nuovi contratti.

Solo al superamento di T0 si fissa ABI Buffer/Surface/Commit e si entra in Track 4.


## 11. Crosswalk and detailed companion documents

The syscall/object/registry/capability source inventory is
`SYSCALL-OBJECT-CAPABILITY-CROSSWALK-2026-11-03.md`. It records current
types, rights, PID-addressed legacy paths and the existing R1–R6 order. It is
the prerequisite for fixing Buffer/Surface object types and must be updated
from source before ABI numbers are assigned.

The device graph and hotplug lifecycle is specified in
`NEXS-NG-DEVICE-LIFECYCLE-AND-HOTPLUG.md`; observable test harness and
surface diagnostics in `NEXS-NG-DIAGNOSTICS-AND-TESTING.md`.


## 12. Source audit refinements

The device lifecycle document now records specific unsafe legacy assumptions:
PCI enables BusMaster on every discovered function before DMA ownership,
maps a fixed BAR span, and does not size paired 64-bit BARs; USB core explicitly
defers runtime hotplug to Driver Maturity Fase 2; GPU unregister lacks
reference-drain semantics. These are blockers for safe dynamic device services,
not features to carry into NG unchanged.

The boot inventory now records that EFI stub validates only DTB header/total
size and passes pointer-only x0; kernel FDT API lacks available length. Common
BootInfo must pass both physical address and length.

## 13. Newly confirmed priorities from the full boot transcript

Before more graphics/kernel extraction, fix the test observability foundation:
- archive every boot run's serial, QEMU stdout/stderr, build log, command/firmware
  manifest, QMP actions and screenshots;
- remove silent truncation from nxadb's error path;
- add input acknowledgement and window/focus assertions, including nxwins;
- treat UEFI firmware output as a separate channel from guest serial;
- include boot stage/fault identifiers and GFX-TIMING events in one catalog.

The memory report is a 5 GiB FDT usable region at 0x40000000, PMM metadata at
0x40a48000 (41120 KiB), 5070 MiB free. AArch64 HAL_RAM_SIZE remains a stale
2 GiB constant; bootstrap currently maps only 128 MiB plus metadata and then
dynamically maps discovered FDT regions. Audit/remove stale constants and test
non-contiguous regions plus reserved holes. On AMD64 explicitly exercise the
3–4 GiB PCI hole under a 5 GiB guest.

UEFI warnings in the attached transcript precede successful NEXS EFI stub and
kernel milestones. Keep them in the artifact; classify each warning only after
checking ISO EFI PE machine types and boot entry. Do not mark UEFI complete.

The existing nxwins tool is a thin userland client of legacy SYS_WINDOW_ENUM
and OS1_window_focus; the future NG equivalent must use compositor-owned
window capabilities and IPC, not revive kernel window policy.


## 14. Verified implementation progress — 2026-11-03

- nxadb now preserves previous UART/QEMU logs, captures stdout+stderr, writes
  per-run command manifests and has a complete bootlog command. Timeout
  messages label excerpts as partial and retain the full on-disk artifacts.
- Added capture regression tests (archive preservation, manifest, complete
  9KB+ UART and all three output channels).
- Kernel default build now enables NX_STRICT and the additional warning gates
  that were clean on both architectures: -Wundef, -Wformat=2, -Wcast-align,
  -Wpointer-arith, -Wswitch-enum, -Wdouble-promotion, -Wvla and
  -Wmissing-declarations. Conversion/sign-conversion remain a non-fatal audit
  profile because legacy PCI/USB/graphics call sites still need correction.
- BOOT-MEM now logs every region with type/base/size/end and separately
  reports usable sum, usable span and reserved descriptor high-water mark.
  This avoids treating the AMD64 high MMIO aperture as physical RAM.
- Both architectures passed fresh 30-second QEMU tests after the capture and
  memory-log updates: KTEST 8/8, NXShell input, nxwins enumeration/focus,
  screenshots 1280x800, zero fault matches. See NEXS-NG-DIAGNOSTICS-AND-TESTING.md
  and build/runs/{amd64,aarch64}.

## 15. Next implementation sequence (do not reorder security prerequisites)

### BOOT-1 — bounded BootInfo and DTB ownership (highest priority)
1. Extend the AArch64 handoff to pass DtbSpan in x1; preserve it through
   start.S/kernel_main and expose a typed boot-info object with physical base
   and length. Keep an explicitly labelled legacy adapter only where a
   bootloader truly cannot supply a length.
2. Make fdt_init(base,length) validate magic, totalsize, header version,
   structure/string/reserve offsets and every arithmetic operation.
3. Convert all structure walks to bounded cursors; no strlen or property-name
   lookup may escape the DTB span. Bound CPU count and cell widths.
4. Parse the FDT reserve map and /reserved-memory ranges as RESERVED, append
   the DTB allocation itself as RESERVED, and merge/validate overlaps before
   PMM sees the region list.
5. Test malformed/truncated DTBs, offset overflow, missing terminators,
   oversized cells, overlapping ranges, reserved overlays and non-contiguous
   RAM. Confirm PMM never allocates a DTB/reserved PFN.

### DEV-1 — typed device tree and resource catalogue
1. Add a normalized device descriptor independent of FDT/ACPI source:
   stable instance ID + generation, bus/type, compatible/protocol IDs,
   MMIO/PIO ranges, IRQ spec, DMA mask/IOMMU domain, clocks/resets, status,
   parent/child topology and hotplug capability.
2. FDT/ACPI parsers become discovery providers only; they do not initialize
   arbitrary drivers. Device manager validates and publishes descriptors.
3. Split architecture/ISA primitives, bus protocols, device drivers and
   generic service interfaces into separate ASTRA layers.
4. Runtime connect/disconnect is a state machine: DISCOVERED → RESOURCED →
   BOUND → ACTIVE → QUIESCING → REVOKED. Each instance has generation/refcount;
   queues/semaphores have bounded depth, explicit backpressure, cancellation,
   drain and timeout semantics.
5. Disconnect order: stop new requests, signal service, cancel queued work,
   wait for in-flight IRQ/DMA/IPC refs, mask IRQ, revoke MMIO/DMA mappings,
   release pages/resources, then increment generation. No stale handle may
   address a replacement device.

### DEV-2 — protocol and device simulation matrix
- PCI: BAR sizing (32/64-bit), alignment/overflow/overlap, no BusMaster before
  DMA domain setup, IOMMU on/off, MMIO hole and hot-unplug under load.
- VirtIO: separate transport (PCI vs MMIO) from device protocol; validate
  descriptor/ring bounds, queue ownership, IRQ and reset lifecycle.
- USB/HID: connect/disconnect, endpoint cancel/drain, keyboard/mouse report
  queues, overrun and unplug during transfer.
- Display: virtio-gpu device protocol separated from generic GPU service;
  CPU software raster/scanout provider as a real independent backend, not an
  ISA-specific rendering path.
- Test multiple QEMU CPU models, RAM sizes, SMP counts, PCI layouts and both
  architectures; later repeat on physical hardware.

### DIAG-1 — central diagnostic catalogue
Keep fatal faults distinct from performance events. Define stable subsystem
and event IDs, severity, boot phase, CPU, device instance+generation,
timestamp, correlation ID, and recovery action. GFX-TIMING render_hold above
the frame budget is a performance warning, not a kernel fault; aggregate
histograms/maxima and rate-limit reports instead of flooding UART. Every
driver/service must expose state transitions, queue high-water marks, drops,
timeouts, reset/recovery and disconnect completion.

### GFX-NG — graphics migration
Continue GFX-NG-1 buffer/surface capability objects only after BOOT-1 and
object/VM lifetime audits. Compositor, window manager, CPU renderer, fonts,
terminal and GPU policy are ring-3. Kernel surface manager remains mechanism
only; GPU transport/driver remains a device service where hardware isolation
permits. Preserve the legacy syscall broker until compatibility tests pass.
