# NEXS-NG — Syscall legacy / Object / Registry / Capability crosswalk

**Data audit:** 2026-11-03 · **Status:** source-grounded inventory, migration design; no syscall renumbering authorized.

## 1. Core principle

The old syscall interface is a compatibility boundary, not the architecture to extend. The new OS1 design keeps a minimal low-level kernel ABI and routes resource operations through capability handles. Registry/namespace resolves names; a capability authorizes use. A PID, path or registry name alone must not become authority.

Programme R in `PLAN-2026-07-23-HARDENING-SDK-RUST.md` remains the authoritative migration sequence. This crosswalk adds the graphics/device/NG implications; it does not supersede R1–R6.

## 2. Current object substrate (source verified)

- UAPI: `include/abi/object.h`; kernel: `kernel/include/kernel/object.h`,
  implementation: `kernel/core/object.c`.
- Types currently declared: FILE, PROCESS, REGKEY, WINDOW, CONSOLE, PIPE, PORT
  (1–7). No BUFFER, SURFACE, DISPLAY, GPU or DEVICE object exists.
- Handle table has 32 slots per process; handle is an integer slot index and
  object is refcounted. Rights include READ/WRITE/WAIT/DUPLICATE/TRANSFER/
  DESTROY/MUTATE. Duplicate/grant can attenuate.
- `pin_handle` pins object references for I/O; global `object_lock` protects
  table/refcount transitions; blocking I/O should run outside that lock.
- Important unresolved audit: slot index has no generation encoded in user
  handle. Verify close/reuse races, stale integer reuse and cross-process
  semantics before designing long-lived surface/device capabilities.
- `OBJ_TYPE_WINDOW` is not remote: its payload contains window ID/owner PID and
  object read/control calls `compositor_*()` in kernel. It is a legacy proxy
  to the kernel compositor, not yet an RPC endpoint to compositord.
- `OBJ_TYPE_REGKEY` is defined but Programme R0 records zero userland users.
  The registry has three historical access routes: SYS_REGISTRY, REGKEY object
  and /reg regfs; R2 must collapse them without losing virtual process env keys.
- `OBJ_TYPE_PORT` is a real capability mailbox. It is the correct direction
  for service discovery and RPC, but PID-addressed IPC remains available.

## 3. Legacy syscall crosswalk

| Legacy syscall / API | Current implementation | Existing object equivalent | Target / migration |
|---|---|---|---|
| OPEN/CLOSE/READ/WRITE/LSEEK (56/57/63/64/62) | POSIX-shaped dispatcher | FILE handle + object I/O | already same mechanism; retain libc ABI |
| FILE_READ/WRITE/LIST_DIR (251/252/254) | retired | FILE handle + READ/WRITE/STAT | retired, never reuse numbers |
| MKDIR/UNLINK (259/260) | retired | parent DIR handle + OBJ_CTL_MKDIR/UNLINK | R1b; retain POSIX wrapper composition |
| STAT (263) | path metadata syscall | FILE + OBJ_CTL_STAT | compatibility one-roundtrip helper; verify it does not fork semantics |
| REGISTRY (250) | direct `sys_registry` path | REGKEY object / `/reg` regfs | R2 collapse; preserve `sys.proc.<pid>.env.*` virtual routing |
| SEND/RECV/TRY_RECV (230/231/233) | PID-addressed queue IPC | PORT object capability | R4; port name lookup + transferred send capability |
| PORT_SEND_CAPS (262) | port message + handle transfer | PORT + TRANSFER rights | keep as capability transfer primitive; test rollback/queue-full |
| KILL (221) | PID + ancestry/level policy | PROCESS + OBJ_CTL_KILL | R5; legacy wait/reaped-status semantics retained |
| WAIT (247) | PID + retained exit status | PROCESS + OBJ_CTL_WAIT/object_wait | R5; cannot acquire dead PID capability after reap, retained status needs dedicated path |
| SPAWN/SPAWN_CAPS (220/234) | path/args + privilege caps | process object / process service port | preserve bootstrap; move policy to processd, no path-prefix privilege |
| GETPROCS (222) | process enumeration | `/proc` namespace / PROCESS objects | userland proc service; capability-filtered view |
| SET_FOCUS (232) | PID focus operation | WINDOW object + OBJ_CTL_FOCUS | compositor policy RPC; don't preserve PID authority |
| CREATE_WINDOW (210) | direct compositor allocation | WINDOW + future SURFACE/BUFFER | compositord creates window, kernel issues surface/buffer caps |
| WINDOW_DRAW (211) | kernel pixel raster | no pixel object method | libos1-gfx writes mapped surface and commits |
| WINDOW_BLIT (213) | user buffer copied into kernel window | shared BUFFER/SURFACE | mapped buffer + commit/damage |
| WINDOW_SET_FLAGS (214) | kernel mutates policy flags | WINDOW object | compositord policy RPC |
| DESTROY_WINDOW (215) | kernel destroys compositor window | WINDOW + SURFACE revocation | service destroys policy object; kernel revokes resources |
| WINDOW_WRITE (217) | kernel text rendering | CONSOLE/WINDOW stream | terminal service writes to userland surface |
| WINDOW_OF_PID (218) | PID lookup | service-owned window relation | remove from new API; compatibility broker only |
| WINDOW_GRID (219) | font-derived terminal grid | terminal/font service query | userland terminal service |
| WINDOW_ENUM (202) | copies compositor list | WINDOW objects / service namespace | compositord enumeration IPC |
| DRAW (200) | direct root framebuffer drawing | no application-facing object | remove from ordinary app ABI; panic path/display service only |
| DISPLAY_INFO (270) | kernel compositor state | DISPLAY object/service | displayd query |
| SET_DISPLAY_MODE (271) | kernel GPU mode + compositor resize | DISPLAY control capability | displayd policy + kernel-validated hardware caps |
| WINDOW_RESIZE (272) | kernel reallocates pixel buffer | new SURFACE generation | compositord window policy + kernel surface replace |
| DISPLAY_POLL (273) | kernel polls GPU events | display service event port | displayd receives hotplug/mode event |
| SET_STYLE (274) / SET_ZOOM (275) | kernel compositor policy | none (policy, not resource primitive) | compositord config/registry IPC |
| SET_FONT (253) | kernel-global font install | no kernel font object desired | fontd/userland font resources |
| NANOSLEEP/CLOCK_GETTIME (257/258) | kernel scheduling/time primitives | wait/timer objects in future ABI | low-level kernel primitives remain; userland clock service adds wall time |
| PIPE (261) | kernel pipe object | OBJ_TYPE_PIPE | already object-backed; preserve POSIX fd facade |
| SBRK (216) | process heap compatibility | VM object/map primitives | v1 compatibility only; new API uses VM objects |
| SYSSTATS (225) | privileged aggregate snapshot | diagnostic namespace/capability | move broad introspection to services; keep minimal counters kernel-side |

This table distinguishes operations on **resources** from pure kernel mechanisms.
Not every syscall should become `object_ctl`: context switch, trap return,
VM map/unmap, wait, interrupt delivery and capability transfer remain minimal
mechanisms. Object methods are for operations on a resource already named by a
capability.

## 4. Registry and service-discovery semantics

Current state:
- `/reg` is a mounted namespace tree; `/proc` maps process objects.
- `SYS_REGISTRY` still directly calls `sys_registry`.
- `OBJ_TYPE_REGKEY` has no users per Programme R0.
- `OBJ_TYPE_PORT` resolves named service ports and separates send/receive
  rights; port ownership is capability-based.
- Registry key ownership and virtual process environment routing have
  historical semantics; do not remove them during R2.

Target:
1. registry stores discoverable metadata and configuration;
2. service name resolves to an endpoint descriptor;
3. client receives a send capability (directly or by an authenticated broker);
4. endpoint rights and message schema authorize actual operations;
5. service death revokes stale endpoint generation; service restart republishes;
6. registry read access does not grant device MMIO, DMA, IRQ or surface access.

## 5. Capability policy and hardening concerns

- Ambient `CAP_*` masks gate acquisition; per-handle rights govern later use.
  This two-stage model must remain explicit.
- `PLVL_USER = CAP_ALL` is recorded as CAP-POLICY-01 in hardening plan:
  mechanism is enforced, but least privilege policy is not achieved. Do not
  assume a new CAP bit will help while the default grants it to everyone.
- `SYS_CAP_GRANT(target_pid,...)` remains PID-addressed. Programme R/port
  transfer provides a path away from ambient PID routing; audit grant's target
  lifetime and process reference pinning.
- Rights attenuation is not sufficient if an object operation has hidden
  owner/level checks with inconsistent semantics. For each verb, document
  required right + namespace acquisition ACL + target ownership rule.
- Process wait is special: exit status survives object death; do not force
  all waits through a live-process handle.
- Window focus is intentionally more permissive than window-manager control
  in current code. Keep click-to-focus policy in compositord, not as accidental
  kernel-wide WRITE authority.

## 6. Migration order (preserve R sequencing)

- R1/R1b files: already retired path verbs; keep them retired.
- R2 registry: collapse SYS_REGISTRY/REGKEY/regfs, preserve virtual env.
- R3 windows: migrate 17 ad-hoc window/display verbs; prerequisite is C1 UART
  diagnostic independence and stable service ports.
- R4 IPC: migrate PID-addressed send/recv to port capabilities.
- R5 process: migrate kill/wait, preserving wait-after-reap semantics.
- R6 VFS: namespace mounts and role partitions, ROOT mount bootstrap exception.

Surface/Buffer/Commit must be designed as object types after R0 inventory and
the handle-generation audit. They are not a reason to reorder R3 or introduce
a parallel syscall family. The v1 dispatcher remains the adapter until each
userland caller has moved.

## 7. Required source audit before implementation

Read complete functions and call sites, not only syscall numbers:
- `kernel/core/syscall_dispatch.c`: every switch case and capability gate;
- `kernel/core/object.c`: acquisition, pin/unpin, close, grant, transfer,
  pipe/port queue, read/write/wait/ctl, teardown;
- `kernel/lib/registry.c`: SYS_REGISTRY, REGKEY and regfs path semantics;
- `kernel/sched/process.c`: spawn, wait/reap, kill/subtree, port/IPC;
- `user/sys/lib/lib.c`, `lib-2.c`, syscall stubs and all app call sites;
- `include/abi/object.h`, `caps.h`, syscall numbers and per-service policy.

Required test matrix: rights attenuation, duplicate/grant, stale handle reuse,
close racing with I/O, process death while receiving, port queue full rollback,
service restart, wrong object type, owner mismatch, cross-process transfer,
legacy binary compatibility and reaped process wait.
