# Library ownership and split inventory

Date: 2026-11-08
Status: source-grounded first classification; not yet the final SDK source manifest.

## Core runtime

| Current source | Responsibility | Target owner | State |
|---|---|---|---|
| user/sys/lib/lib.c | C/POSIX runtime, OS1 native wrappers, registry/VFS/process APIs, graphics/window calls, error conversion | NX-SDK core, then split into libc and OS1 native modules | Still monolithic; service clients extracted below |
| user/sys/lib/common/string.c | Userland string/memory subset | NX-SDK libc | Userland-only; differs from kernel/lib/string.c |
| user/sys/lib/common/vsnprintf.c | Userland formatting and scanning | NX-SDK libc | Userland-only; differs from kernel/lib/vsnprintf.c |
| user/sys/lib/math.c | Userland IEEE-754 math | NX-SDK libc | Not equivalent to kernel/lib/math.c (fixed-point) |
| user/sys/lib/malloc.c | Userland allocator | NX-SDK runtime | Must remain distinct from kernel allocator policy |
| user/sys/lib/font_lib.c | Font parsing/loading/raster helpers | NX-SDK graphics extension | Still source-included into lib.c; not standard libc |
| user/arch/<isa>/syscall.S | User syscall veneers, _start and setjmp/longjmp support | NX-SDK runtime/architecture support | Must remain in user build; not kernel assembly |

The old claim that userland directly includes kernel/lib sources is false in the current tree. The userland common files differ from their kernel counterparts. Do not unify by filename or copy/symlink; first define shared behavior and run the same test vectors.

## Service client extensions

| Source | Server owner | Client owner | Evidence |
|---|---|---|---|
| extensions/services/notify_client.c | NX-VIOLA: nxntfy_srv, started and supervised by nxinit | NX-SDK service extension | Moved from lib.c; amd64 and aarch64 nxadb smoke tests sent a notification and listed it from the service registry |
| extensions/services/execsvc_client.c | NX-VIOLA: nxexec --service, started and supervised by nxinit | NX-SDK service extension | Moved from the library root; public protocol EXECSVC_VERSION=2. captest exercises the wire protocol directly, but no in-tree caller of execsvc_spawn was found; direct client-function test remains required |

The server implementation must never be copied into the SDK. Service clients negotiate the service contract and return deterministic unavailable/stale-endpoint errors.

The notification protocol currently has no explicit wire-version field. Add a versioned request contract and compatibility tests before advertising a stable service API. The execution protocol already has EXECSVC_VERSION=2 and the server checks exact equality.

## Native API versus service extension

- OS1 registry, VFS, process, timer and window wrappers in lib.c currently call kernel syscalls/object APIs directly. They are native API compatibility veneers, not clients of boot-started servers today.
- Graphics/window calls are not yet a userland compositor client: the current compatibility path is still kernel-backed. Do not move them into a service category until the serverization implementation and call graph change.
- OS1_report_error was moved with the notification client because its visible reporting policy depends on nxntfy_srv. errno conversion remains in the runtime and calls the extension seam.

## Next extraction order

1. Split libc standard/POSIX implementation from OS1 native APIs, preserving one definition per public symbol.
2. Move font_lib.c and graphics/window compatibility functions into explicit SDK graphics modules.
3. Audit errno/error-reporting dependencies so the base libc does not accidentally require optional service policy.
4. Generate symbol ownership and call-site maps from both architecture object files; compare defined/undefined symbols and link maps.
5. Add API conformance tests before any further source move.
