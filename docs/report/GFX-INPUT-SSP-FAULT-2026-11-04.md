# AArch64 input-time kernel fault and SSP evidence — 2026-11-04

**Status: OPEN — reproduced once in an interactive session; root cause not established.**

This is a second AArch64 stack-protector incident, distinct from the earlier compositor_window_write() epilogue incident. It must not be merged into that diagnosis or classified as a VFS failure.

## 1. Runtime and preservation

- QEMU: qemu-system-aarch64, machine virt, Cortex-A57, SMP=4, 5 GiB RAM.
- Boot: UEFI AArch64 image, GRUB EFI; VirtIO GPU, keyboard, mouse and block device.
- Guest boot session PID: 86631; started 2026-11-03T23:03:50Z.
- Interactive testing used tools/nxadb.py shell, mouse, screenshot and status commands.
- At capture, nxadb status reported faults=3. This is the number of regex matches in accumulated serial text, not three independently established kernel crashes. The preserved serial contains one final panic sequence.
- QEMU remained alive with the guest halted; the runtime was stopped only after preserving serial, QEMU stdout/stderr, PID manifest and panic screenshot.

Evidence directory:

docs/report/logs/GFX-runtime-fault-aarch64-20261104-001123/

It contains the complete 76 KiB serial log, QEMU stdout/stderr, QEMU PID/command manifest and a verified 1280×800 screenshot captured after the panic. The screenshot shows the guest display at the exact interrupted input line.

## 2. Interactive sequence and corrected trigger classification

Before the fault, the following userland operations were exercised: nxinfo, nxmemstat, nxproc, nxwins, nxperm, nxpower -cli status, nxsettings -cli display/system/proc/mem/notify, nxres (read-only usage/status), nxreg, ls /, cd /reg, pwd, and ls /reg. The /reg directory listing returned the namespaces background mouse srv style sys system theme.

nxfilem was launched from NXShell and displayed the root/home file listing. Mouse movement and a window-close interaction were exercised; the shell returned to focus. nxproc and nxreg were then used to inspect process and registry state.

The last attempted command was cat /reg/system.version. **It was not executed.** The panic screenshot shows the NXShell prompt still containing the partial line cat /reg/system.ver with the cursor at its end. Therefore this event is classified as an input-time fault while characters were being entered, not a failure in cat, /reg, VFS file reads, or the registry read operation. The command text is merely the last visible interaction and is not evidence of the failing syscall.

The same runtime's nxproc output showed idle tasks PID 32000–32003, with PID 32002 assigned to CPU 2. The panic dump identifies current process PID 32002. This correspondence is evidence to investigate, not proof that the idle task caused the corruption.

## 3. Kernel evidence

The complete serial panic sequence records:

- SSP: Stack Smash at 0xffff0000400a0ab4
- ELR_EL1 = 0x0000000000000000
- FAR_EL1 = 0x0000000000000000
- ESR_EL1 = 0x0000000086000004 (EC=0x21, ISS=0x4)
- SPSR_EL1 = 0x0000000020000305
- current task PID 32002
- per-CPU fault frame 0xffff0000408d30d0, reported faulting SP 0xffff0000447a2a20
- panic diagnosis: kernel instruction abort / jump to NULL, followed by the SSP failure report.

The strings in the panic banner are visibly corrupted/interleaved in the serial and screenshot; the serial contains two panic banners in the same final exception sequence. The NXADB counter of 3 is explained by its regex matching two KERNEL PANIC banners plus the phrase Unrecoverable kernel, not by three independent faults. Do not infer that the text damage itself is the root cause; preserve it as another symptom. The current panic() implementation increments panic_flag but does not elect a single printing CPU: every concurrent caller continues through the lock-free output path. A multi-CPU panic race is therefore a concrete hypothesis for the duplicated/interleaved banners and must be closed with first-panic ownership before trusting future multi-CPU panic transcripts.

## 4. Symbol and stack-frame inspection

The AArch64 image used by this runtime maps the SSP return address to sys_registry:

- sys_registry starts at 0xffff0000400a0700.
- the call to __stack_chk_fail is at 0xffff0000400a0ab0; its return address is 0xffff0000400a0ab4 (sys_registry + 0x3b4).
- the disassembled prologue allocates 272 bytes and stores the saved global canary at [x29, #264].
- k_key and k_val occupy stack ranges beginning at x29+0x48 and x29+0x88 respectively; their declared sizes are 64 and 128 bytes.
- registry_get() bounds its copy to size-1 and writes a terminator at size-1; the source-level read path does not by itself establish an overflow.

The call-site is the shared SSP epilogue for sys_registry; it does not identify which operation (ENUM, READ, WRITE, or DEL) was active. Registry operations also occur in background services. Since the visible shell command was still being typed, the faulting sys_registry frame may belong to a concurrent task that was interrupted during the input activity. The caller PID, caller frame pointer and saved canary were not captured in this run.

## 5. Competing hypotheses — none proven

1. **Kernel-stack/context-switch lifetime or frame corruption.** kernel_timer_tick() invokes schedule(regs) on every CPU timer IRQ. The scheduler supports preemption of interrupted kernel execution; its pending_reenqueue/deferred-reap protocol is intended to prevent a task stack from being reused before exception return. Audit that protocol specifically for EL1-interrupted frames and concurrent SMP work stealing.
2. **Input/event dispatch interaction.** keyboard.c currently owns modifier state, US/IT key maps, ASCII/UTF-8 conversion and focused-PID IPC. IRQ providers enqueue raw events, but PID 1 drains and dispatches them through SYS_COMPOSITOR_RENDER. The observed failure happened while typing, so correlate input ring operations, focus, IPC and timer scheduling.
3. **Registry syscall stack corruption.** The SSP call-site is in sys_registry, so audit all enum/read/write branches and the strict user-string-copy implementation. Do not assume this is the source merely because the symbol is named sys_registry.
4. **Separate stack/return-state corruption.** ELR=0 and the panic's corrupted text indicate the SSP report may be secondary to a broader context/stack failure. Preserve the original exception frame before any nested panic path can overwrite diagnostic context.
5. **Concurrent panic reporting.** panic() currently has no first-caller election; a second CPU can enter panic and print concurrently. The duplicated banners may be a consequence of concurrent fatal paths, not necessarily a second independent failure.

The earlier AArch64 SSP incident at compositor_window_write() remains a separate open observation; it does not prove that compositor rendering caused this event.

## 6. Required instrumentation before another stress run

- Extend __stack_chk_fail() with a no-allocation, no-lock diagnostic record: return PC, caller FP/LR, CPU ID, current PID/name, kernel-stack bounds, global guard, and whether the caller FP is inside the current task's stack. Read the caller's saved canary only after validating the frame against stack bounds; do not hard-code a single canary offset for unrelated functions.
- Preserve the first exception's full pt_regs/ESR/FAR/SPSR/ELR and per-CPU fault state before invoking panic() or another stack-protected function. Avoid recursive SSP failure in the diagnostic path.
- Add first-panic CPU ownership with an atomic compare-and-swap: only the winner emits the full banner/trace/screen; secondary panic callers record a minimal CPU marker if safe and halt without competing UART output.
- Add a small per-CPU scheduler transition ring recording prev_pid, next_pid, interrupted PC/SP/SPSR, stack bounds, pending_reenqueue, deferred-reap state and whether the IRQ interrupted EL0 or EL1. Dump it only on fatal faults.
- Expose or log input_ring_dropped and kbd_dropped_events with CPU/task context; distinguish ring overflow from lost IPC delivery.
- Add a monotonically increasing input-event sequence and bounded trace (provider/type/code/value/focus PID/dispatch CPU) that can be correlated with scheduler trace without printing every event on UART.

## 7. Reproduction and fix gates

1. Add the diagnostic instrumentation and compile it on both architectures; verify that the fault path itself has no stack protector recursion or unsafe dereference.
2. Run clean AArch64 boot and use NXADB to type short and long shell commands containing /, ., digits, punctuation, Shift, Caps Lock, Ctrl and repeated keys while services are active.
3. Repeat with mouse motion, focus changes, window open/close and simultaneous registry reads; preserve full serial/QEMU logs and screenshots per run.
4. If the caller FP is outside the current task stack, audit scheduler task ownership, stack aliases, pending_reenqueue, deferred free and EL1 IRQ restore before changing registry or VFS.
5. If the caller FP is inside the expected stack, compare the saved canary and nearby stack words against the global guard and inspect the exact branch/locals in the disassembly.
6. Only after a writer/race is proven, implement the minimal correction and add a regression test. Then run clean make test-all on AMD64 and AArch64 plus repeated interactive input/window stress. A passing soak without reproducing the panic is not a fix.

## 8. Related source and architecture decisions

The input migration is already planned: the kernel currently translates scancodes/layouts/characters and routes pointer events; the disabled kernel-thread prototype is explicitly not the target. The intended end state is a supervised userland nxinput service, with raw device event delivery separated from layout, character semantics and policy. The kernel must retain only the minimum mechanism needed for interrupt delivery, device ownership and safe event transport until the driver/service capability model is ready.

The compositor/surface split and the terminal-lock/SSP investigation remain gated by docs/architecture/GRAPHICS-USERLAND-MIGRATION.md and docs/report/GFX-LEGACY-TERMINAL-CONCURRENCY-2026-11-03.md. Do not move code or remove IRQ-safe locking as a response to this incident.
