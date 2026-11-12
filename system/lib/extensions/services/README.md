# NX-SDK service client extensions

These are shared client protocol components, NOT server executables.
Server entrypoints and lifecycle policy belong to supervised userland.

- port_client.c: kernel-authenticated transfer counts and received capability
  cleanup over the existing PORT object queue. Payload slot values alone are
  never proof that a capability was transferred.
- buffer_client.c: capability-backed resource buffer/map/unmap requests.
- execsvc_client.c: execution request framing, delegated pipe transport,
  response decoding and existing client fallback. The live server is
  user/sys/services/nxexecd.c; UI command is user/sys/bin/nxexec.c.
- notify_client.c: notification IPC and centralized error severity policy;
  the actual popup/deduplication service is nxntfy_srv.
- gfx_buffer.c: active/retired mapped buffer ownership for renderers.
- surface_client.c: SDK display/input/surface resource client adapters;
  rendering and event routers are nxcompositor, nxkeyboardd, nxmoused.
- time_client.c: versioned timer telemetry client; server is nxtimerdd.

The SDK graphics library also owns reusable rendering algorithms and
per-process active theme selection state. The latter is NOT an authoritative
cross-process compositor theme state; migration needs a real versioned
registry/IPC consistency contract and acceptance tests, not a file move.

Canonical role inventory is in project-root
docs/architecture/NXSERVICE-OWNERSHIP-AUDIT-2026-11-10.md.
Do not duplicate service implementations or silently change client ABIs.
# Incremental verification, 2026-11-12

The execution client now constructs the bounded `os1_spawn_request_v1` for
`OBJ_CTL_SPAWN_OWNED` on a PROCESS capability. The kernel assigns logical
ownership while the child is still `PROC_CREATED`, before it can execute.
The broker does not repair ownership after an already runnable spawn.
This is not the completed delegation of cwd/environment/TTY context, nor a
solution for uncertain RPC outcomes if a broker dies before replying.
