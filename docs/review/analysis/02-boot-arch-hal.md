# Subsystem Analysis 02 — Boot, Platform Discovery, HAL & Arch-MMU

> Severity/kind tags per [`../TAXONOMY.md`](../TAXONOMY.md). Hand-written (maintainer), spot-checked against runtime.

| | |
|---|---|
| **Subsystem** | Boot handoff, platform/hardware discovery, HAL, architecture MMU |
| **Sources (hand-read)** | `kernel/arch/amd64/boot/start.S` (261), `kernel/arch/amd64/platform/platform.c` (356), `kernel/arch/aarch64/platform.c` (172), `kernel/core/hal_bus.c` (53), `kernel/arch/amd64/hal.c` (83), `kernel/arch/aarch64/hal.c` (46), `kernel/include/kernel/hal.h` (63), `kernel/arch/amd64/mm/mmu.c` (321), `kernel/arch/aarch64/mm/mmu.c` (226) |
| **Deferred to doc 03** | arch CPU/exception/syscall mechanics (cpu.c, idt/gdt/apic/msr, exception.S, isr_stubs.S, context.S, trampoline.S, uaccess.c) |
| **Build** | **[verified]** clean both arches |

---

## 1. Purpose & Role

This layer takes control from firmware/QEMU, establishes long mode / EL1, discovers
RAM + CPUs, builds the first page tables, and presents a Hardware Abstraction Layer
(device registry + register accessors) so the generic kernel is arch-agnostic.

## 2. The headline finding — architecture parity is conditional

The project is presented as "boots correctly on both `make run ARCH=aarch64` and
`make run ARCH=amd64`". **Runtime evidence refines this:**

| Capability | aarch64 (`make run`) | amd64 (`make run`, `-kernel`) |
|---|---|---|
| Boot protocol | DTB via `x0` ✓ | **magic `0x0` — unrecognized** ✗ |
| RAM detected | **3967 MB** (real) ✓ | **hardcoded 1 GB** (ignores `-m`) ✗ |
| Maps "up to 4GB" | **yes** ✓ | **no** (1GB) ✗ |
| `-m 4G` | OK ✓ | **virtio-blk "queue size 0" → divide-by-zero crash** ✗ |
| SMP | CPUs 1-3 online ✓ | boots to shell at 3G |
| Reaches TTY shell | ✓ | ✓ at 3G only |

So amd64's 4GB / real-memory-map path is the **GRUB-ISO (`make release`) path**, *not*
`make run`. The old `SMP_AMD64_STABILIZATION_REPORT.md` ("STABLE… make run ARCH=amd64:
Boot completo con 4 core") overstates the `make run` path. This is the most important
correction in the whole review.

## 3. Findings

| ID | Sev | Kind | Location | Summary |
|----|-----|------|----------|---------|
| BOOT-01 | W4 | BUG · WRONG-DESIGN | `amd64/boot/start.S:5-30,123-124`; `platform.c:157-186` | amd64 `-kernel` boot delivers no recognized magic: kernel has MB2 header + PVH note but **no MB1 header**; QEMU `-kernel` doesn't do MB2, uses PVH; PVH passes info in `%ebx` with magic *inside* the start_info struct, but the code expects a magic *register value* == `PVH_MAGIC` → never matches. |
| BOOT-02 | W4 | BUG | `platform.c:173-185` | Consequence: amd64 falls back to a **hardcoded 1 GB** map, ignoring real RAM; the "4GB" goal is unreachable on `make run`; at `-m 4G` it crashes. |
| BOOT-03 | W2 | REFINE | `platform.c:278-281` | SMP startup sends a single STARTUP IPI; the SDM-recommended sequence is INIT-SIPI-SIPI (two SIPIs). |
| BOOT-04 | W1 | DOC · BAD-IMPL | `platform.c:225-228` | amd64 code uses aarch64 names (`secondary_ttbr0`, `arch_vmm_set_secondary_pgd`) — cross-arch naming leak. |
| ARCH-01 | W3 | WRONG-DESIGN | `platform.c:292-311` | amd64 CPU count via `CPUID.01h:EBX[23:16]` = max addressable APIC IDs, not online cores; should parse **ACPI MADT**. (aarch64 correctly uses FDT, `aarch64/platform.c:112`.) |
| ARCH-02 | W3 | STUB | `platform.c:222` | `arch_pci_init()` is an empty stub; amd64 has no ACPI parsing at all. |
| ARCH-03 | W2 | STUB | `platform.c:79,189-193` | `timer_get_us()` returns `jiffies*1000`; `jiffies` is "dummy for now" — no real microsecond clock on amd64. |
| ARCH-04 | W2 | BAD-IMPL · BUG | `aarch64/platform.c:46-96` | FDT memory parse evidently fails at runtime (log shows "Manual discovery"); RAM size is found by a **page-fault probe**; and lines 82-88 are a **dead double-assignment** immediately overwritten by 91-94. |
| ARCH-05 | W2 | REFINE | `aarch64/platform.c:62-96` | The probe reads raw physical addresses pre-MMU relying on a bus-abort + `probe_failed` flag; fragile and can touch device windows. |
| HAL-01 | W3 | WRONG-DESIGN · PERF | `kernel/include/kernel/hal.h:36-61` | **Your "HAL too complicated":** every `hal_read32(addr)` synthesizes a compound-literal `hal_device_t` + runs `hal_auto_type`/`hal_auto_bus` heuristics + dispatches through `hal_dev_read32` — per-access overhead on hot register paths. |
| HAL-02 | W2 | BAD-IMPL | `hal.h:1-7` | HAL spread over 4 headers (`hal.h`+`hal_unified.h`+`hal_device.h`+`hal_platform.h`); layering exceeds the need. (The device *registry* `hal_bus.c` is genuinely thin and fine.) |
| HAL-03 | W1 | REFINE | `hal.h:36-53` | Resource/bus auto-detection by address magnitude (`addr < 0x10000`) is a fragile heuristic. |
| AMMU-01 | W3 | SECURITY | `amd64/mm/mmu.c:54-61,128-140`; `aarch64/mm/mmu.c` (via `PAGE_KERNEL_EXEC`) | **No W^X**: amd64 maps RAM RW with NX set only when *both* `PTE_UXN`&`PTE_PXN` are present (never, for kernel RAM); aarch64 maps RAM executable. Corroborates MM-VMM-01. |
| AMMU-02 | W3 | STUB · SECURITY | `amd64/mm/mmu.c:280-287` | `arch_vmm_protect()` is a no-op stub → runtime permission changes silently ignored. aarch64 appears to have **no** `arch_vmm_protect` at all (MISSING). |
| AMMU-03 | W3 | BUG | `amd64/mm/mmu.c:266-277`; `kernel/mm/vmm.c:272-316` | Two divergent teardown paths (`arch_vmm_destroy_process_pgd` frees only the PML4; generic `vmm_destroy_pgd` walks index 0). Neither frees user RAM frames → leak. |
| AMMU-04 | W2 | BUG | `kernel/mm/vmm.c:37-43` vs `amd64/mm/mmu.c:82-125` | Two `get_next_table` implementations: the generic (vmm.c) one does **not** understand 2MB/1GB blocks; if it walks a block region created by `vmm_dynamic_remap`, it misreads a block as a table pointer. [inferred] |
| AMMU-05 | W2 | SECURITY | `amd64/mm/mmu.c:111,123,244` | All intermediate page-table entries are tagged `X86_PTE_US` (user-accessible), more permissive than necessary. |
| AMMU-06 | W2 | PERF | `amd64/mm/mmu.c:69-75` | `arch_vmm_map_mmio` maps `0xFE000000–0xFFFFFFFF` one 4KB page at a time (~8192 map calls) at every PGD setup. |
| AMMU-07 | W2 | PERF·REFINE | `amd64/mm/mmu.c:69-75` | MMIO identity-mapped only for `0xFE000000–0xFFFFFFFF`; any device window outside this range is unmapped. (NOTE: the amd64 `-m 4G` crash I first attributed here is actually **DRV-VIRTIO-01** — 64-bit BAR truncation in `pci_get_bar`/`amd64/hal.c`; confirmed by source read. This row is the lesser, separate MMIO-coverage limitation.) |
| AMMU-08 | W2 | BUG | `amd64/mm/mmu.c:153-158`; `aarch64/mm/mmu.c:102-107` | TLB invalidation only on the modifying CPU; **no SMP shootdown** to peers. Corroborates MM-VMM-05. |

### Detailed entries (selected)

**BOOT-01/02 — amd64 boot protocol `[verified magic=0x0; inferred mechanism]`**
Serial shows `AMD64 Platform Initialization (Magic: 0x0, Info: 0x1580)` then `Unknown
boot protocol (Magic: 0x0). Using safe 1GB default.` The header block in `start.S`
provides Multiboot2 (`0xe85250d6`) and a PVH note (`.note.PVH`, entry `_start_32`) but
**no Multiboot1 header**. QEMU `-kernel` for x86 honours Multiboot1 and PVH, not
Multiboot2; here it boots PVH, whose ABI delivers the `hvm_start_info` pointer in a
register and stores its identifying magic *in the struct's first field*, not in a
"magic register". `platform.c` only recognises `mb_magic ∈ {MB1, MB2, PVH}` as a saved
register value, so PVH boot lands in the `else` and hardcodes 1GB. *Fix options:* (a)
add a Multiboot1 header so `-kernel` sets `EAX=0x2BADB002` and the existing MB1 path
runs; or (b) implement PVH correctly (treat `%ebx`→`hvm_start_info`, validate
`start_info->magic`). Either restores real memory discovery and unblocks 4GB on amd64.

**HAL-01 — the over-abstraction you flagged `[static]`**
A bare MMIO read should compile to a `mov` from a mapped address. Here
`hal_read32(addr)` (`hal.h:58`) expands to constructing a temporary `hal_device_t`
via two heuristic functions and then an indirect `hal_dev_read32` call. That is the
opposite of the Plan 9 portability layer, which is a *thin* set of direct primitives.
The good news: the device **registry** (`hal_bus.c`, `hal_register_device`,
`hal_device_find`) is already minimal and worth keeping. *Fix direction:* replace the
compound-literal alias macros with direct `static inline` `mmio_read32`/`port_in*`
accessors; keep the registry; delete `hal_unified`/`hal_platform` indirection where it
only wraps a `mov`.

## 4. Refactor Direction (toward the declared goals)

| Goal | Implication here |
|---|---|
| **Both arches "just boot"** | Fix BOOT-01/02 (multiboot1 or PVH) so amd64 `make run` gets the real map; or document that amd64 requires the ISO path and make `make run ARCH=amd64` build+run the ISO. |
| **Stable boot via a real boot library (GPLv2-compatible)** | A vetted loader (e.g. **Limine**, BSD-2 → GPLv2-compatible; or **U-Boot**, GPLv2) would replace the hand-rolled `start.S`/PVH guesswork and give consistent multiboot2/handoff on both arches and real hardware. (Note: GRUB is GPLv3 — *not* GPLv2-only-compatible; flag for the license decision.) |
| **Thin Plan 9-style HAL** | Collapse HAL-01/02 to direct inline accessors + the existing thin registry. |
| **seL4 isolation** | W^X (AMMU-01), working `arch_vmm_protect` (AMMU-02), frame-refcounted teardown (AMMU-03), SMP TLB shootdown (AMMU-08) are prerequisites. |
| **Real hardware** | ACPI MADT for CPU count (ARCH-01), real PCI init (ARCH-02), real timers (ARCH-03), robust memory discovery instead of the probe (ARCH-04/05). |

## 5. Verification Notes
- aarch64 boot/discovery/SMP/4GB: **[verified]** (serial log).
- amd64 boot magic=0x0 + 1GB fallback + `-m 4G` crash: **[verified]**. Crash root
  cause **confirmed** as DRV-VIRTIO-01 (64-bit BAR truncation, `pci.c:106` +
  `amd64/hal.c:26-27`), superseding my initial MMIO-range inference (now AMMU-07).
- W^X, teardown leak, no TLB shootdown: **[static]** (source), consistent with MM doc.
