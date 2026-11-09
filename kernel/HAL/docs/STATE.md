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
