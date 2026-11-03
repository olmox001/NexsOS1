# NEXS Legacy Boot Path Inventory and NG Boot Contract

Status: inspected against the current Makefile and architecture entry sources.  
Source baseline: b495da3a711e5af64dd826c29c6da4777c487074.

## 1. Actual boot path used by current targets

### x86-64 / amd64

- Build output: build/amd64/kernel.elf.
- Development run: QEMU receives -kernel build/amd64/kernel.elf.
- Release ISO: GRUB configuration uses multiboot2 /boot/kernel.elf.
- Actual kernel entry: kernel/arch/amd64/kinit/start.S.
- The entry object contains a Multiboot2 header with the entry-address header
  tag and a Xen PVH note. It receives the Multiboot2 magic in EAX and the
  information pointer in EBX, saves both, establishes low boot mappings and
  enters the higher-half kernel.
- The actual x86 kernel boot path is not kernel/arch/amd64/boot/boot.S.

### AArch64

- Build output: build/aarch64/kernel.bin, produced from the linked ELF with
  objcopy -O binary.
- Development run: QEMU receives -M virt -cpu cortex-a57 -m 5G -smp 4,
  the VirtIO GPU/input/block devices, -kernel build/aarch64/kernel.bin and
  -dtb build/aarch64/virt.dtb.
- The DTB is generated from the same QEMU machine configuration.
- QEMU's AArch64 raw-kernel stub passes the DTB physical address in X0.
- Actual kernel entry: kernel/arch/aarch64/kinit/start.S.
- The entry accepts X0 as the DTB pointer, checks CurrentEL, and has EL3,
  EL2 and EL1 transition paths before enabling the MMU and entering the
  higher-half kernel.

The development targets are not using the separate bootloader.elf as the
kernel loader. The current bootloader target builds a separate artifact from
kernel/arch/<arch>/boot/header.S, stage1.S and stage2.S; it is included in
make all but is not used by the run or release boot path described above.

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
translation setup, maps early device space and a broad RAM window, enables
MMU/caches, and jumps to the higher half. The normal VMM later replaces the
temporary translation tables. Secondary CPU startup uses PSCI CPU_ON and
passes a per-CPU stack through context_id.

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
