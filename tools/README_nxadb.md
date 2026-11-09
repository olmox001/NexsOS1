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

  -a, --arch ARCH            Architecture: aarch64 (default) or amd64
  --boot-mode {iso,direct}   GRUB media or direct QEMU -kernel diagnostics
  --firmware {auto,bios,uefi}
                             ISO firmware. amd64 auto=bios; aarch64 auto=uefi
  --build-root PATH          Isolated project-relative build namespace


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

  # AMD64 BIOS and UEFI are distinct, independently captured lanes
  nxadb -a amd64 --boot-mode iso --firmware bios test -n 1
  nxadb -a amd64 --boot-mode iso --firmware uefi test -n 1

  # Direct-kernel diagnostic lane
  nxadb -a amd64 --boot-mode direct test -n 1

  # AArch64 ISO is always UEFI
  nxadb -a aarch64 --boot-mode iso --firmware uefi test -n 1

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

- QEMU lifecycle is tracked per lane. BIOS, UEFI and direct boots have
  distinct PID manifests, QMP sockets and log directories. Stop only targets
  the recorded process after verifying its command line; unrelated QEMU
  processes are not killed.
- QMP's `input-send-event.device` selects a display device, not the keyboard
  device. nxqemu assigns `nxgpu0` to the virtual GPU and binds both input
  devices to it so injected key events reach the guest console.
- Mutating nxadb operations share one lock per architecture/build-root.
  This intentionally serializes BIOS, UEFI and direct lanes because they share
  the same writable rootfs image; one lane cannot race another over disk/QMP.


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

Visual inspection and pointer input
-----------------------------------

  nxadb start --display default       Start QEMU with a visible host window
  nxadb start --display none          Headless (default; QMP screenshots still work)
  nxadb screenshot                    Capture VirtIO-GPU head 0 as verified PNG
  nxadb screenshot gui-after-click.png
                                      Save under build/runs/<arch>/<lane>/
  nxadb mouse <dx> <dy>               Move guest relative pointer
  nxadb mouse <dx> <dy> --click       Move and left-click
  nxadb mouse <dx> <dy> --click --button right
                                      Right-click after moving

Screenshots are captured from QMP device `nxgpu0`, not the first/default VGA
adapter. This matters on AMD64 where firmware VGA and VirtIO-GPU are separate
display devices. Each PNG has a JSON manifest and raw PPM alongside it.
The tool refuses to overwrite an existing capture and constrains output paths
to `build/runs/<arch>/<lane>/`.

QMP typing uses a conservative 75ms per-key delay to avoid lost characters
when the guest is under rendering load. Mouse press/release edges are sent as
separate QMP events so a short click is not collapsed into one input frame.


Boot capture and visual hardening (2026-11-03)
---------------------------------------------

Every QEMU start preserves the previous runtime UART/stdout/stderr files under
build/runs/<arch>/<lane>/boot-<timestamp>-<id>/ and writes a persistent
run-*.json manifest containing architecture, boot mode, firmware, lane, exact
QEMU command and log paths. QEMU stdout is no longer discarded. The current
complete channels are:

  build/runtime/nxadb/<arch>/<lane>/serial.log
  build/runtime/nxadb/<arch>/<lane>/qemu.stdout.log
  build/runtime/nxadb/<arch>/<lane>/qemu.stderr.log

Lane names are `iso-bios`, `iso-uefi` and `direct`. AArch64 rejects BIOS;
AMD64 has independent BIOS and UEFI ISO lanes. AMD64 UEFI uses OVMF/EDK2
through the shared nxqemu firmware discovery and does not require a DTB.

Use nxadb bootlog to print all captured channels and the latest manifest.
Use nxadb bootlog --history to list previous archived captures. logcat
continues to follow the guest UART only. Error messages may show a bounded
last-6000-character excerpt, but explicitly identify it as an excerpt; the
full log remains on disk.

nxadb test --soak-seconds 30 --display default performs a real QEMU run:
baseline screenshot, NXShell input, nxwins window enumeration, focus of the
active shell window, 30-second process/fault supervision and final screenshot.
The screenshots are stored under build/runs/<arch>/<lane>/ and verified as PNGs.
make test runs the selected architecture; make test-all runs amd64 and
aarch64 sequentially. Both release architectures must pass independently.

A successful gate requires every KTEST case to pass, a real NXShell command
before and after the liveness probe, timer diagnostics, advancing counter
heartbeat within its bounded timeout and zero kernel-fault matches. Optional
visual soak adds nxwins/focus and verified screenshots. AMD64 BIOS, AMD64 UEFI,
AMD64 direct, AArch64 UEFI and AArch64 direct are qualified independently;
adding a new lane never inherits evidence from another lane.
