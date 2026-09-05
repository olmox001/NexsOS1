nxadb — Primitive ADB for NeXs OS
=====================================

nxadb is a command-line tool that provides an ADB-like interface for
interacting with a NeXs OS guest running under QEMU.
It is built on top of the existing nxshell (especially nxshell -c) and
uses the guest’s native commands (ls, cat, write, echo, redirections, etc.).


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
  nxadb shell "<command>"  Run a single command via nxshell -c and print
                           cleaned output

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
  nxadb test -c captest -c libctest -e captest=64 -e libctest=18

  # Multi-run smoke test on amd64
  nxadb -a amd64 test -n 3

  # Status & shutdown
  nxadb status
  nxadb stop


Design Notes
------------

- One-shot commands always go through nxshell -c "…". This gives real exit
  status propagation and correct handling of builtins, pipelines,
  && / || / ;, redirections, etc.

- Output capture waits for the real NXShell prompt (NXShell:…>) and log
  stability instead of using fixed timeouts or naïve size checks. This
  greatly reduces truncated output.

- Interactive shell uses full QMP key mapping (including Left/Right/Up/Down,
  Backspace, Delete, Home, End).

- File transfer uses only commands that already exist in the guest:
    pull  → cat
    push  → rm + write (small files) or line-by-line echo "…" >> file
            (larger files)

- The tool automatically kills leftover QEMU instances, uses short paths for
  the QMP socket (AF_UNIX limit), and builds the DTB on aarch64 when needed.


Exit Codes
----------

  0          success
  non-zero   boot failure, test failure, command failure, or user interrupt (130)


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