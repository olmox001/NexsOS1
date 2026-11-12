# Shared executable launch contract

The public API is system/include/api/execsvc.h. Its implementation is
system/lib/extensions/services/execsvc_client.c, part of the shared SDK
linked by every first-party executable via USER_LIB_O. Application code
should not import the private user/sys/bin/nxexec.h header, which retains
shell and terminal-specific foreground monitoring.

## Launch modes and their required guarantees

- EXECSVC_MODE_FOREGROUND: run as a child of the actual caller, with current
  working directory, controlling terminal, inherited file handles, ordinary
  capability clamp and job-control lineage intact. Use for interactive
  shells, pipes and redirection.
- EXECSVC_MODE_DETACHED: same direct caller model but no inherited controlling
  terminal. Do not use for a windowless program needing interactive output.
- EXECSVC_MODE_HOSTED: launch precisely one detached /sys/bin/nxexec.
  That frontend creates a /sys/services/nxttyd session, rendering a terminal
  when needed and detecting a persistent independent GUI window. Use from
  launchers, docks, file managers and graphical application menus.

Each API returns a real positive PID on successful spawn, or a negative
error value. The resolver accepts absolute, process-cwd-relative and
HOME-relative paths. Bare names search /bin before /sys/bin; unsupported
user-home expansion and truncated paths are refused. A search call may
rewrite argv[0] to the canonical executed path.

Example - direct foreground child:

    #include <execsvc.h>
    char *args[] = {"hello"};
    char resolved[EXECSVC_PATH_MAX];
    int pid = execsvc_launch(1, args, EXECSVC_MODE_FOREGROUND,
                             NULL, 0, resolved, sizeof(resolved));

Example - GUI or terminal application hosted by a graphical launcher:

    #include <execsvc.h>
    char *args[] = {"/bin/kilo", "/home/readme.txt"};
    int pid = execsvc_launch_hosted(2, args);

Local execution is bounded by EXECSVC_LOCAL_ARG_MAX=16, the kernel's spawn
argument cap. HOSTED reserves one entry for the supervisor. HOSTED rejects
redirections instead of silently discarding them.

## Existing execution broker

execsvc_spawn() remains the existing nxexecd capability-port RPC. Its
daemon validates the kernel-stamped requester and restricts execution to
/bin. The shared SDK resolver is used by both the broker and local execution.
The broker cannot presently delegate the client's full controlling terminal
and process environment; interactive jobs must therefore remain local.
The logical process owner alone is not a complete replacement for the ctty.

## Scoped mouse management capability

The pointer service line in user/sys/boot/services.conf explicitly requests
window-manage. The authenticated init sends that mask to nxmanageDD before
REGISTER; the machine-owned supervisor grants CAP_WINDOW_MANAGE to that
ROOT service at spawn and on supervised restart. Other ROOT processes have
the ordinary capability defaults and cannot resize/move foreign windows.
The userland compositor service owns presentation pacing. The kernel still
contains the legacy window adapter, renderer and scene policy; their removal
remains open in the live graphics migration catalog.

## Verification points

tools/test_nxmanage_boot_manifest.py checks authority declarations and
service slots. user/bin/libctest.c checks real execsvc resolver, mode errors
and a guest child spawn/wait. user/sys/bin/nxroleprobe.c verifies that
ordinary ROOT does not receive CAP_WINDOW_MANAGE. Final acceptance requires
boot and real GUI screenshots on amd64 and aarch64 in addition to compilation.


## Capability-transfer hardening (12 November 2026)

The broker accepts an os1_port_packet with at least the two kernel-translated
channel handles. Protocol version 3 interprets EXECSVC_FD_GRANTED references as
ordinals in the additional handles transferred with that packet; an arbitrary
integer naming the service's CSpace is not accepted. Unexpected attachments are
closed by the common SDK PORT client. The protocol still uses blocking pipes;
finite transport waits remain an independent open hardening requirement.
