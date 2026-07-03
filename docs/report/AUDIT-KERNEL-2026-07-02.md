# Kernel Audit — 2026-07-02

**Scope:** full read of the boot path (`kernel/main.c`, `kernel/arch/aarch64/platform.c`,
`kernel/arch/amd64/platform/platform.c`), a targeted sweep for parallel/double
implementations, stale/contradictory comments in kernel core, and a HAL-conformance
recheck against `docs/direction/DIR-06-hal-conformance.md`. Report-only — **no code
edited, no commit created**. `make all ARCH=aarch64` and `make all ARCH=amd64` both
verified to complete successfully (build logs end `Build complete for <arch>!`); no
qemu was run. Every claim below is anchored to a `file:line`.

Reference docs read in full before this audit: `docs/ASTRA.md` §1, §2, §7;
`docs/direction/DIR-06-hal-conformance.md`; `docs/B3-SANDBOX-PLAN.md` §3.

---

## 1. Boot path map

### 1.1 Today's sequence (`kernel/main.c`, `kernel_main`)

| Step | Call | `main.c` line | Phase (target model) | Notes |
|---|---|---|---|---|
| 1 | `driver_console_init()` | 76 | K1 | UART first for debug output |
| 2 | `boot_fdt_ptr = x0_arg; fdt_init(...)` (aarch64) / `fdt_init(0)` (amd64) | 80–86 | K1 | arch-conditional at the C level (`#ifndef ARCH_AMD64`), legitimate boot-ABI difference (DIR-06 already accepts this class, see §4) |
| 3 | `print_banner()` | 89 | — | cosmetic |
| 4 | `stack_guard_init()` | 95 | K1 | SSP canary reseed from arch entropy |
| 5 | `cpu_init()` | 99 | K1 | arch-owned (`kernel/arch/{amd64,aarch64}/cpu/cpu.c`) — exception vectors, per-CPU data |
| 6 | `arch_platform_early_init()` | 102 | K1 | registers IRQ chip + parses FDT/boot-protocol (arch platform.c) |
| 7 | `driver_irq_init(); irq_init(); irq_init_percpu();` | 104–106 | K1 | |
| 8 | `driver_timer_init(); timer_init_percpu();` | 110–111 | K1 | |
| 9 | `init_memory()` | 115 | K1→K2 boundary | see §1.2 — PMM/VMM is K1 (memory=hardware), everything after `hal_bus_init()` inside this function is K2 (subsystem init: block/GPU/graphics/GPT/buffer/VFS/keyboard/**registry/procfs**) |
| 10 | `process_init()` | 119 | K2 | pool/runqueue setup only (`kernel/sched/process.c:421-453`), no userland dependency |
| 11 | `init_scheduler()` | 123 | **K2/K3 fused** | `compositor_init()` (K2) then **spawns + enqueues PID1** (K3) then creates CPU0's idle task (K2) — see §1.3, this is the sharpest phase-blur in the file |
| 12 | `arch_smp_init()` | 131 | K1 (SMP bring-up is still hardware bring-up) | **runs AFTER PID1 is already enqueued and schedulable** — see §1.4 |
| 13 | `local_irq_enable()` | 135 | K1→K3 handoff | CPU0 starts taking IRQs/preempting into PID1 only here |
| 14 | `while(1) hal_cpu_idle();` | 145–147 | idle | three redundant labels for one loop, see §3 |

`init_memory()` internal order (`main.c:165-244`): `arch_platform_get_mem_regions`
→ boot-module reservation → `pmm_early_init/pmm_init` → `vmm_init` →
`vmm_dynamic_remap` → `ktest_run_all()` → `hal_bus_init()` → `virtio_blk_init()` →
`ramdisk_init()` → `virtio_gpu_init()` → `graphics_init()` → `gpt_init()` →
`buffer_init()` → `vfs_register_fs(&ext4_fs_ops); vfs_init();` → `keyboard_init()`
→ `registry_init(); registry_mount_vfs(); procfs_init();` (`main.c:226-240`).

### 1.2 Mapping onto K1 / K2 / K3 (maintainer's target)

The maintainer's target is **three separate kernel systems** — K1 (memory/hardware),
K2 (subsystem init), K3 (init-userland, gated on K1+K2 confirmed) — with userland
itself split into U1 (`system_init`, low-level services) and U2 (`nxinit`, system
apps), both **userland processes, not kernel**.

Today's `kernel_main` does not express this separation structurally: it is one
linear function with no phase boundary, no "K1+K2 confirmed" gate, and no
distinction between "kernel finished initializing" and "kernel is now allowed to
load userland." Concretely:

- K1 (steps 1–8, plus PMM/VMM inside step 9 up to `hal_bus_init()`) and K2 (the
  rest of step 9, step 10, and the `compositor_init()` half of step 11) are
  interleaved inside `init_memory()`/`init_scheduler()` rather than being two
  functions with a clear return-value gate between them.
- K3 (spawning `/sys/bin/init` = PID1) is **not gated on "K1+K2 confirmed"** — it
  is the second half of the *same* `init_scheduler()` function that also creates
  the idle task (K2), with no checkpoint in between (`main.c:251-269`).
- There is no U1/U2 split visible from the kernel side today: `main.c:260` loads
  a single ELF, `/sys/bin/init`, and the U1-vs-U2 distinction (low-level services
  vs. system apps) is entirely userland-internal (inside `user/sys/bin/init.c`),
  invisible to the kernel boot map. Confirmed by reading `init_scheduler()` — it
  has exactly one `process_load_elf` call (`main.c:260`).

### 1.3 Phase-blur flags (file:line)

- **PID1 created + enqueued in `init_scheduler()` BEFORE `arch_smp_init()` +
  `local_irq_enable()`** (`main.c:123` vs. `main.c:131,135`). `init_scheduler()`
  (`main.c:251-269`) calls `process_create()` (`main.c:259`),
  `process_load_elf()` (`main.c:260`), and **`enqueue_task(init)`**
  (`main.c:262`) — which puts PID1 on CPU0's runqueue (`on_cpu==-1` resolves to
  CPU 0, `kernel/sched/process.c:237-239`) and calls `hal_cpu_notify()`
  (`kernel/sched/process.c:258`) — all *before* `arch_smp_init()` is even
  called. Because CPU0's own `local_irq_enable()` is deferred until
  `main.c:135` (after `arch_smp_init()` returns), CPU0 itself cannot preempt
  into PID1 early. **The actual risk is a secondary CPU**: each arch's
  `arch_smp_init()` loop creates that CPU's idle task and wakes it *one CPU at
  a time* (`kernel/arch/aarch64/platform.c:269-326`,
  `kernel/arch/amd64/platform/platform.c:554-593`); the moment an early-woken
  secondary reaches `kernel_secondary_main()`
  (`main.c:274-305`) and calls its own `local_irq_enable()` (`main.c:294`), its
  timer can preempt into `schedule()`, which can **work-steal PID1 off CPU0's
  runqueue** (`spin_trylock(&other_cpu->sched_lock)`,
  `kernel/sched/process.c:1588-1589`) — i.e. userland can start running on a
  secondary CPU while `arch_smp_init()` is still bringing up *later* CPUs on
  the primary, before K1 (SMP bring-up, a K1 concern per the target model) has
  finished confirming the whole machine.
- **`panic("Failed to load /init")` kills kernel-alone**
  (`main.c:264`). `panic()` (`kernel/lib/printk.c:256-311`) is unconditional:
  it signals all CPUs to halt via IPI (`irq_send_ipi_all()`,
  `kernel/lib/printk.c:275`/`285`) and then hard-resets the whole machine after
  a grace period (`panic_reboot_after_grace()`, `kernel/lib/printk.c:281`/`308`).
  A K3-only failure (userland's first program failed to load) currently takes
  down K1+K2 with it — there is no path where the kernel stays alive,
  diagnosable, without userland.
- **PID1 created at `PLVL_MACHINE`** (`main.c:259`:
  `process_create("init", PROC_PRIO_USER, PLVL_MACHINE)`) **— violates B3
  §3.1**. Per `docs/B3-SANDBOX-PLAN.md` §3.1, `PLVL_MACHINE` is defined as "the
  machine's own identity — NOT a login user; unkillable, bypasses capability
  checks" — it is meant for the kernel/system identity, not for a spawned
  userland process. In the actual code this level grants PID1 the full
  `CAP_ALL` ceiling unconditionally
  (`kernel/sched/process.c:599-604`, `level_ceiling[PLVL_MACHINE] = CAP_ALL`)
  and makes it **unkillable**
  (`kernel/sched/process.c:967-973`, `proc_is_machine(proc)` check in
  `process_terminate`) and exempt from the monotonic capability clamp entirely
  (`kernel/sched/process.c:693`: `if (creator && !proc_is_machine(creator))`
  — a machine creator's children skip the `caps &= creator->caps` narrowing).
  The target (per the task and B3 §3.1) is `PLVL_ROOT` + explicit capabilities
  via `process_create_caps()` (already present and used internally by
  `process_create()`, `kernel/sched/process.c:606-611,613-614`) — PID1 does not
  need to be unkillable-by-construction or bypass every capability check to do
  its job.
- **Registry/procfs init order vs. their future role backing the VFS.**
  `registry_init(); registry_mount_vfs(); procfs_init();` run at
  `main.c:234-240`, *after* `vfs_register_fs(&ext4_fs_ops); vfs_init();`
  (`main.c:226-227`). Today this ordering is correct — `registry_mount_vfs()`
  and `procfs_init()` both call `vfs_mount_at()`
  (`kernel/lib/registry.c:556`, `kernel/fs/procfs.c:117`) which requires the
  VFS mount table to already exist. The forward risk (ASTRA §6.4, "SRL service
  migration") is structural, not a bug today: when the registry becomes a
  supervised ELF service, `registry_mount_vfs()` will need to happen through
  IPC to a userland process instead of a direct in-kernel call — there is no
  seam/contract boundary at `main.c:237` today that anticipates that move (it
  is a plain function call, like every other line in `init_memory()`).
- **Idle-task creation points are split across three files with no shared
  primitive.** CPU0's idle task is created in arch-neutral code
  (`smp_create_idle_task(0)`, `main.c:268`, inside `init_scheduler()` — i.e.
  paired with K3's PID1 spawn in the very same function). Every secondary
  CPU's idle task is created from *inside* each arch's own `arch_smp_init()`
  loop: `kernel/arch/aarch64/platform.c:294` and
  `kernel/arch/amd64/platform/platform.c:572` — two independent call sites
  performing the identical logical step ("publish this CPU's idle task before
  it can run `schedule()`", per the `SMP-IDLE-RACE #169/#170` comments at both
  sites) with no shared SMP-bring-up primitive between them (ASTRA §3 names
  "B6 — SMP sweep (#96): per-CPU bring-up and IPIs behind a CPU/SMP primitive"
  as exactly this future work — today it does not exist).

### 1.4 Corroborating evidence already in the tree

Both platform files carry extensive self-documented `NOTE(...)` issue markers
that independently corroborate the K1/K2/K3 blur, most relevantly:
`kernel/arch/amd64/platform/platform.c:22-46` header block (BOOT-01..04,
ARCH-01..03) and `kernel/arch/aarch64/platform.c:19-29` header block
(ARCH-04/05). These are pre-existing, already-tracked issues (not new findings
of this audit) but they reinforce that `platform.c` on both arches mixes real
hardware discovery (K1) with ad-hoc fallback heuristics that were clearly
patched incrementally rather than designed against a K1/K2/K3 contract.

---

## 2. Double/parallel logic still alive

### 2.1 `SYS_FILE_READ`/`SYS_FILE_WRITE`/`SYS_LIST_DIR` vs. the object/handle path — ALIVE, actively used, genuine duplication

Two complete, independent implementations of file I/O coexist in the same
dispatcher:

- **Handle-based (ASTRA §6.2 "the fd table folded into the object table")**:
  `SYS_OPEN` (`kernel/core/syscall_dispatch.c:259-285`) →
  `sys_handle_create(OS1_NS_FS, path, rights, OBJ_TYPE_FILE)`; `SYS_READ`
  (296-314) → `sys_object_read`; `SYS_WRITE` (315+) → `sys_object_write`;
  `SYS_CLOSE` (286-289) → `sys_handle_close`; `SYS_LSEEK` (290-295) →
  `sys_object_lseek`. The dispatcher's own header comment states "There is no
  separate fd array" (`kernel/core/syscall_dispatch.c:66`).
- **Path-based (parallel, bypasses handles entirely)**: `SYS_FILE_WRITE`
  (`kernel/core/syscall_dispatch.c:738-784`) does its own
  `arch_copy_string_from_user` + `vfs_resolve_path` + `kmalloc` +
  `vfs_write_file` — no handle involved anywhere. `SYS_FILE_READ`
  (785-823) and `SYS_LIST_DIR` (834-864) are the same shape, calling
  `vfs_read_file`/`vfs_list_dir` directly.

This is **not dead legacy**: `user/sys/lib/lib.c:397-399` defines
`file_write`/`file_read`/`list_dir` as thin wrappers over
`_sys_file_write`/`_sys_file_read`/`_sys_list_dir` (i.e. `SYS_FILE_WRITE` /
`SYS_FILE_READ` / `SYS_LIST_DIR`), and these are the primary file API called
throughout `user/sys/bin/init.c`, `user/sys/bin/nxlauncher.c`,
`user/sys/bin/nexs-fm/{state,fileops}.c`, `user/bin/kilo/kilo.c`,
`user/sys/bin/nxshell.c` (confirmed via grep across `user/`). Both paths are
live, both are exercised, and they enforce access control independently:
`SYS_FILE_WRITE` checks `CAP_FS_WRITE` + the `/sys,/bin` ACL inline
(`kernel/core/syscall_dispatch.c:746-764`), duplicating logic that
`sys_handle_create` also performs for the `SYS_OPEN` path (referenced at
`kernel/core/syscall_dispatch.c:50-51` in the file header's capability-check
table). Two independent ACL checks for the same underlying VFS write is exactly
the kind of double-enforcement surface that drifts when one side gets a fix and
the other doesn't (`SYS_FILE_READ`, notably, has **no** capability check at all
— reads are open by design, but that means the two "parallel" paths aren't even
symmetric with each other).

### 2.2 `sys_registry` (`REG_OP_*`) vs. `/reg` VFS (`regfs_*`) — ALIVE, both exercised against the same keys

`sys_registry()` (`kernel/lib/registry.c:572` onward, dispatched at
`kernel/core/syscall_dispatch.c:735-736` via `SYS_REGISTRY`=250) is a complete
standalone op-switch (`REG_OP_READ`/`WRITE`/`ENUM`/`DEL`,
`kernel/include/kernel/registry.h:10-13`) operating directly on
`registry_get`/`registry_set`/`registry_del`. Separately, `regfs_read`/
`regfs_write`/`regfs_list`/`regfs_unlink`
(`kernel/lib/registry.c:442-542`) are mounted at `/reg`
(`registry_mount_vfs()`, `kernel/lib/registry.c:556`) and reachable through the
generic VFS surface (`SYS_OPEN`+handle, or `SYS_FILE_READ`/`SYS_FILE_WRITE`
from §2.1). Both converge on the same underlying tree
(`registry_get`/`registry_set`/`registry_del`), but there are two distinct
syscall entry points with two distinct capability-check call sites: `sys_registry`
gates writes with `proc_has_cap(current_process, CAP_REG_WRITE)` inline in its
own switch, and `regfs_write` gates the *same* capability again independently
(`kernel/lib/registry.c:471-472`, comment explicitly acknowledges this: "in
addition to the CAP_FS_WRITE the SYS_FILE_WRITE path already checked — the two
caps layer"). `user/bin/capreg.c` is the smoking gun that this is not
theoretical: it calls `OS1_registry_set`/`OS1_registry_get`/`OS1_registry_del`
(→ `sys_registry`, e.g. `capreg.c:77-156`) **and** `OS1_fs_write("/reg/...")`
/`OS1_fs_unlink("/reg/...")` (→ VFS → `regfs_*`, `capreg.c:101,116,124`)
against the same namespace in the same test.

### 2.3 fd-table remnants after Stage-4 flatten (`0905842`) — CLEAN, no live duplication found

Commit `0905842` ("F4.1 Stage 4 — flatten the fd table into the object/handle
table") genuinely retired the old per-process `fds[16]` array. Grepped for
`struct fd_table`, `fds[16]`, `FD_FILE`, `FD_WIN`, `FD_KBD`,
`fd_array`, `process_fd_init` across `kernel/`: the only hits are **comments**
referencing the old model for context (`kernel/core/object.c:527,655`,
`kernel/core/syscall_dispatch.c:223,300`,
`kernel/sched/process.c:749` — "replaces the old process_fd_init"). No
surviving struct, array, or parallel descriptor path. The `fd>=100`
window-id alias mentioned as legacy in `docs/B3-SANDBOX-PLAN.md` batch 7 is
also gone (only historical comments remain,
`kernel/core/syscall_dispatch.c:382`, `user/sys/lib/lib.c:419`), and the stale
unbuilt `user/sys/lib/syscall.S` the same plan calls out for deletion no
longer exists (only the two legitimate arch-specific `user/arch/{aarch64,amd64}/syscall.S`
remain). **This item can be marked done, not a residual double-logic risk.**

### 2.4 `SYS_FLUSH` vs. `SYS_COMPOSITOR_RENDER` after `339b31f` — RETIRED, confirmed resolved

`include/api/syscall_nums.h:40-41`: "201 retired: SYS_FLUSH was a duplicate of
SYS_COMPOSITOR_RENDER (212) — both just pushed the compositor. Unified onto
212; flush() now routes there." No `SYS_FLUSH` symbol exists in
`syscall_nums.h` any more; number 201 is not reused. **Confirmed resolved, not
a live double-logic item** — the task's framing anticipated this might still be
open; it is not.

### 2.5 Graphics `g_ctx`/`graphics_get_screen_surface` vs. `vgpu_get_framebuffer` — dual accessor over now-unified memory

Two named APIs exist for what is now the same physical buffer, used by two
different call sites:

- `graphics.c`'s `g_ctx` (`kernel/graphics/graphics.c:36`, populated by
  `graphics_init()` at line 49-59) and `graphics_get_screen_surface()`
  (line 85-96, re-queries `gpu_get_primary()` every call) are consumed by the
  low-level draw wrappers (`draw2d.c`, `draw3d.c` — 7+ call sites) and by the
  on-screen panic path (per the comment at
  `kernel/drivers/gpu/virtio_gpu.c:472-475`).
- `vgpu_get_framebuffer()` (`kernel/drivers/gpu/virtio_gpu.c:197`, wired as
  `gpu_ops.get_framebuffer`) is consumed by the compositor's flush path
  (`kernel/graphics/compositor.c:2191-2192`,
  `dev->ops->get_framebuffer(dev, NULL)`), which blits its own separate
  `compositor_backbuffer` (`kernel/graphics/compositor.c:301`) into it at
  damage-rect granularity.

The comment at `kernel/drivers/gpu/virtio_gpu.c:472-475` documents that these
used to point at *different* memory (`dev->framebuffer_virt` was never
assigned, so the panic screen silently no-op'd) and were unified by pointing
`dev->framebuffer_virt = priv->backing_store` — the same buffer
`vgpu_get_framebuffer` returns. So the underlying **data** is no longer
duplicated (good, a real bug was fixed here), but the **API surface** still
has two names/paths for reaching it — one arch-neutral "HAL bridge" accessor
(`graphics_get_screen_surface`) and one `gpu_ops` contract method
(`vgpu_get_framebuffer`), self-flagged as `GFX-GFX-01` in
`kernel/graphics/graphics.c:18-24` for an unrelated concurrency caveat
(function-static return value not SMP-safe). Not urgent, but a legitimate
candidate for collapsing onto one accessor per ASTRA's "no layer contains
knowledge of which implementation sits underneath it" rule (§1.1).

### 2.6 `fault_printf`/`printk`/`panic` — NOT double logic, deliberate two-tier contract (negative finding, recorded for completeness)

`fault_printf`/`fault_vprintf` (`kernel/lib/fault_print.c:50-64`) are an
explicitly lock-free, `get_cpu_info()`-avoiding emergency path (file header,
`kernel/lib/fault_print.c:1-14`), used only when `fault_depth() > 0`.
`printk`/`vprintk` (`kernel/lib/printk.c:133`) are the normal-context,
lock-taking path. `panic()` (`kernel/lib/printk.c:256-311`) explicitly branches
between the two based on `fault_depth()` at line 267. This is the same
two-tier pattern DIR-06 already accepts for IRQ EOI (chip-owned implementation
behind one contract) — recorded here as a checked-and-cleared item, not a
finding requiring action.

---

## 3. Stale/contradictory comments in kernel core

- **`kernel/main.c:125-127`** — three-line internal dialogue that contradicts
  itself mid-comment: `/* Set CPU0 current task to idle (placeholder) to allow
  scheduling? */` (a question, "placeholder") immediately followed by `/*
  Actually, init_scheduler creates 'init' and 'idle' tasks. */` and `/* We want
  CPU0 to pick 'init' immediately. */`. This reads as an unresolved note-to-self
  left in place after the actual behavior was figured out — the "placeholder"
  framing is stale relative to the two corrective lines right below it.
- **`kernel/main.c:249`** — `/* Initialize scheduler (placeholder) */`
  directly above `static void init_scheduler(void)`. The function body
  (`main.c:251-269`) is not a placeholder: it creates PID1 via
  `process_create`+`process_load_elf`, unconditionally `panic()`s the whole
  machine if that fails (`main.c:264`, see §1.3), enqueues PID1, and creates
  CPU0's idle task. Calling this "(placeholder)" understates that it is the
  single most load-bearing function in the K2→K3 transition.
- **`kernel/main.c:140-144`** — three separate labels for one loop: `/* Main
  kernel loop */` (140), `pr_info("Entering idle loop...\n")` (141), `/* Enter
  supervisor loop */` (144), `pr_info("[Init] Entering supervisor loop\n")`
  (144-145) — all describing the identical `while (1) { hal_cpu_idle(); }`
  three lines later (146-147). Reads as layered edits (three different names:
  "kernel loop", "idle loop", "supervisor loop") that were never consolidated;
  harmless at runtime (it's just two log lines) but actively misleading about
  how many distinct things are happening at boot's tail.
- **`kernel/graphics/compositor.c:16`** — `/* Disable optimizations to ensure
  stack safety/debugging */` sitting alone between two `#include` blocks
  (lines 6-13 and 17-25), with **no** `#pragma GCC optimize`, no
  `__attribute__((optimize(...)))` anywhere in the file, and no per-file `-O0`
  override in the Makefile (`Makefile:218` lists `compositor.c` among ordinary
  sources under the global build flags — confirmed by grep, no
  `compositor`-specific flag line exists). The comment claims an effect that is
  not implemented anywhere in the file today; either a pragma was removed and
  the comment survived, or it was never wired up. Worth either restoring the
  pragma (if stack safety on this file is still a real concern — it is large
  and IRQ-adjacent, `compositor.c` is 2410 lines) or deleting the comment.

No further contradictions were found in `kernel/sched/process.c`,
`kernel/core/*.c`, or `kernel/lib/registry.c` beyond the above; those files'
"no longer"/"resolved"/"stale" language was cross-checked against the current
code in each case (e.g. the SCHED-01 focus-boost claim in
`kernel/sched/process.c:31-35` was verified true against
`kernel/graphics/compositor.c` and `kernel/include/kernel/graphics.h:113-118`
— `compositor_get_focus_pid()` really is gone, the push-only model really is
what both files implement) and found accurate, not stale. `process.c` is
2000+ lines and self-flags its own complexity (`SCHED-02`,
`kernel/sched/process.c:36-37`: "schedule() is large and intricate; many pc==0
panic guards betray past context-corruption bugs") — a full line-by-line
contradiction sweep of that file was not exhaustive beyond the targeted checks
above; a dedicated pass on `process.c` alone is a reasonable follow-up if the
maintainer wants deeper coverage than this audit's scope allowed.

---

## 4. HAL/DIR-06 conformance recheck

`grep -rn "ifdef ARCH_" kernel/ --include=*.c` outside `kernel/arch/`:

| Site | Gates | Status |
|---|---|---|
| `kernel/main.c:37,57,65` | `kernel_main` signature (multiboot magic+mbi vs. x0/FDT), `mb_info_ptr`/`mb_magic` extraction | **Justified** — already accepted by DIR-06 as "legitimate boot-ABI difference" (`docs/direction/DIR-06-hal-conformance.md:52-54`); unchanged since that assessment |
| `kernel/drivers/virtio/virtio_input.c:18-20` | `#include <drivers/pci.h>` only | **Justified** — confirmed by reading the file: `is_pci` is a plain `int` field/parameter present in **both** arch builds (`virtio_input.c:33,69`), explicitly `(void)is_pci;` on line 70 where unused; only the PCI *header* is gated for the amd64-only PCI transport-discovery path. This resolves DIR-06's "to review" note (`docs/direction/DIR-06-hal-conformance.md:56`) — it is a provider gate, same class as `ps2.c`, not a core-logic leak |
| `kernel/drivers/ps2/ps2.c:10` | entire PS/2 driver body | **Justified** — PS/2 is x86-only hardware; already accepted by DIR-06 (`docs/direction/DIR-06-hal-conformance.md:55`) |
| `kernel/lib/ktest_samples.c:110,124` | expected PTE bit layout in `test_vmm_protect` (AArch64 `AP[7:6]` vs. amd64 `NX`/`RW`) | **Justified (new, not previously reviewed)** — this is a test *assertion* checking that the arch-neutral `vmm_protect()` contract produces the correct arch-specific encoding underneath; observing per-arch bit layout is the point of the test, not a leak into core logic |

**Core sweep (re-verified this audit, matches DIR-06's existing "core is clean"
finding):** `grep -rn "rdmsr\|wrmsr\|outb(\|inb(\|__asm__" kernel/core/*.c
kernel/sched/*.c kernel/mm/*.c` → zero hits. Extended this audit to
`kernel/irq/*.c`, `kernel/fs/*.c`, `kernel/graphics/*.c` for
`rdmsr/wrmsr/__asm__/outb/inb/cntvct_el0/cntfrq_el0` → zero hits, all clean.
`kernel/sched/elf.c` → zero `ARCH_*` ifdefs, confirming DIR-06's "FIXED" entry
for the PTE-encoding leak (`docs/direction/DIR-06-hal-conformance.md:42-49`)
still holds.

**`platform.c` frozen-file note.** ASTRA §5 rule 2 ("No new code in
`kernel/arch/<arch>/platform/`") and ASTRA §2's table ("frozen file — do not
touch until B4 replaces it behind a contract") were checked against git
history: `kernel/arch/amd64/platform/platform.c` was touched on 2026-06-27
(`74fc5cc`, "publish per-CPU idle task before waking AP, SMP idle-race
#169/#170") — after DIR-06's last edit (2026-06-25) and within the freeze
window. The change itself (`git show 74fc5cc -- kernel/arch/amd64/platform/platform.c`)
is a pure reordering fix inside the existing `arch_smp_init()` loop (move
`smp_create_idle_task(i)` before the SIPI instead of after the ACK wait,
mirrored on aarch64 at `kernel/arch/aarch64/platform.c:294`) — the commit
message explicitly reasons about "DIR-06 conformant" and it is a critical
correctness fix (a NULL idle-task context switch), not new feature surface.
Recorded as a **minor rule-tension worth a maintainer decision**, not a hard
violation: either the freeze should carry an explicit "critical-fix" carve-out
in ASTRA §5, or this class of fix should land through the eventual B4 contract
instead of the frozen file directly.

**Per-arch behavioral divergence candidates** (places where the same logical
step is implemented twice, independently, rather than once behind a shared
primitive): the idle-task-creation split from §1.3 (aarch64
`platform.c:294` vs. amd64 `platform.c:572`, no shared SMP primitive) is the
clearest live candidate; `arch_smp_init`'s CPU-count discovery is a second one
(`fdt_count_cpus()` on aarch64 vs. `amd64_count_cpus()`'s CPUID EBX[23:16] —
already self-flagged as `ARCH-01` in
`kernel/arch/amd64/platform/platform.c:39-42` for over-counting on HT/NUMA,
not re-litigated here since it is pre-existing tracked debt, not a new
finding).

---

## 5. Top-10 cleanup micro-phases

Ordered, small, independently buildable, cleanup/alignment only — no new
features. Each is scoped to build cleanly on both arches per this audit's
verified baseline (`make all ARCH=aarch64` / `ARCH=amd64` both currently pass).

1. **Demote PID1 from `PLVL_MACHINE` to `PLVL_ROOT` + explicit caps.** Change
   `main.c:259` to call `process_create_caps("init", PROC_PRIO_USER, PLVL_ROOT,
   CAP_ALL)` (or equivalent), closing the B3 §3.1 violation (§1.3). Small,
   isolated, testable via the existing `sandboxtest`/`captest` regressions.
2. **Delete the three stale/contradictory boot-loop comments in `main.c`**
   (§3): collapse `main.c:125-127` into one accurate sentence, fix the
   `init_scheduler` "(placeholder)" label at `main.c:249`, and unify the three
   loop labels at `main.c:140-144` into one. Zero behavior change, pure
   comment hygiene.
3. **Restore or remove the dead "Disable optimizations" comment in
   `compositor.c:16`** (§3) — either add the matching
   `__attribute__((optimize("O0")))`/pragma if stack safety on this file is
   still wanted, or delete the orphaned claim.
4. **Add a DSB/load-acquire on the aarch64 `cpu_boot_ack` spin-wait**
   (`kernel/arch/aarch64/platform.c:235-239,303-307`) — the file's own comment
   already flags this as unverified on real hardware; a one-line barrier
   closes a documented, self-acknowledged gap without touching the surrounding
   logic.
5. **Collapse `graphics_get_screen_surface`/`g_ctx` onto the `gpu_ops.get_framebuffer`
   contract** (§2.5) so `draw2d.c`/`draw3d.c`/the panic path and the compositor
   read the scanout buffer through one accessor instead of two, now that both
   already resolve to the same `backing_store` pointer.
6. **Pick one entry point for registry access and make the other a thin
   wrapper** (§2.2) — either have `sys_registry()` call through
   `vfs_write_file("/reg/...")`/`vfs_read_file`/`vfs_list_dir` internally, or
   have `regfs_write`/`regfs_read` call `sys_registry`'s underlying helpers —
   so there is one capability-check call site instead of two independently
   maintained ones.
7. **Pick one entry point for file I/O and make the other a thin wrapper**
   (§2.1) — same shape as #6 but for `SYS_FILE_READ`/`WRITE`/`LIST_DIR` vs.
   `SYS_OPEN`+handle. Larger than #6 (more call sites in userland,
   `user/sys/lib/lib.c:397-399` is the seam to preserve so no userland ABI
   changes), but the same mechanical pattern: keep both syscalls, remove the
   duplicated ACL/VFS logic underneath one of them.
8. **Extract a shared `arch_smp_bringup_cpu(cpu_id, entry, stack)` primitive**
   consumed by both `kernel/arch/aarch64/platform.c:269-326` and
   `kernel/arch/amd64/platform/platform.c:554-593`, so the idle-task-creation
   ordering fix that already had to be applied twice (`74fc5cc`, once per
   arch) only needs to be reasoned about once. This is the concrete first
   slice of ASTRA's named "B6 — SMP sweep (#96)".
9. **Add an explicit K1/K2 return-value gate inside `init_memory()`/
   `process_init()`/`init_scheduler()`** — even before any bigger restructure,
   splitting `init_scheduler()` (`main.c:251-269`) into
   `init_scheduler()` (idle task only, K2) and a separate
   `spawn_init_process()` (K3, called only after an explicit "K1+K2 confirmed"
   check) directly addresses the sharpest phase-blur found in §1.3 without
   touching `arch_smp_init`/`local_irq_enable` ordering yet.
10. **Give `panic()` a K3-scoped variant** (or a boot-phase flag consulted by
    `panic()`) so a K3-only failure like `main.c:264`
    (`panic("Failed to load /init")`) can eventually halt userland bring-up
    and drop to a kernel-alone diagnostic prompt instead of hard-resetting the
    whole machine — scope this phase to just adding the boot-phase flag and
    the conditional branch in `printk.c`; wiring an actual kernel-alone
    diagnostic shell is follow-on work, not part of this micro-phase.
