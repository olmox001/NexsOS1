# NX-HAL migration state

- Existing kernel/arch ownership moved under kernel/HAL/arch.
- NX-CORE links architecture providers through libnxhal.a.
- Core ISA primitives are real hal_* symbols; generic code no longer includes
  arch/arch.h.
- Generic kernel has no ARCH_AMD64/ARCH_AARCH64 branches and uses an opaque
  HAL execution frame.
- PTE/MMU format constants are provider-selected HAL headers.
- Complete requested NXEC preboot common/HAL/test source is integrated into
  HAL-owned paths; temporary HAL/nxec remains only for equivalence checking.
- Integrated NX-HAL host, x86_64/AArch64 freestanding cross-link, ASan/UBSan,
  clang-tidy and cppcheck gates pass on the current source; the test harness
  resolves clang-tidy from the installed LLVM toolchain instead of assuming it
  is in PATH.
- Fault decision, recursion-guard and backtrace callbacks now cross the
  versioned `nxhal_fault_protocol_v1` boundary instead of direct provider-to-Core
  calls. The protocol has direct bind/ABI/forwarding host and sanitizer tests.
- Standalone NX-HAL and NX-CORE builds pass for amd64 and aarch64 with this
  protocol enabled.
- HCD ISA discovery no longer owns a second raw register-access implementation.
  AMD64 CPUID/CR/MSR reads and AArch64 discovery system-register reads now use
  provider-private `arch/isa.h` primitives shared with the runtime provider;
  cross-link and static-analysis gates pass for both adapters, and both HAL/Core
  architecture builds pass after the change.
- Provider diagnostics now use the versioned `nxhal_diagnostic_protocol_v1`
  boundary. Normal logs preserve Core log-level filtering/prefixing through
  `vprintk_level`; fault output preserves the existing lock-free
  `fault_vprintf` path; panic preserves the existing owner/IPI/backtrace/screen/
  watchdog sequence through `vpanic`. Direct provider calls to
  `pr_*`, `fault_printf` and `panic` are removed. Host, sanitizer, static,
  standalone HAL and integrated Core builds pass on both architectures.
- A whole-archive relocatable audit is used for true boundary measurement
  (member-level `nm -u` is not sufficient because it reports HAL-internal
  references as undefined). Current mandatory reverse-edge removals are:
  device/IRQ/PCI registration and access; PMM/VMM memory services; SMP/timer
  services; Core-owned per-CPU/global state; and FDT/boot handoff. The direct
  panic-state write was removed as redundant with the atomic Core panic publish,
  and device naming now uses bounded HAL-owned encoders instead of Core
  `snprintf`; the consolidated HAL objects have no external `panic_flag`,
  `snprintf`, `printk`, `fault_printf` or `panic` reference.
- Per-CPU timer drift/accounting is Core-owned behind the versioned
  `nxhal_timer_protocol_v1`: providers call only `arm_current`/`tick_current`
  and receive the resulting hardware compare target as a scalar. No `cpu_info`
  pointer crosses this timer boundary, and consolidated HAL objects no longer
  reference `timer_percpu_arm` or `timer_percpu_tick`; the remaining aarch64
  `get_cpu_info` dependency belongs to uaccess and is tracked separately.
  Host/cross/sanitizer/static gates plus standalone and integrated amd64/aarch64
  builds pass with this seam.
- SMP bring-up no longer forms a Core/provider symbol cycle. Core policy owns
  idle-task-first and the bounded ACK wait, calls the outbound
  `hal_smp_wake_secondary(cpu, stack)` primitive, and providers request policy
  only through versioned `nxhal_smp_protocol_v1`. The dead Core entry-function
  argument was removed from both providers; aarch64's assembly wake ABI is now
  `(cpu_id, stack_top)` and amd64 uses the same provider-private primitive.
  No legacy `arch_cpu_wake_secondary`/`cpu_wake_secondary` alias remains.
  Host/cross/sanitizer/static plus standalone/integrated amd64+aarch64 builds
  pass, and consolidated HAL objects have no old SMP reverse-edge symbols.
- Linker-defined image symbols and the single Core boot entry are tracked
  separately and are not mistaken for service dependencies.
- Timekeeping now has one versioned NX-HAL clocksource/clockevent contract in
  `hal/time.h`; NX-CORE scheduler, SMP waits, VirtIO deadlines, PMM timing and
  panic-time clock reads no longer depend on the retired generic raw
  `hal_timer_get_count` interface.
- Early SSP entropy is intentionally separate from timekeeping. The versioned
  `hal/entropy.h` contract exposes architected hardware randomness and an
  explicitly non-temporal early jitter sample, with provider-private ISA
  adapters on amd64/aarch64 and direct host/sanitizer/static tests.
- Scheduler policy is no longer encoded as a number of timer IRQs. The previous
  `DEFAULT_QUANTUM=3` silently became a 3 ms quantum after the clockevent moved
  to 1 kHz and the timer path invoked `schedule()` every millisecond. The
  scheduler now owns a 30 ms monotonic deadline and timer IRQs reschedule only
  when that deadline expires (idle/non-running states remain immediate).
- The time/scheduler snapshot passes clean NX-CORE builds on both ISAs and the
  full five-lane nxadb matrix: amd64 GRUB-BIOS/GRUB-UEFI/direct 24/24 KTEST,
  AArch64 GRUB-UEFI/direct 25/25 KTEST, timerdiag + counter heartbeat +
  shell-after-heartbeat PASS, zero detected faults.
- Performance measurement after removing 1 kHz forced preemption still observes
  roughly 1.06-1.12k context switches/s on the current amd64 desktop. This is
  not closed: keyboard now blocks on raw-event readiness, but polling-only USB
  HID fallback, the 5 ms mouse service loop, 10 ms shell loop and other timed
  service loops remain, while nxcompositor allocates/free
  frame and per-window pixel buffers on every 33 ms frame. Those are the next
  measured wakeup/allocation targets; do not attribute the residual rate to the
  scheduler tick without new evidence.
- The first input migration micro-phase is build- and runtime-qualified.
  Input no longer uses or extends the generic object ABI: there is no
  `OBJ_TYPE_INPUT`/`OS1_NS_INPUT`. A single `drivers/input/core.c` owns the raw
  keyboard/mouse queues; PS/2, USB HID and VirtIO publish through
  `OS1driver_input_publish`, and the dead secondary VirtIO input buffer/API was
  removed. Syscall 278 is retained as the stable number but is now the bounded
  `SYS_INPUT_RAW_READ(event, role, mode)` contract with TRY/WAIT modes and one
  `OS1low_input_read_raw` user ABI. `nxkeyboardd` uses the blocking path, with
  an explicit `-EAGAIN` fallback only while polling-only USB HCD support remains.
- The misnamed HAL `nxhal_input_protocol_v1` was removed rather than preserved
  as a no-op alias. Its sole provider was PL011 UART RX and its sole Core action
  woke a keyboard wait queue with no real HID consumer; UART and HID/input are
  now kept as distinct subsystems.
- Two pre-provider KTEST cases exercise keyboard and mouse queue routing plus
  invalid role/mode rejection. The new input core passes the local Power12 AST
  gate (no heap/goto/unbounded loops and all functions <=50 physical lines).
  HAL host/cross/sanitizer/static, both standalone HAL boundary audits, clean
  NX-CORE builds and complete amd64/aarch64 rootfs builds pass. All five nxadb
  runtime lanes pass with interactive shell/timerdiag/heartbeat/post-heartbeat
  shell and zero faults: amd64 GRUB-BIOS/GRUB-UEFI/direct 24/24 KTEST and
  AArch64 GRUB-UEFI/direct 25/25 KTEST.
- Keyboard navigation is now explicitly qualified end-to-end. Common evdev
  codes live in `drivers/input_codes.h` instead of the VirtIO driver header;
  PS/2 Set-1 has a bounded E0/E1 decoder that maps arrows/navigation/right-side
  modifiers to canonical evdev codes, and `nxkeyboardd` forwards scancode-only
  keys with ASCII=0 instead of dropping them. Two new KTEST cases cover PS/2
  extended press/release and navigation decoding. The PS/2 controller bring-up
  was also split into bounded controller/mouse/IRQ helpers: source-local
  Power12 audit reports no heap/goto/unbounded loops, `ps2_init` is 17 lines,
  the largest IRQ handler is 38 lines, and the decoder is 31 lines.
  `nxadb test` now includes `SHELL-UP-HISTORY=PASS`, proving real Up-arrow
  history recall through the VirtIO/input-core/user-service/IPC/NXShell path on
  every runtime lane.
- The post-fix five-lane navigation gate passes on amd64 BIOS/UEFI/direct and
  AArch64 UEFI/direct with timerdiag, short heartbeat, post-heartbeat shell,
  24/24 (amd64) or 25/25 (AArch64) KTEST and zero detected faults. Separate
  `make run` image/direct gates also reach NXShell on both architectures.
- Independent `make run` boot qualification reaches NXShell in all five
  current Makefile lanes: amd64 GRUB-BIOS, amd64 GRUB-UEFI, amd64 direct,
  AArch64 GRUB-UEFI and AArch64 direct. Runtime policy for both image and direct
  boots is centralized in `tools/nxqemu.py`; the dead duplicate QEMU flag set
  was removed. AMD64 UEFI uses OVMF + GRUB x86_64-efi Multiboot2 and explicitly
  boots with no DTB dependency.
- GRUB source/build ownership is now unified around the clean
  `boot/grub/upstream` submodule pinned at
  `d38d6a1a9b79427848976f53d474392cd29c2a71` (GRUB 2.14). One builder and one
  versioned patchset produce i386-pc, x86_64-efi and arm64-efi from that pin.
  Host-specific installed stages are fingerprinted and published under
  `build/cache/grub/prebuilt/<target>`, survive `make clean`, and are reused
  without recreating `build/grub/<target>`. Package/install round trips with
  version, commit, pipeline, host and tree-hash validation pass; all three
  targets also pass delete-scratch/reuse tests. Measured warm reuse on the
  current macOS host is 0-1 seconds for i386-pc, x86_64-efi and arm64-efi,
  with no configure/build tree recreation. Clean AMD64 BIOS+UEFI and AArch64
  UEFI media builds consume those prebuilt stages, and El Torito inspection
  confirms distinct BIOS/UEFI boot entries. The remaining long AArch64 image
  build observed during qualification was a full userland/rootfs rebuild
  (SDL/Lua/gnulib/apps + ext4 image population), not a GRUB rebuild; incremental
  rootfs/userland dependency granularity remains a separate build-system task.

## Diagnostic stop point — 2026-11-09

The HAL split is not complete.  The current tree has a useful mechanical
boundary and several real versioned protocols, but it must not be described as
repository-independent or semantically clean yet.

Verified strengths at this stop point:

- NX-CORE generic C/H source has zero direct `arch/*` includes and zero
  `ARCH_AMD64`/`ARCH_AARCH64` conditional branches under the boundary scan.
- NX-CORE links architecture implementation through one `libnxhal.a` archive.
- Versioned Core/HAL protocols exist for fault, diagnostics, timer, SMP, device,
  IRQ, CPU, uaccess and memory interactions.
- Consolidated link audits currently leave only runtime/compiler/image symbols
  on amd64; AArch64 additionally retains the FDT/boot reverse edges listed
  below.
- Both architecture builds and the five runtime lanes were qualified before
  this stop point; keyboard-navigation regression coverage is permanent in
  `nxadb test`.

Blocking architectural debt still present:

1. **Device drivers are still inside HAL providers.**
   `arch/aarch64/devices/gic.c`, `uart_pl011.c`,
   `arch/amd64/devices/uart_16550.c` and `pic_pit.c` contain real driver
   policy, buffering, IRQ registration and device/controller behavior.  They
   are not ISA primitives.  `virtio.c` under both provider trees also depends
   on the kernel driver interface.  These must be moved to the proper
   driver/service layer and consume HAL resource/transport primitives instead.
   Interrupt/timer hardware primitives that genuinely belong to the provider
   must be separated from driver policy rather than moving the current files
   wholesale.

2. **HAL provider source still depends on NX-CORE private headers.**
   28 provider files currently include `kernel/*` headers, including scheduler,
   fault, IRQ, PMM/VMM, platform, memory-layout and string/type internals.  The
   standalone HAL build hides this problem by adding `-I$(KERNEL_ROOT)/include`
   plus the Core ABI/API include paths.

3. **The HAL core still contains provider selection.**
   `HAL/core/core.c` directly includes `arch/arch.h` and still has an
   `ARCH_AMD64` conditional for port I/O.  `HAL/core/frame.c` directly
   includes `arch/pt_regs.h` and branches on `ARCH_AARCH64`.  Provider-private
   frame/ISA implementation must sit behind provider operations so the HAL core
   itself is architecture-neutral.

4. **AArch64 still has direct FDT/Core reverse edges.**
   The consolidated AArch64 HAL object still imports `boot_fdt_ptr`,
   `boot_fdt_size`, `fdt_count_cpus` and `fdt_get_mem_regions`.
   Boot/FDT data must cross a normalized HAL-owned boot descriptor/protocol,
   never Core globals/functions.

5. **NX-CORE build is not independent of HAL/provider selection.**
   `kernel/Makefile` still selects `HAL/arch/<isa>`, passes `-DARCH_*`,
   exposes the private provider include directory and consumes the provider
   linker script directly.  A future standalone NX-CORE repository must
   compile against only a neutral HAL SDK/contract; provider selection and
   image/link integration belong to the coordinating build layer.

6. **NX-HAL standalone build is not standalone.**
   `HAL/Makefile` includes `../config/toolchain-versions.env` and Core
   include/ABI/API directories.  A clone of HAL by itself cannot currently
   build from its own declared dependencies.

7. **Resource/capability semantics are not yet the authoritative device path.**
   `hal/resource.h` has bounded resources, grants and rights, but live device
   discovery still publishes raw `nxhal_device_desc.base` and `.irq`.
   Provider/device code does not yet consume grants/resources as the mandatory
   access path, so the capability model is presently parallel validation rather
   than enforced hardware authority.

8. **The boundary gate is incomplete by design.**
   `tools/audit-boundary.sh` rejects selected migrated symbol families and
   generic NX-CORE ISA leaks, but does not reject HAL provider inclusion of
   Core-private headers.  Its PASS therefore means “no regression in the
   already-migrated symbol families”, not “HAL/Core separation complete”.

9. **Assembly/frame ABI reconstruction remains unfinished.**
   AMD64 syscall/ISR/context assembly and AArch64 exception assembly still carry
   hand-maintained provider frame offsets.  C static assertions protect some
   layouts, but there is not yet one assembler-safe provider ABI definition
   consumed and checked by both sides.

Work is intentionally stopped at this diagnostic checkpoint.  No further HAL
migration should be marked complete until these debts are addressed and
requalified on both architectures.


## 2026-11-10 — NXPAX fault telemetry increment (separate from HAL boundary qualification)

The existing HAL/Core fault protocol now emits a typed, bounded NXPAX hardware
event before delegating fault policy back to the existing Core implementation.
This retains architecture-owned exception decoding on amd64 and aarch64.
The production ring is in `HAL/core/nxpax.c` and is capacity-linked to
`2 * NXHAL_CPU_LIMIT`. Sequence/slot ownership and drops are atomic; slot
reads use a single nonwaiting ownership CAS, so an interrupt-path producer
does not wait on a reader and concurrent snapshots cannot mix two records.
The diagnostic callback publication path was also corrected from volatile
flagging to release/acquire atomic publication.

The Core consumes no new HAL-internal policy and adds only a disposition
report in `kernel/core/nxpax.c`. Userland reads the existing RESOURCE object
through `sys/bin/nxpax` and the named capability port; no new syscall ID
or object type was introduced. Direct fault-register/resource access is
privileged and non-transferable. See `../../../docs/NXPAX-IMPLEMENTATION-STATE.md`
for exact ABI, source map, latest kernel/userland builds, guest runs, crash
injection, host/sanitizer and concurrent stress evidence.

**Qualification limitation:** Power12 strict gate is active but RED due to
unproved requirements and actual findings; existing HAL/Core boundary debts
listed above remain open. Successful NXPAX tests do not certify HAL
separation, complete fault coverage or Power12.


## 2026-11-10 — HAL IRQ-mask duration telemetry checkpoint

NX-HAL now has allocation-free per-CPU telemetry for explicit local IRQ-mask
intervals.  The instrumentation lives at the HAL IRQ primitive boundary, so it
covers direct enable/disable and save/restore users, including irqsave
spinlocks, without teaching NX-CORE about DAIF or RFLAGS.

The implementation derives interval nesting from the architectural IRQ state
captured before masking.  Only an enabled-to-disabled transition starts an
interval; nested saves while already masked do not create a second interval,
and only a restore/enable that returns to an enabled state closes it.  Raw
clocksource counts come from the provider clocksource; early calls made before
that clocksource is ready are recorded as invalid samples rather than using a
fabricated frequency.  The public snapshot is sequence-validated and bounded to
four read attempts; it reports interval count, total/max raw counts, active
start and invalid samples.

Qualification at this checkpoint is deliberately limited to the HAL boundary:
the dedicated optimized host test and ASan/UBSan test pass; the full NX-HAL
host and sanitizer suites pass; and both amd64 and AArch64 kernels compile with
their existing warning-as-error policy.  The test exercises nesting, multiple
interval accumulation, an unavailable clocksource, a backwards counter sample,
NULL output and CPU bounds.  No assert, heap allocation, goto, unbounded loop or
warning suppression was added.

This is not yet runtime evidence that long guest IRQ-off sections explain the
AArch64/QEMU timer lateness.  The next step is to expose the snapshot through
the common diagnostic/object path and measure it during the same nxadb timer
campaign before changing timer or scheduler policy.


## 2026-11-10 — Dual-architecture clean-build and boot stability checkpoint

NX-HAL's canonical source list, fault protocol and IRQ-mask telemetry remain
in place. The two architectures compile through NX-CORE's standalone build,
and the HAL host test suite passes. Kernel/HAL compiler profiles retain
blocking baseline correctness warnings; conversion warnings remain visible,
with a separate fail-fast conversion qualification switch and explicit debt.

On the fresh complete build, nxadb booted and interacted with all four
lanes: AMD64 GRUB BIOS ISO, AMD64 direct, AArch64 GRUB UEFI ISO, and AArch64
direct. The respective kernel test summaries were 28/28 and 29/29; each
lane completed the shell, timerdiag, heartbeat and NXPAX service probe
with zero unexpected faults. No new HAL architecture ABI was introduced
to achieve those passes. This establishes boot qualification, not complete
Power12 qualification or verified IRQ-off root-cause attribution.

Power12 audit findings remain recorded and block strict qualification.
Refer to `../../../docs/architecture/BUILD-BOOT-STABILITY-2026-11-10.md`
for the exact build profile, logs, open safety work and handoff status.
