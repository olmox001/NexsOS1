> STATUS: agent-generated, **maintainer spot-checked** (2026-06-02) — see REVIEW.md Corrections section.

# Subsystem Analysis 03 — CPU, Exceptions, Syscall Entry, Context Switch & User-Access

> Severity/kind tags per [`../TAXONOMY.md`](../TAXONOMY.md).  Evidence basis:
> **[verified]** built/run; **[static]** read-only; **[inferred]** reasoned with assumption stated.

| | |
|---|---|
| **Subsystem** | CPU init, IDT/GDT, APIC, exception dispatch, syscall entry (SYSCALL/SYSRET + int 0x80 + SVC), context switch, uaccess |
| **Sources (amd64)** | `kernel/arch/amd64/cpu/cpu.c`, `idt.c`, `gdt.c`, `apic.c`, `msr.c`, `syscall_hal.c`, `isr_stubs.S`, `context.S`, `syscall.S`, `gdt_defs.h`; `kernel/arch/amd64/boot/trampoline.S`; `kernel/arch/amd64/mm/uaccess.c`; `kernel/arch/amd64/include/arch/amd64_internal.h`, `arch/pt_regs.h`, `arch/arch.h` |
| **Sources (aarch64)** | `kernel/arch/aarch64/cpu/cpu.c`, `syscall.c`, `exception.S`; `kernel/arch/aarch64/boot/start.S`; `kernel/arch/aarch64/include/arch/pt_regs.h`, `arch/arch.h`, `arch/platform.h` |
| **Supporting** | `kernel/include/kernel/cpu.h`, `kernel/include/kernel/vmm.h`, `kernel/mm/vmm.c` |
| **Boot/HAL/MMU** | Deferred to doc 02; not duplicated here. |
| **Build** | **[verified]** compiles clean both arches. |

---

## 1. Purpose & Role

This layer owns everything between hardware reset and the first C kernel function, and then
every privilege boundary crossing thereafter: hardware exception delivery, syscall entry/exit,
per-CPU data structures, FPU/SIMD context, and the safe kernel/user copy primitives (uaccess)
that every syscall relying on user pointers must call.

---

## 2. Architecture Summary

### 2.1 amd64 exception path

```
CPU → IDT gate (isr_stub_N) → common_isr_entry (isr_stubs.S)
       swapgs if Ring 3 → push 15 GP regs → amd64_isr_dispatch(pt_regs*)
          vec < 32: exception switch (8/13/14 specialised; all others: default halt)
          vec == 0x80: kernel_syscall_dispatcher
          vec 32-255: timer/hw-IRQ dispatch → lapic_eoi
       restore regs → swapgs if Ring 3 → addq $16 → iretq
```

```
User SYSCALL instruction → MSR LSTAR → syscall_entry (syscall.S)
       swapgs → save user RSP → load kernel RSP from %gs:16
       push iretq frame + vec/err + 15 GP regs → kernel_syscall_dispatcher
       restore → addq $16 → swapgs → iretq   (NOT sysretq)
```

### 2.2 aarch64 exception path

```
Hardware → VBAR_EL1 vector table (exception.S, 2KB aligned, 128B per entry)
           vector_stub macro → sub sp,#816 → save x0-x30, ELR/SPSR/SP_EL0
           save q0-q31 + FPSR/FPCR (full NEON context)
           → sync_handler / irq_handler / serror_handler
                  sync_handler: reads ESR_EL1 EC field
                     EC=0x15: syscall_handler → kernel_syscall_dispatcher
                     EC=0x20/21/24/25: data/insn aborts → terminate user / panic kernel
                     others: pr_err + panic
           restore NEON + GP regs → eret
```

---

## 3. What Works

- **[static]** amd64 `pt_regs` layout (`pt_regs.h`) and both assembly stubs
  (`isr_stubs.S:87-141`, `syscall.S:36-107`) are mutually consistent: the struct
  member order matches exactly the push/pop order in both files.
- **[static]** `cpu_info` GS offsets hard-coded in `syscall.S` (`%gs:16` for
  `stack_top`, `%gs:24` for `user_stack_tmp`) exactly match the struct field offsets
  computed from `cpu.h:13-19` (self=0, cpu_id=8, online=12, stack_top=16,
  user_stack_tmp=24). No off-by-one.
- **[static]** aarch64 `exception.S` saves the full NEON register set (q0-q31, FPSR,
  FPCR) — 512 bytes — on every exception entry. This is correct for a kernel that
  does not maintain per-process FPU lazy state.
- **[static]** aarch64 `arch_enter_user_mode` zeroes all 31 GP registers before
  `eret` (`exception.S:417-450`); amd64 `arch_enter_user_mode` zeroes all 14
  caller/callee GP regs before `iretq` (`context.S:72-86`). Neither leaks kernel
  register state to EL0.
- **[static]** `amd64_isr_dispatch` correctly issues `lapic_eoi()` for all hardware
  vectors 32-255 and also calls `pic_send_eoi` for the legacy PIC range 32-47
  (`idt.c:187-194`).
- **[static]** aarch64 probe-recovery: `sync_handler` increments `ELR_EL1` by 4 to
  skip the faulting instruction when `probe_in_progress` is set (`cpu.c:75-79`).
  This correctly recovers from the platform memory probe.
- **[static]** aarch64 `syscall.c:arch_copy_from_user` and
  `arch_copy_string_from_user` save and restore IRQ state and hold `mm_lock` around
  the TTBR0 switch, providing correct single-CPU serialisation.
- **[static]** MSR STAR value in `msr.c:53` (`(GDT_KERN_DATA<<48)|(GDT_KERN_CODE<<32)`)
  is correct: SYSCALL loads CS=0x08 (kernel code), SS=0x10 (kernel data); SYSRET
  would load CS=GDT_KERN_DATA+16=0x20 (user code), SS=GDT_KERN_DATA+8=0x18 (user
  data) — matching the GDT layout in `gdt.c:100-103` and `gdt_defs.h`.
- **[static]** GDT per-CPU isolation: each CPU has its own `gdt_raw[cpu_id]` and
  `tss_data[cpu_id]`; `gdt_set_rsp0` updates only the calling CPU's TSS (`gdt.c:136-141`).
- **[static]** aarch64 secondary-CPU startup (`start.S:154-189`) correctly configures
  MAIR, TCR, SCTLR before jumping to `kernel_secondary_main`.

---

## 4. Central Invariant / Context

**Vector 0 (divide-by-zero) on amd64 `make run -m 4G`** falls into the `default:` branch of
`amd64_isr_dispatch` (`idt.c:162-165`):

```c
pr_err("\n[C%d] Unhandled CPU Exception: %ld\n", arch_get_cpu_id(), vec);
amd64_dump_regs(regs);
arch_cpu_halt();  /* → cli; while(1) hlt */
```

There is **no recovery**. The kernel prints the register dump and halts permanently.
The ultimate cause is in the virtio-blk driver dividing by the queue size when `queue_size`
is 0 (a consequence of the BAR-mapping defect documented as AMMU-07 in doc 02, triggered
by the 1GB fallback from BOOT-01/02). The exception-dispatch path itself is correct for
the operation it performs; the bug is upstream in the boot/platform layer.

**User-fault vs kernel-fault discrimination (amd64):** The error-code `U/S` bit
(`regs->err & 4`) is printed by `amd64_page_fault_handler` but is never used to
branch — both user faults and kernel faults are treated identically: dump and halt.
There is no attempt to terminate the user process and continue, as is done on aarch64.

---

## 5. Findings

| ID | Sev | Kind | Location | Summary |
|----|-----|------|----------|---------|
| EXC-AMD64-01 | W2 | MISSING · STUB | `idt.c:121, 128-148` | amd64 memory-probe recovery block is dead/incomplete: `probe_in_progress` is declared (`idt.c:121`) but never set to `true` anywhere on amd64 (only aarch64 `platform.c:63` sets it). The block that would set `probe_failed` and advance RIP is also unimplemented (falls through to halt). The whole probe branch is unreachable dead code on amd64. |
| EXC-AMD64-02 | W3 | MISSING | `idt.c:83-101, 152-165` | amd64 does not distinguish user vs kernel faults for vectors 0, 1, 2, 3, 4, 5, 6, 7, and all others in `default:`. On a user-triggered divide-by-zero or invalid opcode the kernel halts rather than delivering a signal / terminating the process. aarch64 terminates the process and schedules. |
| EXC-AMD64-03 | W3 | BAD-IMPL | `apic.c:19-22` | LAPIC LINT0 configured as ExtINT (not masked) routes legacy PIC IRQ0 (timer) to the same vector 32 as the LAPIC periodic timer (`lapic_timer_setup` also programs vector 32, `apic.c:96`). Both fire on every tick → double `kernel_timer_tick` per interval. [inferred: if both LAPIC timer and PIC are active simultaneously] |
| EXC-AMD64-04 | W2 | BUG | `idt.c:46, 64` | `idt_initialized` is a plain `static int` with no `volatile` qualifier and no memory barrier. Secondary CPUs spin on `while (!idt_initialized)` (`idt.c:64`); without a barrier the C compiler / CPU may cache the stale zero indefinitely. |
| CPU-AMD64-01 | W3 | MISSING | `isr_stubs.S:95-109`, `idt.c:175-178` | Preemptive context switch (timer IRQ → `kernel_timer_tick` → returns a different task's frame) saves only 15 GP registers in `common_isr_entry`; no `FXSAVE`/`XSAVE` of SSE/AVX state. SSE is enabled at `cpu.c:36-39` (CR4.OSFXSR). On a timer interrupt between two tasks both using XMM registers (or compiler-autovectorised `memcpy`/`memset`), the preempted task's XMM state is overwritten by the next task → silent cross-process data corruption. (Cooperative `ctx_switch` is correct by SysV ABI: xmm regs are caller-saved.) |
| CPU-AMD64-02 | W2 | BAD-IMPL | `context.S:49-92` | `arch_enter_user_mode` immediately clobbers the kernel stack (writes `rdx` to RSP at line 50), discarding the return address before building the iretq frame. The comment acknowledges this. The function is correctly `__noreturn` so it does not matter functionally, but a stray `ret` or exception between line 50 and `iretq` would jump to garbage. |
| CPU-AMD64-03 | W1 | BAD-IMPL | `context.S:88-91` | `arch_enter_user_mode` omits `swapgs` before `iretq`. The comment says "skip it or configure it later". With GS base set to `cpu_info` by `cpu.c:51-52`, omitting `swapgs` means user space inherits kernel GS base — it can read `cpu_info` data via the `fs`/`gs` segment if userland sets up a base address that overlaps (only if SMAP/SMEP absent). Incomplete but noted. |
| SYS-AMD64-01 | W2 | PERF · REFINE | `syscall.S:105-107` | Fast-path syscall entry (`MSR LSTAR`) returns via **`iretq`** instead of `sysretq`. `iretq` is 20–50 cycles slower. Functionally correct; the comment calls it "more robust than sysretq" (avoiding the well-known SYSRET non-canonical-RIP #GP-in-ring0 hazard). The performance regression is real; the design choice is defensible. |
| SYS-AMD64-02 | W2 | BAD-IMPL | `syscall.S:59-77` | The frame pushed by `syscall_entry` duplicates `rcx` (saved as both `rcx` at offset and the iretq `rip` field) and `r11` (saved as both `r11` and `rflags`). These are valid for the iretq return, but any code that modifies `regs->rcx` or `regs->r11` for a different purpose while on the SYSCALL path will produce a surprise when the (dead) duplicate fields are also present in the frame. |
| SYS-AMD64-03 | W2 | REFINE | `idt.c:57-58` | `int 0x80` gate (DPL=3, `0xEE`) is installed in the IDT (`idt_init:58`), enabling the legacy 32-bit Linux syscall path from user space. Intentional or not, this is a second syscall surface. If the ABI goal is SYSCALL-only, this should be removed or documented. |
| SYS-AARCH64-01 | W2 | MISSING | `exception.S:282-283` (FIQ entries) | FIQ vectors for EL1-sp0 and EL1-spx are infinite loops (`b .`). For QEMU `virt` this is harmless, but on real hardware with GIC FIQs (e.g. secure firmware) these would freeze the core silently. |
| SYS-AARCH64-02 | W2 | BAD-IMPL | `cpu.c:129-131` | In `sync_handler`, when a `is_kernel_user_access_fault` is detected, the handler manually releases `current_process->mm_lock` and calls `local_irq_enable()` (`cpu.c:129-131`). This presupposes a specific lock discipline (that the faulting code always holds exactly this lock) which is fragile and undocumented; a future refactor of the uaccess critical section could easily miss this coupling. |
| UACC-AMD64-01 | W2 | DOC · MISSING | `uaccess.c:3,14,26,41` | The comment claims "Safe User Memory Access via SMAP/SMEP instructions (stac/clac)" but no `stac`/`clac` instructions are emitted anywhere. CR4.SMAP is not enabled (`cpu.c:36-39` sets only OSFXSR/OSXMMEXCPT). Since SMAP is currently off, no runtime fault occurs. The misleading comment overstates the protection provided. If SMAP is enabled in the future without adding `stac`/`clac`, every uaccess call will immediately trap. |
| UACC-AMD64-02 | W3 | SECURITY · TOCTOU | `uaccess.c:23-26` | `arch_copy_from_user`: `vmm_check_range` validates the mapping, then **without a lock** `memcpy` runs. On SMP another CPU or kernel thread could `munmap` / remap the page between the check and the copy, causing the copy to read a newly-freed or remapped page. The aarch64 implementation (`syscall.c:51-65`) correctly holds `mm_lock` and disables interrupts around the TTBR switch + copy; the amd64 version has no analogous protection. |
| UACC-AMD64-03 | W3 | SECURITY · TOCTOU | `uaccess.c:31-43` | `arch_copy_to_user`: same TOCTOU window as UACC-AMD64-02. The validation check at `uaccess.c:37` and the `memcpy` at `uaccess.c:41` are not serialised under any lock. |
| UACC-AMD64-04 | W3 | SECURITY | `uaccess.c:45-64` | `arch_copy_string_from_user`: page-boundary check at `uaccess.c:52-57` only re-validates when `(src_addr & 0xFFF) == 0` (on each new page). The very **first** byte of the string is not validated by `vmm_check_range` if it is not at a page boundary — only the starting-address `vmm_is_user_addr` check guards it. A user could point `src` to the last byte of a valid page so the initial check passes, but the string extends into an unmapped next page without triggering the boundary check (which fires at boundary, not before crossing). Also suffers from the same TOCTOU as UACC-AMD64-02. |
| UACC-AMD64-05 | W2 | BUG | `uaccess.c:19-20` | Overflow check `src_addr + n < src_addr` correctly detects most wrapping, but `vmm_is_user_addr(src_addr + n)` at line 20 is evaluated with the **wrapped** value when `n == 0` and `src_addr == 0` — `vmm_is_user_addr(0)` returns `false` (correctly), but the overflow check `0 + 0 < 0 = false` passes first. Edge case: `n == 0` with any `src` in user range returns 0 (success) from `vmm_check_range` without copying, which is correct. However, the combined semantics of `overflow_check || vmm_is_user_addr(src+n)` does not cover the case where `src_addr + n` exactly equals `0x0001_0000_0000_0000` (top of user VA) — the canonical boundary of `vmm_is_user_addr`. [static; edge-case, not immediately exploitable] |
| UACC-AARCH64-01 | W3 | SECURITY · BUG | `syscall.c:87` | `arch_copy_to_user`: `vmm_check_range` is called at line 87 with `current_process->page_table` **without first checking** `current_process != NULL`. The analogous `arch_copy_from_user` correctly guards with `if (!current_process || !current_process->page_table)` at line 43. A null-pointer dereference is possible if `arch_copy_to_user` is invoked from a kernel context without a current process. |
| UACC-AARCH64-02 | W3 | SECURITY · TOCTOU | `syscall.c:87-91` | `arch_copy_to_user`: `vmm_check_range` runs at line 87 **before** `spin_lock(&mm_lock)` at line 91. On SMP, between the range check and the lock+TTBR switch, another CPU could unmap the target pages. Fix: acquire `mm_lock` before calling `vmm_check_range`. |
| UACC-AARCH64-03 | W2 | BAD-IMPL | `syscall.c:112-149` | `arch_copy_string_from_user`: same first-byte boundary gap as UACC-AMD64-04 (initial byte not individually page-validated before the loop's boundary check). Also: `max_len == 0` is not guarded; `dest[max_len - 1]` at line 140 would underflow. The `ret = 0` is never set to a success indicator (only to -1 on boundary fail), so callers cannot distinguish "truncated" from "complete". |
| CPU-AARCH64-01 | W2 | REFINE | `cpu.c:110-111` | `sync_handler`: `is_kernel_user_access_fault` checks `vmm_is_user_addr(far)` where `far` is `FAR_EL1`. For data aborts, `FAR_EL1` correctly holds the faulting *data* address, so the test correctly classifies kernel-touches-user-range. However, a wild kernel pointer that happens to land in the user VA range (`[0x1000, 0xFFFF000000000000)`) would be misclassified: instead of panicking, the handler terminates the current user process and schedules, silently hiding the kernel bug. |
| CPU-AARCH64-02 | W1 | DOC | `exception.S:333-337, 348-352, 366-370` | EL1 and EL0 sync handlers contain dead debug scaffolding: `stp x0,x1,[sp,#-16]!` + `mov x0,#0x09000000` + `mov w1,#N` + `ldp x0,x1,[sp],#16`. The `str w1,[x0]` that would write to the PL011 UART is commented out. Net SP effect is zero (pre-decrement restored by post-increment), so there is no stack corruption. The dead code should be removed for clarity. |
| GDT-AMD64-01 | W2 | REFINE | `gdt.c:100-103` | User data descriptor (index 3, access `0xF2`) is placed **before** user code (index 4, access `0xFA`) in the GDT. The resulting selectors are: kern-code=0x08, kern-data=0x10, user-data=0x18, user-code=0x20. The STAR computation in `msr.c:53` and `pt_regs.h:99-107` consistently use 0x18/0x20, so the system is internally consistent. However the conventional x86_64 ABI ordering (kern-code, kern-data, user-code, user-data) is reversed for user segments, which is a source of confusion. |
| CPU-TRAMP-01 | W2 | BUG | `trampoline.S:76-81` | Trampoline GDT has only 4 entries (null, 32-bit code, 32-bit data, 64-bit code). After the long-jump to 64-bit mode (`ljmp $0x18`) the DS/ES/SS registers still hold `0x10` (the 32-bit data descriptor). In 64-bit mode data-segment descriptors are mostly ignored except for SS, but `0x10` is a 32-bit descriptor; some CPUs may enforce the `D` bit. No 64-bit data segment is defined in the trampoline GDT. [inferred; no runtime crash observed but architecturally unsound] |

---

## 6. Detailed Entries

### EXC-AMD64-01 — amd64 memory-probe recovery is silently broken `[static]`

**Evidence:** `idt.c:128-148`.

When `probe_in_progress` is true and a GPF (vec=13) or page fault (vec=14) arrives:

1. `probe_failed = true` is set.
2. Execution falls straight through — no `return`, no `break`, no RIP adjustment.
3. The `switch` statement below immediately matches case 13 (`amd64_gpf_handler`) or
   case 14 (`amd64_page_fault_handler`), both of which call `arch_cpu_halt()`.

The aarch64 counterpart (`cpu.c:75-79`) correctly increments `frame->elr += 4` and
returns the frame, allowing the probe to be reported as failed but continuing execution.
This divergence means the amd64 platform can never complete its memory-probe-based RAM
discovery via this path.

**Fix direction:** After setting `probe_failed = true`, compute the length of the
faulting instruction and advance `regs->rip` accordingly, then `return regs`. The
cleanest approach is a dedicated `fixup_table` (like Linux's exception table): the
probe assembly stub registers a fixup entry; the fault handler looks it up and jumps to
the recovery label. Alternatively, wrap the probe in inline assembly with a known
fixed-size instruction so a constant offset can be added.

---

### EXC-AMD64-02 — No user-process fault isolation on amd64 `[static]`

**Evidence:** `idt.c:83-101, 152-165`.

On aarch64, `sync_handler` (`cpu.c:109-138`) checks `is_user_fault` via `SPSR.M[3:0]`
and either terminates the user process or panics. On amd64 there is no equivalent
check. A divide-by-zero in a user ELF binary results in the kernel halting, rather than
the process receiving SIGFPE (or equivalent) and being cleaned up. This is both a
correctness defect and an availability hazard: any user program can freeze the entire
machine by executing `div $0`.

**Fix direction:** In `amd64_isr_dispatch`, for exception vectors 0–31, check
`regs->cs & 3` (copied from the CPU-pushed CS). If the fault came from Ring 3
(`cs & 3 == 3`), call `process_terminate` and `schedule` instead of halting. Only
fall through to `halt` for Ring 0 (kernel) faults.

---

### UACC-AMD64-01 — No SMAP enforcement; misleading comment `[static]`

**Evidence:** `uaccess.c:3,14,17-43`.

The file is titled "Safe User Memory Access via SMAP/SMEP instructions (stac/clac)"
and the comment body says "we only need to bypass SMAP (stac)". Neither `stac` nor
`clac` appears anywhere in the file. CR4.SMAP is not enabled by `cpu.c:36-39` (which
sets only OSFXSR and OSXMMEXCPT). So currently: SMAP is not active, therefore no
violation occurs, but the comment creates a false impression of a security guarantee
that does not exist. If SMAP is enabled in the future without adding `stac`/`clac`,
every `arch_copy_from_user` and `arch_copy_to_user` call will trap immediately.

**Fix direction:** Either (a) add `stac` before and `clac` after the `memcpy` calls,
enable CR4.SMAP in `cpu.c`, and update the comment to reflect the actual mechanism; or
(b) remove the SMAP claims from the comment and document the actual defence (range
check + page table walk).

---

### UACC-AMD64-02/03 — TOCTOU in amd64 copy_from/to_user `[static]`

**Evidence:** `uaccess.c:23-26` (copy_from_user), `uaccess.c:37-41` (copy_to_user).

The validation sequence is:
1. `vmm_check_range` — walks the page table and verifies PTE_VALID.
2. `memcpy` — executes the actual transfer.

On a uniprocessor this race is impractical, but on SMP any other CPU could call
`munmap` (or the kernel could reclaim a page) between steps 1 and 2. The result is a
`memcpy` into or from a page that is no longer mapped in the user address space, which
can corrupt kernel memory (copy_from) or write to a newly-allocated kernel page (copy_to).

aarch64 avoids this by acquiring `mm_lock` and disabling interrupts before switching
TTBR0, ensuring no concurrent modification (`syscall.c:51-73`). amd64 has no analogous
critical section around the `memcpy`.

**Fix direction:** In `arch_copy_from_user` and `arch_copy_to_user`, take
`current_process->mm_lock` (and save/restore IRQs) around the `vmm_check_range` +
`memcpy` sequence. The `memcpy` runs with CR3 already loaded with the user PML4, so
the page table switching step is not needed on amd64, only the lock.

---

### SYS-AMD64-01 — Fast-path syscall returns via iretq, not sysretq `[static]`

**Evidence:** `syscall.S:105-107`.

The kernel correctly sets up LSTAR (`msr.c:57`) and FMASK (`msr.c:62`) to enable
fast-path SYSCALL entry. On return, however, `syscall.S` executes `swapgs` + `iretq`
rather than `sysretq`. The `iretq` path is 20-50× more expensive than `sysretq`
because it pops and validates an entire 5-word hardware frame. More importantly, the
comment at line 105 says "more robust than sysretq for now" — this suggests a
conscious deferral. The fix is known and intentional, but the performance regression
and ABI mismatch (advertising SYSCALL, delivering INT) are real.

A precondition for sysretq: RCX must contain the return RIP and R11 must contain the
return RFLAGS. `syscall.S` saves `rcx` (return RIP) at line 65 and `r11` (return
RFLAGS) at line 73. After the dispatcher returns, these are popped back into `rcx` and
`r11` (lines 97-98). The path to `sysretq` would require: remove the `pushq $0x1B`
/ `pushq %gs:24` / `pushq %r11` / `pushq $0x23` / `pushq %rcx` iretq frame pushes
(lines 52-56), and replace the tail with `swapgs; sysretq`.

---

### UACC-AARCH64-01 — Missing null check in arch_copy_to_user `[static]`

**Evidence:** `syscall.c:87` (aarch64).

`arch_copy_from_user` at line 43 guards: `if (!current_process || !current_process->page_table) return -1;` before dereferencing `current_process->page_table`. `arch_copy_to_user` skips this guard and dereferences `current_process->page_table` at line 87 unconditionally. If called from a kernel thread or interrupt context where `current_process` is NULL, this is an immediate null-pointer dereference. Given the aarch64 kernel currently uses `current_process` as a global `struct process *`, any context that calls `arch_copy_to_user` without a user process will crash.

---

### CPU-AMD64-01 — No FPU/SSE context save in ctx_switch `[static]`

**Evidence:** `context.S:12-36`, `cpu.c:29-39`.

`cpu.c:arch_cpu_init` enables SSE with `OSFXSR` and `OSXMMEXCPT` in CR4. GCC is free
to use XMM registers for `memcpy`, `memset`, and auto-vectorised loops anywhere in the
kernel. `ctx_switch` saves only: rbx, rbp, r12-r15, rsp, rip — the System V ABI
callee-saved integer registers. No `FXSAVE`/`FXSAVE64`/`XSAVE` instruction appears.

If two kernel threads both touch XMM registers (directly or via inlined `memset`), a
context switch between them will silently restore incorrect XMM state from the next
process's stack slot (which contains whatever the processor left there). This is a
silent data-corruption hazard under any non-trivial workload.

**Fix direction:** Add `FXSAVE` / `FXRSTOR` (or `XSAVE`/`XRSTOR` with proper state
compaction) to `ctx_switch` around the callee-saved register block. Alternatively,
implement FPU lazy-save: keep a per-CPU "last-FPU-owner" pointer and trap
`#NM` (vec=7, CR0.TS) to save/restore only on actual FPU use. The aarch64 path
already does eager NEON save on every exception (`exception.S:57-78`).

---

## 7. Refactor Direction (toward coherent ABI + seL4-style kernel/user boundary)

### 7.1 Syscall ABI unification

| Issue | amd64 | aarch64 |
|---|---|---|
| Entry mechanism | SYSCALL → LSTAR → `syscall.S` | SVC → EL1 exception → `exception.S` |
| Return mechanism | `iretq` (slow, wrong) | `eret` (correct) |
| Syscall number | RAX | X8 |
| Arguments | RDI,RSI,RDX,R10,R8,R9 (Linux x86_64 ABI) | X0..X5 (AArch64 ABI) |
| Frame abstraction | `pt_regs.h` accessors (`pt_regs_arg`, `pt_regs_set_return`) | same |

The `pt_regs_*` accessors correctly abstract the ABI differences. The remaining goal is
to make the amd64 exit path use `sysretq` (SYS-AMD64-01) and to confirm that
`kernel_syscall_dispatcher` never accesses raw register fields that differ between
arches.

### 7.2 Safe kernel/user boundary (uaccess)

The current state:

- **aarch64**: TTBR0 switch under `mm_lock` + IRQ-disable — correct but heavyweight
  (two TLB flushes per copy). For a higher-half kernel with ASID tagging this could be
  reduced to a single TTBR0_EL1 write with ASID separation.
- **amd64**: No lock, no SMAP, TOCTOU window. Must be fixed before any production syscall
  that copies user data is trustworthy.

Target state for seL4-style isolation:
1. Define a canonical user VA range (`vmm_is_user_addr`'s `[0x1000, 0xFFFF000000000000)` is
   already arch-neutral in `vmm.h:109-112`).
2. amd64: Add `mm_lock` + IRQ save around `vmm_check_range + memcpy` in all three copy
   functions; then enable CR4.SMAP and add `stac`/`clac` bracketing.
3. Both arches: propagate `access_ok`-style checks (the existing `vmm_is_user_addr` call)
   to the syscall dispatcher level, not just inside copy functions.

### 7.3 Exception isolation

- amd64: Add user-vs-kernel fault discrimination in `amd64_isr_dispatch` (EXC-AMD64-02).
- amd64: Implement probe fixup table or fixed-size probe stubs (EXC-AMD64-01).
- amd64: Add `FXSAVE`/`FXRSTOR` to `ctx_switch` (CPU-AMD64-01).
- Both arches: Document that `arch_cpu_halt` on a kernel exception does not stop
  sibling CPUs; add an IPI broadcast to fence all cores before the halt.

### 7.4 GDT / descriptor hygiene

- Move user-code (0x20) before user-data (0x18) to match conventional ordering, or
  document why the current order is intentional (GDT-AMD64-01).
- Add a 64-bit data segment to the trampoline GDT (CPU-TRAMP-01).

---

## 8. Verification Notes

- Exception dispatch correctness (isr_stubs.S frame layout, pt_regs struct alignment,
  GS offset arithmetic): **[static]** — verified by cross-referencing assembly push/pop
  sequences against struct field order and manual offset arithmetic.
- Vector 0 on amd64 → `default:` → halt: **[verified]** at runtime (serial output
  "Unhandled CPU Exception: 0", then silence); path traced **[static]** through
  `isr_stubs.S → amd64_isr_dispatch`.
- SMAP not enabled (CR4.SMAP not set in `cpu.c`): **[static]**.
- TOCTOU in uaccess: **[inferred]** (requires SMP and concurrent `munmap` — not
  observable on a uniprocessor run, but the code path is clear from the source).
- FPU context gap in `ctx_switch`: **[static]**; may not manifest under QEMU `-smp 1`
  if GCC happens not to use XMM registers in the kernel paths exercised.
- aarch64 null dereference in `arch_copy_to_user` (UACC-AARCH64-01): **[static]**;
  not triggered in the current boot path where a user process is always active during
  syscalls.
