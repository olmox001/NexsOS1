# NEXS-NG — Diagnostica, test reali e osservabilità visuale

**Data:** 2026-11-03 · **Stato:** specifica e piano strumentazione.

## 1. Obiettivo

Un test è valido solo se si può ricostruire cosa è successo e, quando il test riguarda la GUI, vedere il risultato. Il sistema di verifica deve poter:
- avviare e fermare guest amd64/aarch64 in modo deterministico;
- inviare input tastiera/mouse attraverso QMP;
- acquisire screenshot QEMU e conservarli accanto al log seriale;
- salvare serial, stderr QEMU, trace e manifest della configurazione;
- inviare comandi alla shell guest e verificare output/exit status;
- correlare screenshot, input e log con un run ID e timestamp monotonic.

## 2. Stato strumentazione esistente

`tools/nxadb.py` fornisce sessioni per arch, QMP keyboard input, shell, push/pull, logcat e test. I log seriali vengono salvati in sessione; il comando `start_qemu()` configura QEMU con `display="none"`, quindi la sessione automatizzata attuale è headless e non è una prova visiva della GUI.

`tools/nxqemu.py` già accetta `--display`, `--qmp`, `--serial` e `--debug`. Non esiste ancora una funzione pubblica `screenshot` in nxadb, né un ciclo di test che acquisisca e verifichi l'immagine.

## 3. Estensione strumenti host

### 3.1 Screenshot QMP
Aggiungere a nxadb un comando `screenshot [path]`:
1. verificare che la sessione QEMU della stessa arch sia attiva e che il QMP socket appartenga al PID/sessione gestita;
2. inviare il comando QMP `screendump` con path assoluto sotto `build/runs/<run-id>/screens/`;
3. controllare la risposta QMP e che il file esista, abbia dimensione non nulla e header immagine valido;
4. registrare path, dimensioni, formato e timestamp nel manifest;
5. esporre screenshot con `read_file`/viewer del file remoto, non assumere che il nome file equivalga a un'immagine valida.

Il comando deve rifiutare path fuori dalla directory progetto e non deve mai sovrascrivere screenshot precedenti senza un'opzione esplicita.

### 3.2 Sessione visuale
Aggiungere una modalità visuale separata dal test headless. Può usare `nxqemu.py --display default` con QMP attivo, mantenendo la stessa sessione e lock per arch. Il test visuale non deve avviare un secondo QEMU sulla stessa disk image mentre nxadb headless è attivo.

QMP screendump e display visibile sono due funzioni diverse: screendump consente la cattura automatica, display visibile permette all'operatore di interagire. Entrambe vanno verificate sulla versione QEMU effettivamente installata.

### 3.3 Mouse e tastiera
Estendere il controllo QMP con eventi mouse assoluti/relativi, click e release espliciti. Ogni sequenza deve rilasciare tutti i tasti/bottoni anche su errore. Il test deve registrare l'input inviato e attendere l'evento/output atteso, non solo il tempo trascorso.

## 4. Run bundle

Ogni test crea un bundle immutabile:
```
build/runs/<UTC-run-id>/
  manifest.json
  host/
    qemu-command.txt
    qemu-version.txt
    toolchain.txt
    git-status.txt
    git-revision.txt
  logs/
    firmware-grub.txt
    serial.txt
    qemu-stderr.txt
    qemu-debug.txt
    test-output.txt
  screens/
    boot-grub.png
    kernel-init.png
    shell-ready.png
    gui-idle.png
    gui-interaction.png
  traces/
    kernel-trace.bin
    surface-events.jsonl
    device-events.jsonl
  reports/
    summary.md
```
Non tutti i boot path producono ogni file; manifest riporta `captured`, `unsupported`, `not-reached` e motivo. Mai creare file vuoti spacciandoli per acquisiti.

## 5. Boot readiness e log completo

Separare i marker:
- firmware/bootloader entry;
- EFI stub start, kernel image loaded, ExitBootServices success (UEFI);
- kernel entry e protocol source;
- BootInfo/FDT/ACPI validated;
- memory manager online;
- IRQ/timer/SMP online;
- device discovery complete;
- root filesystem mounted;
- system_init/init started;
- graphics/input services ready;
- NXShell prompt ready;
- shell command round-trip success.

Il test automatizzato invia un comando banale e verifica output marker. La stringa “Kernel initialized successfully” non è sufficiente. I log devono conservare tutto il boot, senza tail-only.

## 6. Surface Manager diagnostic contract

### 6.1 Counters
Per tipo e per owner domain:
- live buffer/surface objects, create/destroy, peak live;
- allocated bytes, mapped bytes, pinned bytes, DMA leased bytes;
- mappings per process, duplicate aliases e permission faults;
- refcount high-water, outstanding commits, active scanout refs;
- commit submitted/completed/rejected, queue depth/high-water, latency histogram;
- invalid capability, wrong type, denied rights, stale generation;
- arithmetic overflow, invalid stride/format/range, unsupported plane;
- map/unmap failure, revoke timeout, device removal while busy;
- allocation failure, quota refusal, memory-pressure reclaim.

### 6.2 Event records
Record bounded JSONL/debug records (or fixed binary event ABI) with:
`timestamp_ns, cpu, event_seq, object_diag_id, generation, type, owner, operation, result, reason, bytes, queue_depth`.
No kernel pointer or raw physical address in user-readable output. Redact address-like fields even in debug snapshots unless the reader holds a dedicated machine diagnostic capability.

### 6.3 Object inspection
Read-only object snapshot should report:
- type, diagnostic ID, generation, state, owner;
- effective rights for the querying handle (not global rights);
- buffer extent/format/dimensions where relevant;
- references by category (handles, mappings, DMA, commit, scanout);
- pending queue work and last completion/error;
- creation and last-transition trace sequence.

A query must snapshot under a bounded lock or sequence counter; it must never wait for GPU completion while holding object-table/VM locks.

### 6.4 Invariants
Assert/test:
- no destroy while map/pin/DMA/commit/scanout reference remains;
- no map after revoke begins;
- generation increments on replacement/revoke and stale commits fail;
- rights only attenuate through duplicate/grant;
- damage rectangles are bounded and clipped with checked arithmetic;
- every accepted commit gets exactly one terminal completion (success, cancel or error);
- unplug drains or quarantines resources; never frees memory still DMA-reachable.

## 7. Kernel diagnostic build profiles

Flags are additive by profile, not all forced into production:
- `NX_TRACE=1`: trace ring enabled per subsystem;
- `NX_DIAG_OBJECTS=1`: object IDs/counters/invariant checks;
- `NX_DIAG_LOCKS=1`: lock hold/wait timing and order validation;
- `NX_DIAG_SURFACES=1`: buffer/surface event and accounting;
- `NX_DIAG_DEVICES=1`: device graph/lifecycle/queue diagnostics;
- `NX_STRICT=1`: enforce contract attributes currently optional;
- host `SANITIZE=address,undefined`: host tests only unless target runtime support proven.

The kernel must not enable all tracing by default. Release builds retain safety checks and bounded error counters, but may remove high-volume traces. Every flag has a documented compile-time cost, runtime cost and expected output.

## 8. Test matrix

| Test | amd64 | aarch64 |
|---|---|---|
| clean build + strict build | required | required |
| boot from current legacy path | Multiboot2/GRUB BIOS | Image+DTB / GRUB EFI path separately |
| shell command round trip | required | required |
| serial log complete | required | required |
| screenshot at boot/shell/GUI | required where QEMU display backend supports | required where QEMU display backend supports |
| keyboard/mouse input | virtio + USB/PS2 where present | virtio + USB where present |
| SMP=1 and SMP=4 | required | required |
| malformed FDT/ACPI parser host tests | applicable parser tests | required FDT |
| object/registry capability regression | required | required |
| device add/remove stress | virtual hotplug supported devices | virtual hotplug supported devices |
| compositor/surface lifecycle | required | required |

## 9. Acceptance rules

- A screenshot is “verified” only after opening the captured file and inspecting the actual pixels.
- A boot is “verified” only after full log inspection and successful shell command round trip.
- A GUI input test is “verified” only after event/output change and a before/after screenshot.
- A failed QMP/display path is reported as unsupported or failed, never silently skipped.
- Diagnostic overhead is measured against a no-diagnostic baseline.
- All tests are run serially when they share build outputs, QMP sockets or disk images.


## 10. Instrumentation implemented in legacy QEMU harness — 2026-11-03

The first host-side observability increment is implemented in `tools/nxadb.py`:
- `nxadb start --display none|default` selects headless automation or a visible
  QEMU window without changing the default.
- `nxadb screenshot [name.png]` issues QMP screendump for the explicit
  `nxgpu0` device/head 0, converts PPM to PNG with macOS `sips`, verifies the
  PNG signature and dimensions, refuses overwrite/path escape and writes a
  JSON manifest.
- The QMP screendump device is important: on AMD64 the first unqualified
  screendump captured SeaBIOS/GRUB VGA instead of the VirtIO-GPU desktop.
  Selecting `device=nxgpu0, head=0` fixes this and is verified at 1280x800.
- `nxadb mouse dx dy --click [--button left|middle|right]` sends relative
  pointer motion and separated button edges. QMP schema was inspected on the
  installed QEMU; the pointer and launcher/settings click were visually
  verified.
- QMP text delay increased from 25ms to 75ms per character after the initial
  busy compositor run showed missing characters. Exact command round trips
  now pass on both architectures.

Verified real guest actions:
- AArch64 EFI boot reaches NXShell, executes
  `echo INPUT_TEST_AARCH64_0123456789`, and produces a visually inspected
  1280x800 PNG.
- AMD64 BIOS boot reaches NXShell, executes
  `echo INPUT_TEST_AMD64_0123456789`, and produces a visually inspected
  1280x800 VirtIO-GPU PNG. QMP pointer click opens nxlauncher and nxsettings;
  a second click changes the Style & Theme panel.
- Captured files and manifests are under `build/runs/<arch>/`; complete serial
  logs are `serial-complete.log`. The screenshot artifact from the initial
  unqualified AMD64 screendump was deleted because it contained only the
  firmware VGA screen.

This is verified screenshot + keyboard + mouse interaction in QEMU, not yet
a native desktop-automation agent. The user can opt into a visible QEMU
window using `--display default`.

## 8. Boot capture integrity and operator observability

A boot transcript is a multi-channel artifact, not just the guest kernel UART:
- build stdout/stderr (GRUB/ISO generation and launch wrapper);
- QEMU stdout and stderr (firmware diagnostics, emulator/device failures);
- guest serial/UART stream (UEFI stub, GRUB hand-off where routed, kernel and init);
- QMP events/commands and device inventory;
- screenshots and input-action results.

The capture tool must never treat a tail excerpt as the complete log. Preserve each channel
as a separate raw file, create a per-run manifest with command line, QEMU binary/version,
architecture, CPU model/features, RAM, SMP count, device topology, firmware paths/hashes,
boot artifact hashes, timestamps and exit status. Keep append-only run directories and
make a latest pointer/index, not the only copy. On timeout/fault, print the complete
artifact paths and a clearly labelled bounded excerpt; never discard the full stream.

nxadb currently has two gaps verified in source:
1. start_qemu() deletes the previous serial.log and qemu.stderr.log on every start,
   and redirects QEMU stdout to DEVNULL.
2. boot/shell timeout exceptions expose only session.log_text()[-6000:].
This can erase earlier evidence and omit firmware/emulator diagnostics. Fix these before
using nxadb as the primary boot-test harness.

Screen control must be part of the same test session: QMP screendump, keyboard and mouse
input, and nxwins terminal/window enumeration/focus should record request, result,
timestamp and screenshot. Do not assume a key reached the intended terminal merely because
QMP accepted the event; assert visible state change or a guest-side input acknowledgement.
nxwins remains a userland diagnostic client; it must not become a hidden kernel policy path.

## 9. Memory-map accounting — observed AArch64 QEMU run

The supplied complete transcript reports one usable region:
[0x40000000, 0x180000000) = 5 GiB span, with 5120 MiB reported usable and
5070 MiB free after metadata/kernel reservations. The PMM metadata address is
0x40a48000, size 41120 KiB. This is internally consistent with a 5 GiB QEMU
RAM configuration only if the FDT supplied one contiguous region and no other
reserved gaps were omitted from the PMM input.

Do not conflate:
- RAM region base (0x40000000 on QEMU virt AArch64);
- highest usable end (used to size the PMM PFN metadata span);
- sum of usable bytes (actual installed/allocatable RAM before reservations);
- reserved regions (kernel image, metadata, DTB, initrd/modules, firmware);
- address-space holes (which consume metadata span but must remain reserved);
- DMA-reachable memory (constrained separately by device DMA mask/IOMMU).

The historical AArch64 arch header declared HAL_RAM_SIZE=2 GiB; BOOT-1 has
removed that stale constant after a whole-tree reference audit. The source
translation tables in `kernel/arch/aarch64/kinit/start.S` map a 128 GiB physical
window: L1[0] is Device [0,1 GiB), L1[1..127] are Normal 2 MiB blocks
[1 GiB,128 GiB). This is an address-translation window, not installed RAM.
The VMM now maps only discovered usable regions within ARCH_PHYS_MAP_LIMIT.

The AMD64 5 GiB QEMU layout is different: the PCI/MMIO hole can make address span
larger than physical RAM. Tests must assert usable_pages against region sums,
and verify that every gap/reserved PFN is unavailable, rather than comparing
only the highest end or the span.

## 10. UEFI diagnostics from the supplied transcript

The firmware emits ArmTrngLib initialization failure, several Image start-failed
statuses, TPM/Tcg2 unavailable messages, and an Image type X64 cannot be loaded
on AARCH64 warning. The NEXS EFI stub subsequently reports kernel load, embedded
DTB and successful ExitBootServices, and the kernel reaches userspace. These
firmware warnings are not evidence that the NEXS kernel handoff failed, but they
are not to be silently suppressed: identify which are benign firmware boot-option
probes and which indicate a malformed/mismatched EFI artifact. Validate the ISO
EFI directory and PE machine types; record firmware image hash/version and boot
entry chosen. Require explicit milestones from stub and kernel in tests.

Baseline finding superseded by BOOT-1 implementation: the AArch64 EFI handoff
now passes DtbSpan in x1, BootInfo v1 preserves it, and `fdt_init(base,
available_length)` invokes the bounded parser. Legacy x1=0 remains isolated in
`fdt_init_legacy()`. Host ASan/UBSan validation is 61/61 including the real
QEMU DTB; clean dual-arch builds and runtime verification subsequently passed on 2026-11-03 (see §11 and BOOT-1 report).

## 11. AArch64 runtime evidence — 2026-11-03

Historical baseline run before BOOT-1: QEMU/EDK2 AArch64 boot with KTEST 8/8,
NXShell prompt, nxwins enumeration and focus, 30-second live supervision, two
verified 1280x800 screenshots, and zero kernel fault matches. The later BOOT-1
verification supersedes these counts: AMD64 10/10 KTEST and AArch64 11/11,
with 61 host ASan/UBSan checks, clean builds, GRUB BIOS/UEFI boots, nxwins focus,
30-second soak, 1280x800 screenshot and zero faults on both architectures.
GFX-TIMING render_hold remains a latency warning, not a fatal fault; see the
BOOT-1 report and persistent logs for the complete current run.

The captured Boot0001 entry is the QEMU CD-ROM. The EDK2 warning probes precede
NEXSOS_GRUB_ARM64_CHAINLOAD_STUB and the ordered EFI milestones. See
BOOT-PROTOCOL-INVENTORY.md for the full log and classification.

The pre-BOOT-1 memory instrumentation reported one usable region at
0x40000000..0x180000000 (5120 MiB); PMM free 5070 MiB and metadata at
0x40a49000. At that point the ownership map was incomplete. BOOT-1 now parses
the reservation map and static /reserved-memory nodes and adds the DTB span to
the RESERVED overlays. The host parser test validates the QEMU DTB; runtime
PMM PFN/allocator checks are added but have not yet been boot-tested.

## 12. AMD64 boot-map and soak evidence — 2026-11-03

The corrected BOOT-MEM summary now excludes reserved MMIO descriptors from
usable_span. The AMD64 QEMU run reports 5119 MiB usable, 7168 MiB usable
address span, seven reserved descriptors and highest reserved end 1 TiB.
PMM free=5051 MiB, metadata=0xcd5000 (57568 KiB). The 1 TiB reserved aperture
is not RAM; the 3–4 GiB PCI hole remains reserved inside the usable span.

AMD64 and AArch64 each passed a fresh QEMU 30-second hardening test after the
logging/capture updates: KTEST 8/8, NXShell keyboard input, nxwins enumeration
and focus, two QMP screenshots at 1280x800, zero kernel fault matches.
The detailed per-architecture transcripts are under build/runs/{amd64,aarch64}.
## 13. BOOT-1 FDT/PMM verification status — 2026-11-03

The host parser suite is now located at `kernel/test/fdt_parser_test.c`.
`make test-fdt` builds the AArch64 QEMU DTB as a required input, compiles with
AddressSanitizer and UndefinedBehaviorSanitizer, and does not silently skip the
real-DTB test. Last observed result: **61 checks PASS**; DTB size 1 MiB,
2 output regions (usable RAM + DTB reservation), 4 enabled CPUs.

Runtime KTEST additions cover the PMM usable-region union on both architectures
and, on AArch64, every PFN occupied by the DTB plus allocator samples that must
not return a DTB PFN. Expected suite totals after the baseline eight tests are
9/9 AMD64 and 10/10 AArch64; these are expected counts, not yet observed.

The clean build and runtime gates have not yet been run after the complete BOOT-1
diff. Required evidence remains sequential AMD64/AArch64 build and boot, KTEST,
shell round-trip, nxwins, complete serial/stdout/stderr/manifest capture, fault
scan and soak. The AArch64 EFI path must be tested separately from Image+DTB to
exercise the non-zero x1 length handoff.

Detailed source and implementation status:
`docs/report/BOOT-1-FDT-HARDENING-2026-11-03.md`.
