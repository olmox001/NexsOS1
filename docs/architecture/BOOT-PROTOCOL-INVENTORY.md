# NEXS Legacy Boot Path Inventory and NG Boot Contract

Status: inspected against the current Makefile and architecture entry sources.  
Source baseline: b495da3a711e5af64dd826c29c6da4777c487074.

## 1. Actual boot paths used by current targets

The default `make run` path is GRUB ISO boot on both architectures, matching
`nxadb`, `nxrun` and the release media. Raw QEMU kernel loading is retained
only as an explicit diagnostic path (`BOOT_MODE=direct` or `run-direct`).
Do not conflate the direct loader's register contract with the GRUB handoff.

### x86-64 / amd64

- Build output: `build/amd64/kernel.elf`.
- Default development/release boot: GRUB BIOS ISO, then Multiboot2
  `/boot/kernel.elf`; the entry receives the Multiboot2 magic in EAX and the
  information pointer in EBX.
- Actual kernel entry: `kernel/arch/amd64/kinit/start.S`.
- The entry object contains a Multiboot2 header with the standard entry-address
  header tag and a Xen PVH note. It saves the incoming boot registers,
  establishes low boot mappings and enters the higher-half kernel.
- Diagnostic direct boot: QEMU receives `-kernel build/amd64/kernel.elf`.
  This bypasses GRUB. The captured direct-loader run reported
  `Magic: 0x0`, unlike GRUB's `0x36d76289`; it is not evidence of the
  Multiboot2 handoff and must be tracked separately.
- The actual x86 kernel boot path is not `kernel/arch/amd64/boot/boot.S`.

### AArch64

- Build output: `build/aarch64/kernel.bin`, produced from the linked ELF with
  `objcopy -O binary`.
- Default development/release boot: GRUB arm64-efi ISO chainloads the
  project-owned `nexsos-efi-loader.efi`, which loads the kernel and embedded
  QEMU-generated DTB, validates the FDT span, exits boot services and passes
  the DTB physical address in `x0` and validated available size in `x1`.
- Actual kernel entry: `kernel/arch/aarch64/kinit/start.S`.
- The entry preserves the handoff registers, checks CurrentEL, and has EL3,
  EL2 and EL1 transition paths before enabling the MMU and entering the
  higher-half kernel.
- Diagnostic direct boot: QEMU receives `-kernel build/aarch64/kernel.bin`
  and `-dtb build/aarch64/virt.dtb`. QEMU supplies the DTB pointer in `x0`
  but not its available size in `x1`; the kernel therefore uses the explicitly
  retained legacy pointer-only adapter. This does not exercise the sized EFI
  handoff.

The separate `bootloader.elf`/`bootloader.bin` target is built from
`kernel/arch/<arch>/boot/header.S`, `stage1.S` and `stage2.S`, but is not
the loader used by GRUB ISO, default `make run`, `nxadb`, `nxrun` or release.
The current `mkdisk` implementation explicitly accepts but ignores its
bootloader/kernel arguments and creates a GPT root-filesystem image; that
image is not independently bootable. The separate experimental bootloader
sources contain protocol defects documented in §2 and must not be advertised
as a working no-GRUB disk boot path.

## 2. Legacy boot code that must not be reused without repair

- kernel/arch/amd64/boot/boot.S is not listed in BOOT_SOURCES and is not
  compiled by the current Makefile.
- The separate amd64 bootloader header.S declares an entry-address tag with
  type 5, while the Multiboot2 standard entry-address header tag is type 3.
- Its stage1 parser checks type 9 as an entry-address tag; type 9 is the
  EFI64 entry-address tag, not the normal Multiboot2 entry-address tag.
- The separate header and stage1 parser therefore disagree with each other
  and with the standard tag assignment. This path is currently not used by
  QEMU development or release, but it must be corrected before anyone relies
  on bootloader.elf.
- The separate bootloader linker script also emits an RWX LOAD segment warning.
  It needs explicit PHDRS and page-aligned RX/R/RW segments before it is used
  as a trusted loader.
- AArch64 boot/header.S and boot/stage1.S/stage2.S form the same separate
  experimental bootloader path; the actual QEMU kernel entry is kinit/start.S.
- Keep these files and targets for now. Do not delete or silently route the
  working kernel through them.

## 3. Existing early-memory setup observations

### x86-64

The actual kinit entry builds a temporary identity map for the first 1 GiB and
a higher-half alias, plus an additional 3–4 GiB window used for early MMIO.
It enables PAE, long mode and NXE, then jumps to the higher-half entry. The
normal VM subsystem later replaces the temporary page tables with precise
W^X mappings.

### AArch64

The actual kinit entry receives the DTB in X0, uses a temporary 4 KiB-granule
translation setup, maps L1[0] as Device for [0,1 GiB) and L1[1..127] as Normal
2 MiB blocks for [1 GiB,128 GiB), enables MMU/caches, and jumps to the higher
half. This is a 128 GiB mapped physical window, not a 128 MiB RAM size and not
proof that all mapped addresses contain RAM. The normal VMM later replaces
the temporary translation tables with the discovered usable-memory map.
Secondary CPU startup uses PSCI CPU_ON and passes a per-CPU stack through
context_id.

These mappings are bootstrap-only. NG must build a small, explicit early map
from validated BootInfo memory and device ranges, then replace it with the
normal per-address-space mappings. No broad identity map remains after
bootstrap.

## 4. NG boot protocol contract

Bootloader/firmware formats are normalized into one versioned BootInfo. The
kernel must not make its core depend directly on Multiboot2, Xen PVH, UEFI or
QEMU's AArch64 boot stub.

BootInfo fields:
- magic, ABI version, total size and checksum/versioned extension chain;
- boot protocol/source identifier;
- physical memory map with type, attributes and bounds;
- reserved ranges and firmware/runtime memory policy;
- ACPI RSDP and/or DTB physical address plus validated byte length;
- initramfs modules with physical range, length, name, digest and flags;
- framebuffer description and cache attributes;
- boot CPU identity and topology hint;
- EFI memory map and runtime-services availability, when present;
- command line, boot mode (installer/rescue/installed) and root-device hint;
- entropy source metadata, never an unverified claim of cryptographic entropy.

The kernel copies the BootInfo and required tables into owned memory before
reclaiming bootloader memory. All physical ranges are overflow checked and
validated against the memory map. Unknown boot extensions are skipped by
length; unknown mandatory versions fail closed with a diagnostic.

## 5. NG boot phases

1. Firmware/bootloader enters a small architecture adapter.
2. Adapter validates the protocol header and creates BootInfo.
3. Kernel establishes only the minimum stack, exception vectors, MMU and
   console mappings required to continue safely.
4. Kernel discovers memory, CPU, interrupt-controller and timer primitives.
5. Kernel initializes per-CPU state and starts secondary CPUs.
6. Kernel starts system_init and the initramfs service set.
7. devmgr discovers buses/devices; boot block driver exposes the root device.
8. vfsd mounts the installed root or keeps an ephemeral installer root.
9. Other services start; legacy /sys/bin/init is launched unchanged.

## 6. Initial NG boot target policy

- Keep the existing amd64 Multiboot2/PVH entry and AArch64 raw-kernel+DTB
  target working as compatibility boot adapters for the first NG milestone.
- Build a common BootInfo contract and translate each existing handoff into it.
- Add UEFI boot adapters after the common core boots under current QEMU targets.
- Add U-Boot/FDT board adapters for embedded AArch64 only after the core and
  devmgr are stable.
- Evaluate a BSD-licensed UEFI-capable loader (for example Limine) separately;
  do not add a second competing boot chain before the current targets are
  preserved and tested.
- Repair or explicitly deprecate the unused bootloader.elf path only after its
  consumers are checked.

## 7. Acceptance

- The NG kernel boots with the exact current amd64 QEMU command and reaches
  system_init.
- The NG kernel boots with the exact current AArch64 QEMU command and receives
  a valid DTB pointer.
- Both paths produce identical normalized BootInfo fields for equivalent
  QEMU hardware.
- Invalid/truncated Multiboot2, DTB, ACPI and module ranges are rejected.
- No broad bootstrap identity mapping survives normal VM initialization.
- Boot logs record protocol, BootInfo version, memory map hash, CPU count,
  clocksource and selected early console.


## 8. Runtime evidence and remaining UEFI work — 2026-11-03

The current generated AArch64 ISO contains a UEFI El Torito entry (platform
UEFI, `/boot/grub/efi.img`) and the QEMU firmware log
`build/logs/qemu-aarch64-grub-efi-stub-final.log` is a complete successful
chain for the recorded run:
- EDK2/AAVMF firmware starts and selects the CD;
- GRUB AA64 chainloads `nexsos-efi-loader.efi`;
- `NEXSOS_EFI_STUB_START`;
- `NEXSOS_KERNEL_IMAGE_LOADED`;
- `NEXSOS_DTB_EMBEDDED`;
- `NEXSOS_EXIT_BOOT_SERVICES`;
- kernel validates FDT, brings up all four CPUs, starts init/services and reaches
  `[Init] Entering supervisor loop`.

The firmware log also contains non-fatal firmware diagnostics:
`ArmTrngLib could not be correctly initialized`, TPM2 protocol unavailable,
and an X64 image rejected by the AArch64 firmware before the successful CD
boot option. These are recorded, not silently erased. The AArch64 UEFI QEMU
path is therefore **boot-tested**, but the NG BootInfo/firmware contract is
not complete.

AMD64's current generated ISO reports only a BIOS El Torito entry
(`/boot/grub/i386-pc/eltorito.img`). It boots through the legacy Multiboot2/
GRUB BIOS path. There is no current AMD64 UEFI ISO/EFI loader in this target.

### Remaining UEFI gaps (confirmed by source inspection)

- The AArch64 stub embeds a QEMU-generated DTB; `NEXS_EFI_PREFER_FIRMWARE_DTB`
  is 0, so the embedded blob is tried before firmware DTB.
- EFI memory map is acquired only to call ExitBootServices; it is not copied
  into a versioned BootInfo passed to the kernel.
- EFI memory-map descriptor version/size and firmware reserved/runtime memory
  policy are not exported to the normal memory manager.
- GOP framebuffer mode/base/stride/pixel masks are not captured into BootInfo.
- ACPI configuration table pointers (RSDP) and firmware DTB provenance/length
  are not included in a common handoff object.
- Fixed physical load address and assumptions about Image header TextOffset,
  ImageSize and flags need to become validated BootInfo/load policy, not magic
  constants shared only with this stub.
- ExitBootServices retry exists, but failure diagnostics after losing console
  access are limited; final map snapshot and exit status should be preserved
  in a boot handoff/log buffer.
- AMD64 UEFI adapter is absent; don't infer dual-arch UEFI support from the
  AArch64 implementation.

**Required next work:** design common BootInfo with explicit physical bounds,
EFI map descriptors, ACPI/DTB pointers and framebuffer; implement an AMD64
UEFI adapter; test AArch64 and AMD64 UEFI independently in OVMF/AAVMF. Preserve
the current AArch64 UEFI and AMD64 BIOS boot paths as regression baselines until
the new adapters pass.


### FDT length hand-off gap

The UEFI stub's private `validate_fdt(Address, Available, ...)` checks the
FDT magic, totalsize and physical map ceiling before hand-off. However the
handoff assembly passes only the DTB pointer in x0; the kernel calls
`fdt_init(boot_fdt_ptr)` with no available-length argument. The kernel parser
therefore cannot independently prove that every offset/token lies inside the
blob. Non-UEFI AArch64 entry paths also do not inherit the EFI stub's initial
header validation.

The common BootInfo must carry `fdt_phys` AND `fdt_size`; the parser API must
become `fdt_init(base, length)`. Keep the current pointer-only API only as a
legacy adapter with an explicit trusted-boot assumption, then remove it when
all boot protocols populate BootInfo. The stub's header-only validation is not
a replacement for full kernel-side FDT validation.

## 11. UEFI warning classification — external source check

The EDK2 ArmTrngLib constructor prints the exact message
"ArmTrngLib could not be correctly initialized" on its error path, but returns
RETURN_SUCCESS after logging. It is therefore a non-fatal library initialization
warning in this EDK2 implementation, not proof of a NEXS boot failure.
Reference: https://bsdio.com/edk2/docs/master/_arm_trng_lib_8c_source.html

The TPM/Tcg2 messages indicate that the firmware TPM protocol is not available;
the current nxqemu AArch64 command does not configure a TPM device, so these
messages are expected capability probes unless the QEMU machine profile is
changed to include one.

The X64-on-AARCH64 message and several Image start-failed statuses also appear
in unrelated successful QEMU AArch64 UEFI boots using the same EDK2 build.
This strongly suggests firmware boot-option/image probing rather than a NEXS
EFI executable being rejected. It is not yet proven which boot entry generated
each line for our ISO. Record the selected Boot#### entry and inspect EFI PE
machine types before suppressing or reclassifying any line.
Reference example (independent QEMU AArch64 boot transcript):
https://github.com/tikoci/mikropkl/issues/11

The NEXS-specific success criteria remain the explicit stub milestones, valid
AArch64 PE/COFF EFI executable, successful ExitBootServices, valid bounded DTB
handoff, and kernel reaching K3. Firmware warnings remain in the raw capture.

## 12. Captured QEMU AArch64 UEFI run — 2026-11-03

A fresh nxadb capture now preserves the full serial stream and confirms:
- EDK2 2024-09-12 firmware starts Boot0001, explicitly identified as
  "UEFI QEMU QEMU CD-ROM"; it then starts the NEXS ARM64 chainload stub.
- NEXSOS_EFI_STUB_START, NEXSOS_KERNEL_IMAGE_LOADED,
  NEXSOS_DTB_EMBEDDED and NEXSOS_EXIT_BOOT_SERVICES all appear in order.
- Kernel enters at x0=0x17a5a6000 and parses FDT; AArch64 GICv2 and timer
  initialize; 4 CPUs are brought online; PID1 and NXShell start.
- The firmware warnings occur before the selected CD-ROM boot entry and do
  not prevent the NEXS handoff. The x64 image probe is therefore not evidence
  of the NEXS EFI loader having the wrong machine type.
- The exact FDT memory input is one usable region [0x40000000,0x180000000),
  5120 MiB. The new BOOT-MEM instrumentation logs its base, size and end.
  PMM metadata lands at 0x40a49000 (41120 KiB), and PMM reports 5070 MiB free.
- KTEST: 8 passed, 0 failed. NXShell input, nxwins enumeration and
  focus of window 104 succeeded. QMP screenshots were verified at 1280x800
  before and after a 30-second soak. Fault count remained zero.
- GFX-TIMING still reports render_hold commonly around 45–85 ms on this
  configuration; the soak did not produce a kernel fault, but the latency
  remains a performance/IRQ-latency issue requiring separate diagnosis.

Important newly confirmed PMM/FDT gap: kernel/lib/fdt.c explicitly documents
that off_mem_rsvmap is not parsed and firmware-reserved ranges are not sent
to PMM. The embedded DTB is placed near the top of the discovered RAM and
the boot protocol currently passes only its address to the kernel, not DtbSpan.
The memory region log is therefore an inventory of the usable-memory node,
not a complete safe ownership map. Do not mark memory-map hardening complete.
Next implementation must pass DTB address+size in BootInfo, validate every FDT
offset/token/string against that bound, parse FDT reserve-map and reserved-memory
nodes, and reserve the DTB itself before PMM can allocate any of its pages.

## 13. AMD64 boot-map accounting — 2026-11-03

The 5 GiB QEMU q35/BIOS run returns ten descriptors. Usable descriptors sum
to 5119 MiB, with highest usable end 0x1c0000000, so the PMM address span is
7168 MiB from base 0. Seven reserved descriptors include the conventional
memory holes and a high MMIO aperture ending at 0x10000000000 (1 TiB).
That last descriptor is NOT RAM and must not inflate the usable-RAM span.
This exposed and corrected an error in the new diagnostic summary: it had
initially computed span using the highest end of ALL descriptors, including
reserved MMIO. It now reports usable_span from usable descriptors only and
reports reserved descriptor count/highest reserved end separately.

PMM reports 5119 MiB usable, 5051 MiB free, metadata at 0xcd5000 (57568 KiB).
The 3–4 GiB region hole and all reserved descriptors remain visible in the
per-region log; PMM reports a 7168 MiB span with gaps reserved.

The corrected AMD64 kernel was rebuilt and booted in QEMU. KTEST 8/8,
NXShell input, nxwins list and focus of window 104, two 1280x800 screenshots,
30-second supervision, and zero fault matches all passed.
## 14. BOOT-1 FDT handoff and memory ownership — 2026-11-03

This section supersedes the historical pointer-only gap recorded in §§10 and 12.
It describes the current implementation state, not a claim that the complete
multi-architecture BootInfo/UEFI programme is finished.

- AArch64 EFI now passes the validated DTB totalsize in x1; the entry preserves
  x0/x1 in the 32-byte BootInfo v1 record. C validates the record, invokes
  `fdt_init(base, available_length)`, then normalizes `fdt_size` to the actual
  FDT totalsize. Legacy x1=0 remains supported through `fdt_init_legacy()`.
- `kernel/lib/fdt_parser.c` is a bounded parser shared by memory-region and
  CPU discovery. It validates header/block/token/string/property/reservation
  bounds, static `/reserved-memory`, cell widths and structure depth. Dynamic
  `size`/allocation-based reserved-memory nodes are explicitly rejected.
- The legacy zero-pointer search preserves the existing 1 GiB window and now
  probes 4 KiB pages, scans 8-byte-aligned candidates, and validates each
  candidate's complete blob. AArch64 page probing uses an exact-LDR Data Abort
  fixup.
- The PMM input includes FDT reservation-map entries, static reserved-memory,
  and the DTB itself. PMM counts the usable-region union, reserves all overlays
  and address-space holes, and places metadata only in a suitable usable
  interval. VMM maps only usable ranges within the architecture direct-map
  ceiling.
- The AArch64 bootstrap map in `start.S` covers the 128 GiB physical window
  described in §3; it is not a claim that the full window is populated by RAM.
  `ARCH_PHYS_MAP_LIMIT` is 128 GiB on AArch64 and 256 GiB on AMD64.
- Host validation: `make test-fdt` compiles with ASan/UBSan and requires the
  generated QEMU DTB. Last observed result: 61 checks PASS; QEMU DTB 1 MiB,
  2 resulting regions (RAM + DTB reservation), 4 CPUs. Tests live under
  `kernel/test/fdt_parser_test.c`.

Runtime status: **PASS (2026-11-03)**. Clean builds completed on AMD64 and
AArch64; `make test-all` exited 0. AMD64 booted the GRUB BIOS ISO (10/10 KTEST);
AArch64 booted the GRUB arm64-efi UEFI ISO (11/11 KTEST), with BootInfo source=1
and x1 carrying the 1 MiB DTB span. The AArch64 KTEST also invokes the legacy
x1=0 adapter and confirms page-by-page search finds the real QEMU DTB at
0x40000000 after removing its own synthetic fixture. Both architectures passed
NXShell readiness, nxwins focus, 30-second soak, 1280x800 screenshot and
zero-fault scan. Full logs: `docs/report/logs/`; implementation record:
`docs/report/BOOT-1-FDT-HARDENING-2026-11-03.md`. Fuzzing, dynamic reserved-memory,
generic FDT device discovery, AMD64 UEFI and hardware certification remain
separate future work.

Detailed implementation and verification record:
`docs/report/BOOT-1-FDT-HARDENING-2026-11-03.md`.

Still outside BOOT-1: common AMD64+AArch64 BootInfo with EFI memory map/ACPI/
framebuffer/modules, AMD64 UEFI adapter, generic FDT device/interrupt discovery,
continuous fuzzing and real-hardware certification.
