nxadb — Primitive ADB for NeXs OS
=====================================

nxadb is a command-line tool that provides an ADB-like interface for
interacting with a NeXs OS guest running under QEMU.
It sends keyboard input through QEMU's QMP interface to the active NXShell
prompt and uses the guest's native commands (ls, cat, write, echo, redirections,
etc.). QMP input is routed to the named virtual display device `nxgpu0`; the
keyboard and mouse devices are explicitly bound to that display. One-shot
command status is inferred from the resulting prompt and output;
it is not a host-side process exit status from a separate nxshell process.


Quick Start
-----------

  # Build the guest first, then:
  ./tools/nxadb start                  # Boot the guest and wait for NXShell
  ./tools/nxadb shell                  # Interactive shell
  ./tools/nxadb shell "ls /sys/bin"    # One-shot command
  ./tools/nxadb logcat                 # Follow serial log
  ./tools/nxadb stop                   # Shut down


Global Options
--------------

  -a, --arch ARCH     Architecture: aarch64 (default) or amd64


Commands
--------

Lifecycle
~~~~~~~~~

  nxadb start              Start QEMU, wait for kernel boot and NXShell
  nxadb stop               Stop the guest and clean up
  nxadb restart            Stop + start
  nxadb status             Show current state (running, PID, shell presence,
                           faults, log size)
  nxadb wait-for-boot      Block until the kernel prints the boot marker
  nxadb wait-for-shell     Block until NXShell is alive

Shell & Execution
~~~~~~~~~~~~~~~~~

  nxadb shell              Interactive shell (supports arrows, backspace,
                           delete)
  nxadb shell "<command>"  Type a single command at the active NXShell
                           prompt and print cleaned output

File System Helpers
~~~~~~~~~~~~~~~~~~~

  nxadb ls [path]          List directory (default: .)
  nxadb cat <path>         Print file contents
  nxadb push <local> <remote>
                           Copy host → guest
                           (uses write for small files, echo … >> for larger ones)
  nxadb pull <remote> <local>
                           Copy guest → host
                           (uses cat + automatic cleaning of prompts/echoes)

Process & Debugging
~~~~~~~~~~~~~~~~~~~

  nxadb logcat             Show serial log (follows by default)
  nxadb logcat --no-follow Show serial log once and exit

  nxadb test [options]     Full test gate (replacement for the old nxrun.sh)

  test options:
    -c <cmd>               Command to run inside the guest (repeatable).
                           Commands ending with "test" are treated as gated tests
    -e <name>=<N>          Pin expected total for a test (e.g. -e captest=64)
    -n <runs>              Number of consecutive runs (default: 1)


Examples
--------

  # Boot
  nxadb start

  # Interactive session
  nxadb shell

  # One-shot commands
  nxadb shell "cd /tmp && mkdir test && ls"
  nxadb ls /sys/bin
  nxadb cat /home/.nxshell_history

  # File transfer
  nxadb push README.md /home/README.md
  nxadb pull /home/README.md ./from-guest.md

  # Follow the serial console
  nxadb logcat

  # Run the full test suite
  nxadb test -c captest -c libctest -e captest=64 -e libctest=20

  # Multi-run smoke test on amd64
  nxadb -a amd64 test -n 3

  # Status & shutdown
  nxadb status
  nxadb stop


Design Notes
------------

- One-shot commands are typed into the active NXShell session through QMP.
  This preserves the guest shell's own parsing and builtins. The host infers
  success/failure from prompt return and recognized error output; arbitrary
  guest exit codes are not yet transported as a separate structured result.

- Output capture waits for the real NXShell prompt (NXShell:…>) plus a short
  settle interval. It also returns immediately on a detected kernel fault;
  periodic guest diagnostics cannot keep the prompt detector waiting forever.

- Interactive shell uses full QMP key mapping (including Left/Right/Up/Down,
  Backspace, Delete, Home, End).

- File transfer uses only commands that already exist in the guest:
    pull  → cat
    push  → rm + write (small files) or line-by-line echo "…" >> file
            (larger files)

- QEMU lifecycle is tracked by a per-architecture PID file and unique QMP
  socket. Stop only targets the recorded process after verifying its command
  line; unrelated QEMU processes are not killed.
- QMP's `input-send-event.device` selects a display device, not the keyboard
  device. nxqemu assigns `nxgpu0` to the virtual GPU and binds both input
  devices to it so injected key events reach the guest console.
- Mutating nxadb operations are serialized by a per-architecture file lock,
  preventing simultaneous test runs or shell commands from racing over one
  guest console and serial log.


Exit Codes
----------

  0          success
  1          boot failure, test failure, or command failure
  75         busy: another mutating operation owns this architecture's session
  130        user interrupt


Limitations
-----------

- Binary file push is not yet supported (text only).
- Very large files are transferred line-by-line and can be slow.
- Interactive editing keys work, but complex multi-key sequences or
  non-ASCII input may still be imperfect.
- The serial log contains the echo of typed commands; nxadb tries to clean
  them automatically.


nxadb turns the existing NXShell into a practical, scriptable remote
interface for NeXs OS development and testing.