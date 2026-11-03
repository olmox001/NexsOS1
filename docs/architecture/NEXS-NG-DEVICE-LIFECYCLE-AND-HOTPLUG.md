# NEXS-NG — Device graph, discovery e hotplug sicuro

**Data:** 2026-11-03 · **Stato:** target architecture + audit findings.
**Piani da riusare:** `PIANO-DRIVER-MATURITY.md`, Programme G in `PLAN-2026-07-23-HARDENING-SDK-RUST.md`, `NEXS-NG-HARDWARE-AND-ISOLATION.md`.

## 1. Decisione

FDT, ACPI, PCI, USB e firmware descrivono hardware e relazioni; non concedono accesso. Il kernel NG mantiene primitive di sicurezza (MMIO, IRQ, DMA/IOMMU, pinning, revoca, queue/wait/semaphore). `devmgrd` in userland mantiene il grafo, applica matching e policy, avvia/riavvia i driver e pubblica eventi ai servizi.

Il grafo è una vista normalizzata, non un duplicato delle tabelle firmware. Ogni proprietà mantiene provenienza (FDT/ACPI/PCI/USB/driver), confidence, versione e stato di validazione. Le risorse non validate non possono diventare capability.

## 2. Audit del codice legacy

### FDT
`kernel/lib/fdt.c` dichiara di parsare soltanto memory e CPU count. Il file stesso elenca:
- LIB-FDT-01: header/offset/size dei blocchi non validati prima del walk;
- LIB-FDT-02: reservation map ignorata;
- LIB-FDT-03: AMD64 stub esplicito;
- LIB-FDT-04: ricerca in RAM valida solo il magic;
- string offset e terminatori non validati in modo completo.

L'attuale `fdt_init` dereferenzia header prima di verificare che l'intera struttura rientri in una regione accessibile. `fdt_get_mem_regions` usa off_dt_struct/size_dt_struct e string offsets senza bounds checks completi. Il kernel non deve passare da questo parser a discovery completa prima di risolvere i problemi.

### HAL registry
`kernel/core/hal_bus.c` è un array flat `hal_devices[]`, MAX 64, senza rimozione; il commento dice che registrazione è solo pre-SMP e non lockata. Questo non è un device manager runtime.

### PCI/USB/driver binding
PCI enumeration e USB HCD/HID sono implementati nel kernel legacy; il piano PIANO-DRIVER-MATURITY ha già stabilito Fase 2: registro mutabile, register/unregister dynamic, refcount, remove callback, driver matching e hotplug events. Programme G5 lo richiama. Riutilizzare questi nomi e questa direzione, ma reimplementare nel nuovo stack con capability, non portare il registro globale kernel legacy così com'è.

### UEFI/boot
Il DTB embedded nel loader AArch64 è attualmente prodotto dalla configurazione QEMU e incorporato nell'EFI stub; non è prova che il firmware DTB di una board reale venga selezionato correttamente. BootInfo deve conservare provenance, physical range e byte length validati.

## 3. Normalized Device Graph schema

Ogni nodo:
- `device_id`: ID stabile per sessione (non riutilizzato);
- `generation`: incrementata a ogni detach/rebind/reset;
- `parent_id`, bus type, location (BDF, FDT path, USB port path);
- vendor/product/class/subclass/protocol + compatible strings;
- resources: MMIO range, I/O ports, IRQ/MSI, DMA mask, IOMMU group, memory regions;
- dependencies: clocks, reset, power domain, interrupt parent, bridge/bus;
- driver binding: candidate, selected, service endpoint capability, version;
- lifecycle state, error/reason, discovery source, confidence;
- timestamps monotonic and transition sequence.

Device ID non è una capability. L'accesso alle risorse richiede capabilities distinte e attenuabili.

## 4. FDT parser hardening — prerequisites

1. Ricevere `(base, available_length)` da BootInfo; mai inferire la lunghezza da una pointer-only API.
2. Verificare magic, totalsize, version/last_comp_version, header size, alignment e overflow.
3. Validare offset+size di struct/string blocks e reservation map con checked arithmetic; nessuna regione può uscire da totalsize.
4. Ogni token e payload deve stare nel structure block; stringhe NUL terminate entro i limiti; nameoff entro strings block.
5. Validare profondità bilanciata, FDT_END, token sconosciuti, dimensione e numero massimo di nodi/proprietà.
6. Leggere reservation map terminata da (0,0); unire riserve con firmware memory map e boot modules prima di PMM.
7. Validare `#address-cells/#size-cells` (valori ammessi e profondità), tuple reg, ranges, interrupts, phandle e parent references.
8. Memorizzare snapshot normalizzato in memoria kernel-owned, poi non consultare il blob originale dopo che boot memory è liberata.
9. Host fuzz harness con corpus valido QEMU e casi troncati, overflow, offset alias, string unterminated, token malformed, nesting abuse, overlapping memory/reserved.
10. AArch64 early boot non usa RAM scanning se un boot protocol valido deve passare DTB pointer+length; fallback legacy va isolato e rimosso solo dopo compatibilità verificata.

Non importare libfdt senza prima fissare revisione, licenza per ogni file, configurazione freestanding e confronto dei parser test.

## 5. ACPI / PCI / USB discovery

- ACPI: validare RSDP/XSDT checksums, table length, physical mapping, overlap e revision. MADT/MCFG/SRAT/PPTT/GTDT/DMAR/IVRS/IORT solo quanto necessario; AML policy in servizio userland.
- PCI: enumerazione bus/bridge, BAR sizing con device quiesced, resource conflicts, MSI/MSI-X routing, bridge hotplug e rescan. Non assumere che la scansione iniziale equivalga a hotplug.
- USB: hub port status change → HCD event → device enumeration transaction → HID/interface matching → service event. Detach cancella endpoint transfer e attende completion.
- Display: enumerazione connector/mode, hotplug event e ownership scanout; non inferire il display attivo solo dal primary GPU.
- Input: tastiera/mouse/touchpad/tablet sono device separati e possono essere collegati/rimossi indipendentemente; inputd mantiene per-device state e annulla pressed keys/buttons al detach.
- Terminali: serial UART, virtual console e terminali userland sono endpoint differenti. La console panic non dipende da device graph o compositord.

## 6. Driver lifecycle e sicurezza hotplug

Stati:
`DISCOVERED → PROBING → BOUND → ACTIVE → QUIESCING → DETACHED`
con `FAILED` e `QUARANTINED` raggiungibili da ogni fase.

### Attach
1. devmgr crea record e generation.
2. matcha driver dichiarativo (vendor/device/class/compatible + ABI).
3. kernel valida risorse e crea capability separate MMIO, IRQ, DMA, power/reset.
4. driver riceve capability minime e avvia probe in userland.
5. probe restituisce port/service endpoint; devmgr verifica protocol/version e pubblica ACTIVE.
6. client riceve un object/service capability, non il PID driver.

### Runtime operation
Ogni request include device generation, request ID, deadline, operation code e cancellation token. Queue bounded per-device; backpressure esplicita. Semaphore/wait queue devono avere wakeups senza lost wakeup: sequence counter, predicate recheck sotto la stessa disciplina di lock e cancellation generation.

IRQ handler: acknowledge/mask, produce evento in ring preallocato, wake driver. Nessuna allocazione dinamica, RPC, rendering o attesa bloccante in IRQ context.

### Detach
1. mark QUIESCING + generation++ atomico;
2. impedire nuovi request e invalidare capability di submit;
3. broadcast DEVICE_REMOVING su port; client deve cessare nuovi accessi;
4. cancellare queue non iniziate, attendere o cancellare operation in-flight;
5. completare/cancellare DMA e attendere device idle; reset/quarantine in timeout;
6. disabilitare IRQ, sincronizzare handler e work items;
7. revocare MMIO/IRQ/DMA mappings, chiudere endpoints e semafori;
8. attendere refcount a zero per mapping, queue, request, callback, object handle;
9. liberare risorse e pubblicare DETACHED.

Se il device non conferma DMA idle, il backing buffer resta pinned e il dispositivo resta quarantined; non si libera per “sbloccare” il sistema.

## 7. Queues e sincronizzazione

- bounded MPSC/SPSC secondo produttori reali; non usare lock-free per moda se un mutex/lock semplice basta;
- request queue e completion queue distinte;
- sequence number monotonic per queue, generation per device;
- semaforo/event count associato alla predicate queue-not-empty;
- cancellation e timeout sempre rappresentati come completion reason;
- queue full restituisce backpressure (EAGAIN/ENOBUFS) e non perde capability transfer;
- interrupt moderation e batching solo dopo misure;
- per ogni queue: depth, high-water, enqueue/dequeue, drops, timeout, cancel, service latency.

## 8. Device diagnostics

Esportare snapshot read-only:
`/sys/devices/<id>/info`, `state`, `resources` (redacted), `driver`, `stats`, `events`.
I client ordinari vedono classe, stato e driver; dettagli MMIO/IRQ/DMA solo a capability diagnostica machine-level. Event ring bounded con dropped counter. Ogni transizione conserva reason code e timestamp.

## 9. Test plan

Host parser tests: FDT valid/invalid/fuzz, ACPI checksums/lengths, PCI resource overflow. Runtime QEMU: virtio input/gpu/block attach, device hot-unplug dove supportato, USB hub connect/disconnect, driver process kill/restart, queue saturation, DMA timeout/quarantine, client death durante operation, reconnect con generation incrementata.

Acceptance su entrambe le arch:
- nessun stale device capability dopo detach;
- nessun IRQ handler chiamato dopo unregister;
- nessun DMA use-after-free;
- ogni request termina una volta sola;
- nessun deadlock su queue/semaphore close;
- rescan idempotente e bounded;
- shell e console serial continuano a funzionare quando GPU/input service crashano.


## 10. Additional legacy source findings — PCI, USB and GPU hotplug readiness

### PCI resource discovery is not safe to promote directly

Current `kernel/drivers/pci/pci.c`:
- scans the full 256×32 bus/device space, but has no hotplug rescan/event path;
- `pci_get_bar_size()` probes only the low 32-bit BAR word and does not handle
  a full 64-bit BAR size; `bar_index` is not validated;
- `pci_register_one()` maps a fixed 0x20000-byte window instead of the
  discovered BAR extent;
- `pci_set_mmio_windows()` and `pci_alloc_mmio()` have no overflow,
  alignment/power-of-two or concurrent-allocation protection;
- most importantly, `pci_register_one()` sets command bits IO|MEM|BusMaster
  (`cmd | 0x7`) for every discovered function before a bound driver has
  established a DMA contract or IOMMU mapping.

NG must not inherit this behavior. Enumeration may read config space, but
resource assignment, decode enable and bus mastering are separate state
transitions. Enable bus mastering only after the device driver owns the
resource capability and DMA mappings/IOMMU domain are configured. BAR size,
type, prefetchability, 64-bit pairing, alignment, overlap and overflow must
be validated before MMIO capability creation.

### USB is boot enumeration, not runtime hotplug

`kernel/drivers/usb/usb_core.c` explicitly states at the root-hub scan that
runtime hotplug/re-scan is Fase 2. The current HCD ops expose
`enumerate_port`, but no general remove/disconnect/quiesce callback. The
xHCI/EHCI/UHCI code resets/enumerates ports at controller initialization; it
does not yet provide a complete generation/refcount/endpoint-cancel lifecycle.

Therefore a connected keyboard/mouse can be enumerated at boot, but the
current tree does not establish safe USB disconnect/reconnect while transfers
are in flight. Do not describe current USB support as hotplug-safe.

### GPU registry lifetime is also legacy-only

`gpu_core.c` has a raw linked list and primary pointer. `gpu_unregister()`
unlinks the device but has no reference drain; `gpu_get_primary()` returns a
raw pointer without a lifetime token; unregister does not call `ops->destroy`.
This cannot underpin concurrent display unplug. NG must use device/surface
capabilities, device generation, in-flight commit/DMA refs and explicit
quiescence before resource destruction.

### Required source changes before NG device services

1. Replace PCI's unconditional BusMaster enable with a driver-owned transition.
2. Implement complete BAR sizing (including paired 64-bit BARs) and bounded
   mapping of the exact resource extent.
3. Add USB connect/disconnect event processing and HCD remove/cancel/drain ops.
4. Add GPU device reference/lifetime semantics or remove kernel GPU registry
   from the NG architecture entirely.
5. Build a virtual hotplug test with in-flight input and GPU requests; QEMU
   unplug is not considered safe until queue, IRQ and DMA quiescence is proven.
