# B3 polish & TTY queue — archived to resume after the release/storage phase

> **Why this file**: work paused mid-queue to pivot to the release/boot/storage
> rework (see `docs/MICROSCOPE-RELEASE.md`).  This captures the agreed items so
> they can be resumed without re-deriving context.  Branch `comprehensive-review`.

## Done (landed)
- **Controlling-terminal TTY model** (`33cb8cd`, USR-TTY-01 #123): windowless
  programs run in the launching shell (`ctty_win`, own-window-first routing);
  Ctrl+C delivered through IPC (was a dead `kb_buffer`) and turned into a kill
  by the shell's `run_foreground`; `SYS_WINDOW_OF_PID` (218); `/bin/hello`
  demo.  Verified both arches.
- Earlier B3: 4-level privilege + capabilities (#79), legacy purge + window
  ABI (#123 legacy half), IPC-01 (#85), reparenting/descendant-kill (#122).
  Epic #93 closed.

## Deferred (decided, not started)
1. **Process-manager priority bound** — "a separate process scales priority vs
   its parent by at most a max %".  Decision: **deferred to B5** as part of
   **SCHED-01 #83** (the scheduler↔compositor focus-boost decoupling).  Today
   `schedule()` calls `compositor_get_focus_pid()` and gives the focused PID
   ABSOLUTE first-pick (`process.c:~1206`).  The bounded-boost / parent-relative
   cap is to be designed there.  Symptom it addresses: "doom stops when the
   shell is backgrounded".
2. **Per-process stdout log + `log <pid>`** — kernel keeps a **4 KB kmalloc
   ring buffer per process** (freed at teardown) capturing stdout; ONLY when
   logging is enabled for that process (opt-in, to save perf).  `log <pid>`
   in the shell prints the last lines (sized to the shell window) and streams
   until Enter, AND tees to a VFS file `sys/log/<DATE_TIME>.log`.  When a
   foreground child's window CLOSES, the shell prints the last ~10 lines of its
   log.  The same opt-in gating is later applied to the UART mirror and printf
   (so release builds don't pay for debug output).  Window-close-tail in
   `run_foreground` depends on this buffer.

## Queue (well-defined, to execute after the release phase)
3. **Secondary optimizations** (TBD as found during 2/4).
4. **Update all userland programs + libraries**, including the **doom port:
   the FIRE key does not work** (input mapping bug in the doom port — check
   `user/bin/doom/` key handling vs the kernel keycode/IPC INPUT format).
5. **Keyboard layout `mac19ita`** (MacBook Pro 2019 16" Italian) added and set
   as the **default**; new shell command to **view and switch** available
   layouts.  Layout tables live in `kernel/drivers/keyboard/` (`layout_us`,
   `current_layout`, `utf8_overrides`).
6. **Shell inline-command support** for programs that opt in (a uniform way to
   pass args/commands to a spawned program — today `spawn()` passes no argv).
7. **Documentation** refresh before the next phase.

## Key facts (so the resume is fast)
- Kernel debug path is clean/separate: `pr_*→printk→kputs→uart_putc`, gated by
  `console_loglevel`.  No bare `printf` in the kernel.
- Userland output: `printf→write(1)→FD_WIN→window_text_write` → own window
  (by PID) else `ctty_win`, and mirrors UART unconditionally (to be gated, see 2).
- `window_text_write` (in `syscall_dispatch.c`) is the single sink for both the
  FD_WIN stdout path and `SYS_WINDOW_WRITE` — the natural place to tee the
  per-process log (item 2).
- Ctrl+C = ETX 0x03, delivered via IPC INPUT (`keyboard.c`); shell parses it in
  `run_foreground`.
- `process_wait` is non-blocking (reporter); foreground loops poll + `yield`.
