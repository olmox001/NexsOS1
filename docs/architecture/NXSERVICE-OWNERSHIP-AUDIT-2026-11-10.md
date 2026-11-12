# NX service, extension and SDK ownership audit

Date: 2026-11-10. Branch: DEV-Resurrection.
Status: source/build inventory and preliminary compilation. Not complete Power12
qualification, cross-architecture acceptance or a claim of kernel-wide VFS
removal.

## Machine and system process roles

| Actual implementation | Source | Installed ELF | Authority / role |
| --- | --- | --- | --- |
| nxmanageDD | kernel/user/extension/nxmanageDD/main.c | /krnl/extension/nxmanageDD | kernel-created PID1 MACHINE, object/capability supervisor |
| nxinit | user/sbin/nxinit.c | /sbin/nxinit | ROOT initialization policy child, reads manifest and registers services |
| nxntfy_srv | user/sys/services/nxntfy_srv.c | /sys/services/nxntfy_srv | ROOT notification IPC server and popup UI |
| nxexecd | user/sys/services/nxexecd.c | /sys/services/nxexecd | ROOT execution RPC server using shared resolver, bounded process ownership |
| nxtimerdd | user/sys/services/nxtimerdd.c | /sys/services/nxtimerdd | ROOT timer resource/PORT telemetry server |
| nxpaxd | user/sys/services/nxpaxd.c | /sys/services/nxpaxd | ROOT privileged diagnostic broker with sanitized replies |
| nxcompositor | user/sys/services/nxcompositor.c | /sys/services/nxcompositor | ROOT surface resource compositor |
| nxkeyboardd | user/sys/services/nxkeyboardd.c | /sys/services/nxkeyboardd | ROOT keyboard input router |
| nxmoused | user/sys/services/nxmoused.c | /sys/services/nxmoused | ROOT pointer/gesture/capture router |
| nxexec | user/sys/bin/nxexec.c, nxexec.h | /sys/bin/nxexec | hosted command UI, shared resolver, no server personality |
| nxpax | user/sys/bin/nxpax.c | /sys/bin/nxpax | diagnostic query command, no privileged broker resource |
| nxenvinit | user/sys/bin/nxenvinit.c | /sys/bin/nxenvinit | one-shot environment setup, supervised by manager |

Other nx* binaries (not standalone RPC daemons) are nxbar, nxfilem
(seven source modules), nxfont, nximage, nxinfo, nxlauncher, nxmemstat,
nxnotify, nxperm, nxpower, nxproc, nxreg, nxres, nxsettings, nxshell,
nxtestrunner, nxtimerdiag, nxtop, nxui and nxwins. A persistent GUI
program can be supervised without being a service-protocol implementation.

## System SDK modules audited

The original five client translation units in system/lib/extensions/services are
client-side protocol implementations, not server processes (none defines
an executable main function):

- buffer_client.c owns buffered resource acquisition/map/unmap wrappers.
- execsvc_client.c owns the port/pipe request framing, reply decoding,
  current fallback policy and POSIX client integration. The actual RPC
  request handler lives in user/sys/services/nxexecd.c.
- notify_client.c owns severity mapping, endpoint discovery and notification
  client IPC. Popup, deduplication and notifications rendering live in
  user/sys/services/nxntfy_srv.c.
- surface_client.c owns SDK surface, input-resource and display-object
  client wrappers. nxcompositor, nxkeyboardd and nxmoused own server loops.
- time_client.c owns the versioned time telemetry client. nxtimerdd owns
  server-side resource acquisition and reply construction.

System/lib/gfx contains common raster/chrome/decoration/theme algorithms.
Important: gfx/theme.c ALSO owns per-process active style/palette/background
settings and a generation counter. This is not a globally authoritative
theme service. Cross-process coherence and migration of mutable policy to
the compositor require a versioned registry/IPC design and real tests;
moving the file would break current SDK consumers and is not justified.

System/lib/common, lib.c, malloc, math, fonts, image/video extensions and
SDL/Lua/D3D/OpenGL portability modules remain SDK library code. Source
ownership is not determined by whether a function mentions a service name.
The build currently links the common USER_LIB_O set into every userland ELF,
and link-size/performance qualification must examine this without stripping
necessary functionality.

## Object and capability boundaries

Only the K3 composition root in kernel/main.c has been decoupled from
direct VFS imports: kernel/fs/object_boot_provider.c owns on-image boot
entry conversion to a protected registry key, and
kernel/core/object_boot.c resolves that object and starts one initial
extension. The rest of NX-CORE still contains existing VFS providers.
The complete no-VFS-in-kernel objective remains OPEN.

The machine extension is stored under /krnl/extension and protected with
the same canonical path checks as /sbin. ROOT cannot acquire a MACHINE
executable through user-triggered spawn. Dedicated system servers reside
under /sys/services; commands in /sys/bin retain the caller's privilege
clamp. All service identities are explicit, validated and registered over
the existing capability PORT; each service publishes a single generation
record at system.manage.service.NAME (PID:generation:mode:status).
Numeric slots and srv.* compatibility keys remain secondary views.

The SDK exec client and the ROOT nxexecd server share actual executable
resolution and redirection algorithms. Public RPC service requests are
restricted to /bin USER-class executables; system and machine paths need
direct caller-authorized spawn. This is an initial necessary safety gate,
not a completed caller-credential process factory. In particular cwd,
terminal and environment inheritance as well as redirected fd ownership
require the full delegation model specified in the existing architecture
plan. Never use ROOT service identity to raise a requesting user's rights.

Machine-only registry subtrees system.boot.*, system.manage.* and
system.service.* are checked at both capability acquisition and authoritative
mutation; ROOT's historical owner=0 shortcut does not authorize them.
Machine initial entry still originates in a validated on-image boot
configuration, which the storage provider imports into a registry object.

## Verification and still-open obligations

Actual AMD64 kernel and userland builds passed. Actual AMD64 rootfs has
verified ELF files in the expected locations and no old duplicate service
placements. Structural checks exercise names, unique entrypoints, SDK link
modules and actual built ELF headers; none substitutes for runtime stress.

The last incremental Power12 snapshot had 116 findings across newly
moved service sources (R2/R3/R4/R6/R7/R9). This remains a failing gate and
cannot be declared compliant on the basis of compiler success. Fix
unbounded event loops with real lifecycle policy, establish explicit
ownership of mutable state, preserve all concrete drawing/input logic,
split overly long functions without removing behavior, and check all
returned error statuses; do not simply suppress or exempt them.

Other mandatory acceptance: AMD64 and AArch64 QEMU shell, IPC saturation,
capability spoof/attenuation/revocation tests, ROOT/MACHINE isolation,
one-shot and restarted service semantics, theme consistency, actual libc
coverage and full-project Power12 qualification. The existing architecture
plan in docs/architecture/NEXS-NG-OBJECT-CAPABILITY-DEBUG-TIME-MIGRATION-2026-11-10.md
remains the normative reference for the future object-only process factory.

## 2026-11-10 on-device mdebug regression findings and corrections

The first AMD64 diagnostic boot with mdebug (before repairs) ran 30/30 kernel
tests, initialized PID1/PID2, and reached the shell. The actual userland
test binaries reported: captest 74/82, fdtest 8 failures, writetest 3
failures, capreg 4 failures, capipc 1 failure, and capkill 0 failures.
No fake pass was recorded. The ROOT nxroleprobe was reported successful by
the manager's real wait status but its detailed stdout was not on the
UART because it ran without a controlling terminal. The test runner exited
nonzero and the manager reported that failure.

Root causes confirmed against the actual implementations:
- The first protected-port policy captured ordinary user PORT names
  OS1nx_captest and OS1nx_capidle. Protected receivers are now explicitly
  OS1nx_svc_* (ROOT) or OS1nx_machine_* (MACHINE), while ordinary user
  names retain the actual capability PORT semantics. Service constants are
  changed together in NX-SDK and servers; no aliasing compatibility stub.
- The removed /etc/init.cfg file was still hardcoded in fdtest/writetest and
  one capreg stat case. Writable real ext4 fixture files are now installed
  independently under /home/shared; the tests continue to inspect actual
  read/write/seek/sparse results and failure codes.
- The guest IPC test assumed cap_grant installed a handle at child slot 0;
  kernel evidence showed slot 3. A bounded, kernel-stamped grant-ready message
  now transfers the actual target-slot index; the child validates its
  PROCESS WRITE rights and returns nonzero on failure.
- Registry enumeration's former 256-byte destination was too small for the
  named services. The actual 4096-byte syscall bound is now exercised.
  The VFS /reg synthetic provider supplies a generic fs_ops
  authorize_write callback: VFS enforces CAP_FS_WRITE, the object provider
  enforces CAP_REG_WRITE and reserved MACHINE namespace writes still fail
  in registry_set/del on every entrypoint. No path-specific ACL bypass exists.

These corrections are awaiting fresh on-guest verification before they count
as passes; the Power12 strict/delta gates remain independent.


## Incremental review 2026-11-12

The original inventory above is historical. The current build additionally has
nxttyd under user/sys/services, gfx_buffer.c and port_client.c in SDK service
extensions, and shared keyboard translation in extensions/input. Current exact
ownership and outstanding obligations are in
../review/USERLAND-SYSTEM-CATALOG-2026-11-12.md and
../review/INCREMENTAL-REVIEW-2026-11-12.md. Compilation does not qualify the
current changes: an AMD64 mdebug stress run exposed a context-restore panic
under investigation, and Coreutils has real failing cases.
