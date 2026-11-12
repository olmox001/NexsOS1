# NEXS-NG — Graphics userland migration and kernel surface manager

**Decision:** 2026-11-03
**Historical status (2026-11-08; see current scope below):** migration incomplete. `nxcompositor` now composes frames in ring 3; separate `nxmoused` and `nxkeyboardd` services interpret input. The unused kernel frame renderer and pointer/drag handlers have been removed. Kernel window policy/storage, terminal rasterization, graphics libraries and GPU/display implementations still remain; no capability-backed shared surface manager exists yet. Full rendering/input parity and a performance improvement are not certified. See [the current handoff](../report/GFX-USERLAND-HANDOFF-2026-11-08.md) and [function catalog](GFX-KERNEL-FUNCTION-CATALOG.md) for implementation state and evidence.
**Supersedes:** the DIR-02 and DIR-07 wording that places the compositor inside the HAL.
**Parent specifications:** NEXS-NG-CORE-DESIGN.md, NEXS-NG-HARDWARE-AND-ISOLATION.md, ABI-COMPATIBILITY-MATRIX.md, graphics-port/README.md.

**Current scope (2026-11-12, explicit maintainer clarification):** migrate the
complete compositor and all ordinary graphics policy/rendering into supervised
userland services. Kernel graphics responsibility is restricted to surface/buffer
mechanisms, object/capability enforcement and the minimal emergency console.
The 2026-11-08 decision retaining window policy in the kernel is superseded.
Process scheduling/lifetime and capability enforcement remain generic kernel
mechanisms; window/desktop association, focus, stacking, close/minimize/resize
policy belong to userland. `kernel/resources` is an existing provider boundary,
not a place to relocate compositor policy under another filename.

## 1. Decision

The rendering implementation, rasterizers, terminal drawing, input device aggregation and graphics API libraries execute in **userland services/libraries**. The kernel retains a small **surface/buffer/control manager**: capability-protected objects, backing-memory ownership, mappings, access rights, lifetime, synchronization, validation and controlled presentation hand-off. Kernel mechanisms enforce delegated rights and resource lifetime. Userland services choose window ownership/desktop associations, focus, visibility, stacking and close/minimize/resize behavior, and implement rendering, decoration, event interpretation, presentation pacing and graphics libraries.

The kernel MUST NOT render pixels, composite windows, rasterize fonts, parse GL/D3D commands, draw decorations or run an application-facing graphics toolkit. It may retain validated surface metadata and generic process/object lifetime state; ordinary window and desktop policy execute in userland.

This is a mechanism/policy split, not a relocation of the current 3,000-line compositor as one privileged process.

## 2. Responsibility boundary

| Component | Owner | Responsibilities |
|---|---|---|
| Buffer object manager | Kernel | Allocate/import page-backed storage; bounds, format/stride metadata, pinning, mapping, cache attributes, lifetime/refcounts, DMA leases, revocation |
| Surface object manager | Kernel | Capability object referencing a buffer/range; dimensions/format/usage; owner and rights; attach/detach; generation; acquire/release/commit state |
| Presentation control | Kernel | Validate presenter capability, surface generation, damage rectangles and buffer state; serialize commit sequence; deliver bounded completion/error notifications; protect scanout ownership |
| Window/desktop policy manager | Userland service | Scene/window identity, association with caller capabilities, z-order, focus, visibility, close/minimize/resize state and lifecycle; kernel enforces the delegated generic rights |
| Render scene implementation | `nxcompositor` userland | Own window policy state; clipping, occlusion, damage aggregation, decoration, frame pacing and pixel composition |
| Software composition | Userland library/service | Pixel blending, scaling, transforms, effects, shadows, decorations, cursor composition and CPU fallback |
| Font and text | `nxfontdd` / SDK font extensions | Font parsing, glyph rasterization/cache, text shaping, terminal rendering |
| 2D / 3D APIs | Userland libraries | Drawing primitives, 2D toolkit, software 3D, Mesa/OpenGL, SDL and D3D compatibility |
| Display/GPU service | Userland driver service | Device discovery, mode selection, command submission and scanout programming using granted MMIO/IRQ/DMA capabilities |
| Input | Kernel + `nxmoused`/`nxkeyboardd` userland services | Kernel enforces device/resource and service-role access; userland aggregates per-device input, maps gestures and routes events to windows |
| Emergency console | Minimal kernel path | Serial output and a tiny panic framebuffer writer only; no dependency on nxcompositor, fonts or allocator-heavy graphics |

Surface **control** means kernel-enforced authority and resource state: who may create/map/write/share/commit/present/destroy a surface and when its backing pages may be reused. Window/desktop policy is a userland responsibility under the current scope; kernel enforcement must not encode desktop decisions.

## 3. Kernel object model

Build on the existing capability/object manager; do not introduce ambient graphics syscalls or raw kernel pointers.

### Buffer

A buffer is page-backed storage with immutable allocation size and explicit attributes:
- byte size, page count, alignment, cache/coherency attributes and usage flags;
- backing frame references, pin count, mapping count and DMA lease count;
- owner domain, generation, lifecycle state and reference count;
- CPU-map and device-map rights independently attenuable.

A buffer is not a C pointer. Userland receives an opaque capability and a process-local mapping address created by the VM manager. The kernel never trusts a user-provided pixel pointer after a syscall returns.

### Surface

A surface is a typed view over a buffer range:
- width, height, stride, pixel format, plane/offset metadata;
- usage (render target, texture/sample, scanout candidate);
- buffer capability reference and generation;
- owner and rights: query, map-read, map-write, attach, commit, present, grant, destroy.

All geometry arithmetic uses checked multiplication/addition. The kernel rejects overflow, undersized stride, out-of-range buffer spans, unsupported formats and invalid alignment before mapping or committing. Initial format may be ARGB8888; extensible format negotiation belongs in the ABI, not hard-coded renderer code.

### Commit / synchronization

A commit is a bounded descriptor, not a draw command:
- surface handle + generation;
- buffer sequence or acquire/release token;
- bounded damage rectangle list (or full-damage flag);
- optional completion/fence capability when a real provider supports it.

The kernel validates and queues the commit, pins the backing buffer for the in-flight operation and reports completion/error. A buffer cannot be resized, unmapped or freed while referenced by an active commit, GPU DMA or display scanout. Revocation first prevents new work, then waits/cancels outstanding leases with bounded timeout semantics, then releases pages.

Do not implement a fake GPU fence. A fence object is introduced only when an actual asynchronous provider completion path exists. CPU software presentation can use sequence-numbered completion without claiming hardware queue semantics.

### Window object distinction

A window policy object and a render scene are distinct. Under the 2026-11-12 scope, userland owns geometry, z-order, focus and window lifecycle; the kernel enforces generic object/capability and buffer/surface lifecycle. The current kernel snapshots are transitional implementation debt. A window should reference kernel surface capabilities once those exist. Today legacy `OBJ_TYPE_WINDOW` handles still resolve to kernel window objects and copied pixel storage; this is a compatibility path, not the completed surface design.

## 4. Userland service topology

- `nxmanageDD` and `nxinit`: existing boot supervision and userland service startup; keep graphics daemons in the existing manifest.
- Display service role: future owner of discovery, modes and display lifecycle policy; no new executable name is assigned by this plan.
- Device-specific GPU service role: future consumer of granted MMIO, IRQ and DMA capabilities; existing kernel GPU providers remain implementation debt until that cutover is verified.
- `nxcompositor`: owns the render scene, frame scheduling, composition and presentation requests; the scene and window policy reside in userland.
- `nxmoused` and `nxkeyboardd`: separate supervised pointer and keyboard services. Touch extends the pointer path; complete touch protocol support and capability-authenticated event delivery remain work to do.
- `nxfontdd`: asynchronous shared font resource/raster service with an SDK extension; `nxfont` remains the user command. Reuse the existing userland packed-font and raster algorithms; global and per-app selection are service/client policy.
- `libos1-gfx`: stable userland surface/window/event API.
- SDL, Mesa/OpenGL, software 2D/3D and D3D personalities consume `libos1-gfx` / `os1_video_platform`; upstream submodules remain unmodified.

The compositor is independently scheduled and owns its frame pacing/vsync subscription. Applications submit/commit surfaces; they do not invoke a kernel compositor render function. Static windows need not redraw every frame.

## 5. ABI and compatibility

1. New code uses opaque object capabilities and versioned descriptors. No raw kernel addresses or PID-derived authority.
2. Prefer existing object/VM/port primitives. Add only the minimum surface/buffer operations that cannot be expressed through them; allocate ABI numbers only after reviewing the shared ABI and tests.
3. Service discovery and RPC use named ports/capability transfer, not ambient PID sends.
4. The v1 compatibility dispatcher translates legacy window create/draw/blit/resize/focus/style calls into service requests. It may copy a legacy buffer into a temporary buffer object for correctness first; zero-copy is an optimization after ownership and lifetime tests.
5. No synchronous service RPC while holding scheduler, object-table, VM, IRQ or device locks. Pin/copy descriptors, drop kernel locks, block on a request sequence, validate the authenticated response, then return the legacy result.
6. Service death returns deterministic errors, revokes stale proxy handles and restarts the service without corrupting kernel state. A compositor crash must not crash or deadlock the kernel.

## 6. Migration phases and gates

### GFX-NG-0 — freeze and inventory (documentation / no behavior change)
- Keep the current legacy kernel as the bootable reference; do not delete or mass-move its files.
- Map every graphics syscall, object type, caller, IRQ path, GPU provider and build target to the new boundary.
- Record current pixel formats, resize semantics, input delivery, window ownership and failure behavior.
- Before relying on the legacy compatibility broker, close the legacy terminal/concurrency preflight: `window_text_write → compositor_window_write → term_write → font rasterization`, compositor/terminal lock ownership, IRQ masking, actual backing-buffer lifetime, and the intermittent AArch64 SSP failure. Evidence and acceptance tests: `docs/report/GFX-LEGACY-TERMINAL-CONCURRENCY-2026-11-03.md` and `docs/PENDING-WORK.md` §14. This is a correctness gate, not permission to move/delete the legacy compositor or change the approved ring-3 target.
- Acceptance: ABI inventory and dependency graph reviewed; current AMD64/AArch64 baseline remains buildable.

### GFX-NG-1 — kernel buffer/surface manager
- Implement kernel object types and lifecycle on the NG object manager.
- Implement safe allocation/import, mapping, rights attenuation, pin/unpin, generation checks, bounded commit descriptors and revocation.
- Add host tests for overflow, invalid handles, rights, map/unmap, destroy races, stale generation, concurrent commit and process death.
- Acceptance: no raster/composition code linked into the NG kernel; cross-process surface sharing only by transferred capabilities; ASan/UBSan host tests where possible.

### GFX-NG-2 — minimal userland display service
- Start a supervised display/GPU service from initramfs with only explicit device resources.
- Implement one software/linear scanout path first; keep the panic/serial console independent.
- Test mode changes, display loss, driver restart, buffer pinning and DMA/cache rules on both architectures.
- Acceptance: kernel can boot and recover its console without the display service; display service failure does not panic the kernel.

### GFX-NG-3 — userland compositor and compatibility broker
- Port render scene, dirty regions, frame scheduler, software blending/scaling, chrome and cursor into `nxcompositor`; retain only generic kernel surface/capability mechanisms as specified above.
- Replace direct scheduler/process/IPC calls with capability ports and process/window events.
- Add `window_create`, `window_present/commit`, `window_resize`, `window_focus` and event APIs in libos1.
- Implement the v1 compatibility broker so unchanged legacy applications continue to work.
- Acceptance: compositor runs ring 3; app death/restart and compositor restart preserve kernel integrity and reclaim surfaces safely.

### GFX-NG-4 — graphics libraries
- Move `gl.c`, `draw2d.c`, `draw3d.c`, `font.c`, `chrome.c`, `compositor_style.c`, `region.c` and terminal rasterization to userland equivalents.
- Integrate SDL2 and Mesa/OpenGL through `os1_video_platform`; retain software fallback.
- Keep third-party source trees immutable; NexsOS glue stays in portability/build overlays.
- Acceptance: libraries have no kernel-header dependencies and run against the same surface capability API.

### GFX-NG-5 — hardware acceleration and hardening
- Move device-specific GPU/display drivers into isolated services where the hardware contract permits; kernel retains MMIO/IRQ/DMA capability enforcement and IOMMU/bounce-buffer policy.
- Add real queue/fence objects only for providers that implement actual asynchronous submission/completion.
- Stress SMP, multiple clients, resize, display hotplug, process kill, service restart and memory pressure.
- Acceptance: per-architecture boot/build tests, fault isolation, leak-free revocation and measured latency budgets.

## 7. Legacy source migration map

| Current source | NG destination |
|---|---|
| `kernel/graphics/compositor.c` | Userland window/scene policy in `user/sys/services/nxcompositor.c` and pointer interpretation in `user/sys/services/nxmoused.c`; retain only surface/capability mechanisms in kernel resource providers |
| `kernel/graphics/chrome.c`, `compositor_style.c`, `region.c` | userland compositor libraries |
| `kernel/graphics/gl.c`, `draw2d.c`, `draw3d.c` | `system/lib/gfx` / SDK graphics extensions |
| `kernel/graphics/font.c` | `user/sys/services/nxfontdd.c` and SDK font extension (pending) |
| `kernel/graphics/term.c` | terminal/UI library or terminal service |
| `kernel/graphics/surface.c`, `kernel/include/kernel/gfx_surface.h` | split: metadata/validation/object/mapping control stays kernel; all pixel operations leave kernel |
| `kernel/drivers/gpu/*`, `kernel/drivers/virtio/virtio_gpu.c` | device service/provider, with kernel resource-capability and DMA enforcement |
| `kernel/graphics/graphics.c` | split: display service; minimal serial/panic framebuffer writer remains kernel |
| `kernel/core/syscall_dispatch.c` graphics cases | v1 compatibility broker to nxcompositor/display service; only object/VM/IPC primitives remain kernel |
| `system/include/api/graphics.h`, `system/include/api/os1_video_platform.h` | stable libos1-gfx API and compatibility wrappers |

This is a destination map, not a statement of completed work. The 2026-11-08 implementation is in the active tree; preserve original source/history and validate behavior against the function catalog before retiring further compatibility paths.

## 8. Non-goals / safety invariants

- Do not move pixel rendering into a privileged kernel thread or kthread. The target is ring-3 services.
- Do not replace the current long IRQ-off render lock with an unlocked renderer that still mutates shared kernel buffers.
- Do not let userland write arbitrary physical scanout memory or program device registers without explicit capabilities.
- Do not expose kernel pointers, physical addresses or unvalidated DMA descriptors to applications.
- Do not couple the scheduler to window focus or compositor policy.
- Do not remove the emergency panic display path until an independent recovery console exists.
- Do not equate a ring-3 frame loop or removal of dead code with complete migration: kernel raster libraries, terminal drawing and device implementations must also leave the normal rendering path.

## 9. Immediate next increment

Resume from the [2026-11-08 handoff](../report/GFX-USERLAND-HANDOFF-2026-11-08.md). Complete source/behavior parity using the original function catalog, implement real surface/buffer capabilities against the existing object manager, and move the remaining terminal/library/GPU paths. Do not start a second renderer or discard the current ports. Retire compatibility syscalls only after their callers have working replacements and regression coverage.


## 10. GFX-NG-0 inventory — current legacy tree (2026-11-03)

This is the historical 2026-11-03 baseline, not the current implementation.
In particular, `SYS_COMPOSITOR_RENDER` now returns without rendering, and
snapshot/present/raw-input service operations exist. Current ownership and
remaining gaps are recorded in the linked handoff and function catalog.

### Existing syscall surface

| Syscall | Number | Current behavior | NG destination |
|---|---:|---|---|
| `SYS_DRAW` | 200 | privileged direct root-framebuffer rectangle | remove from normal app ABI; emergency/display service path only |
| `SYS_WINDOW_ENUM` | 202 | copies compositor window list | nxcompositor query IPC |
| `SYS_CREATE_WINDOW` | 210 | allocates kernel window + pixel buffer | nxcompositor window create + surface/buffer capability |
| `SYS_WINDOW_DRAW` | 211 | kernel raster rectangle into window | libos1-gfx CPU drawing + surface mapping |
| `SYS_COMPOSITOR_RENDER` | 212 | PID1 calls USB poll, input drain and kernel compositor render | service event/frame loop; legacy broker only |
| `SYS_WINDOW_BLIT` | 213 | copies user pixels into kernel-owned window buffer | map shared buffer and commit damage |
| `SYS_WINDOW_SET_FLAGS` | 214 | kernel mutates window policy flags | nxcompositor policy IPC |
| `SYS_DESTROY_WINDOW` | 215 | destroys kernel window/buffer | revoke/release surface; nxcompositor destroys window |
| `SYS_WINDOW_WRITE` | 217 | kernel terminal text rendering | terminal service/library writes into mapped surface |
| `SYS_WINDOW_OF_PID` | 218 | PID-to-window lookup | remove from new API; legacy adapter query |
| `SYS_WINDOW_GRID` | 219 | returns font-derived terminal grid | terminal/font service query |
| `SYS_DISPLAY_INFO` | 270 | returns compositor desktop size | display service/nxcompositor query |
| `SYS_SET_DISPLAY_MODE` | 271 | kernel calls GPU driver then resizes compositor | display service request with kernel display-control capability |
| `SYS_WINDOW_RESIZE` | 272 | kernel reallocates logical pixel buffer and notifies owner | nxcompositor window policy + new surface generation |
| `SYS_DISPLAY_POLL` | 273 | PID1 polls host GPU mode events | display service device event loop |
| `SYS_SET_STYLE` | 274 | kernel updates compositor style/theme/background and registry | nxcompositor + registry storage |
| `SYS_SET_ZOOM` | 275 | kernel changes desktop scaling | nxcompositor/display service policy |
| `SYS_SET_FONT` | 253 | installs kernel-global font data | nxfontdd/SDK font extension; emergency-only kernel font state |

Syscall 201 is retired. Existing ABI numbers and wrappers must remain intact
for legacy userland until the v1 compatibility broker is implemented.

### Existing capability/object substrate

- Shared UAPI is `include/abi/object.h`; kernel implementation is
  `kernel/include/kernel/object.h` + `kernel/core/object.c`.
- Current types are FILE, PROCESS, REGKEY, WINDOW, CONSOLE, PIPE and PORT
  (values 1–7; `OBJ_TYPE_COUNT=8`). No BUFFER or SURFACE object exists yet.
- `OBJ_TYPE_WINDOW` currently stores a legacy integer window ID and owner PID;
  the object manager calls back into compositor code for read/control/destroy.
  This is precisely the dependency the NG adapter must invert.
- Existing handle rights are READ/WRITE/WAIT/DUPLICATE/TRANSFER/DESTROY/
  MUTATE, with generic `object_ctl`; the graphics ABI should add narrowly
  defined surface/buffer rights and typed controls only after an ABI review.
- `include/abi/syscall_nums.h` is the shared syscall-number source of truth.
  `user/sys/lib/lib.c` and `lib-2.c` implement wrappers.
- `user/sys/lib/portability/os1_video_platform.h` is the current single
  NexsOS-specific portability seam for SDL/Mesa/D3D. Preserve and evolve it;
  do not patch third-party submodules.

### Actual implementation footprint

The current kernel graphics files total about 7,232 lines in the measured set:
`compositor.c` 3,435; `graphics.c` 221; `surface.c` 51; `gl.c` 297;
`draw2d.c` 242; `draw3d.c` 309; `font.c` 495; `term.c` 508;
`chrome.c` 420; `compositor_style.c` 217; `region.c` 327;
`gpu_core.c` 97; `virtio_gpu.c` 613. This confirms extraction is a
subsystem migration, not a single-file move.

The current `gfx_surface_t` is a typedef of `struct gl_surface`, containing
a raw pixel pointer plus width/height/stride/capacity. `surface.c` currently
implements validity checks AND pixel clear/fill/composite helpers. The NG
surface manager must instead own object metadata and memory lifetime; all
pixel helpers leave the kernel.

`graphics.c` is mixed responsibility: it exposes the primary GPU framebuffer
and performs primitive drawing, but also contains the panic display path.
Split the normal display service from the small panic framebuffer/serial
writer; the latter cannot depend on a userland service.

### Coupling seams to eliminate before extraction

The legacy compositor currently reaches process termination, IPC/input
delivery, scheduler focus state, GPU provider APIs, global font and terminal
state, and kernel allocation/locks. These are explicit extraction blockers.
Replace them with authenticated service ports/events and surface capabilities;
never preserve direct calls by moving the entire old kernel dependency graph
into one privileged service.

### GFX-NG-0 completion status

The syscall inventory, capability substrate, source footprint, userland
portability seam and primary extraction blockers are now documented. The
remaining G0 work is a caller-by-caller semantic inventory for legacy window
operations (especially style/theme, text write, close/kill and input routing)
and checked descriptor layouts for buffer/surface/commit. GFX-NG-1 must not
start by assigning object numbers or syscall numbers before that ABI review.


## 11. Integration with Astra, hotplug and test instrumentation

The compositor's historical IRQ-to-process-context isolation is not the
ring-3 migration. ASTRA §7.10 is clarified by the 2026-11-03 status correction:
the legacy compositor remains linked into the kernel.

Implementation dependencies:
- Surface object lifecycle uses the NEXS-NG object/capability runtime and
  must follow Programme R's semantic syscall crosswalk; do not add an
  independent graphics syscall family.
- Device discovery and runtime removal use
  `NEXS-NG-DEVICE-LIFECYCLE-AND-HOTPLUG.md`; surface buffers must participate
  in the same pin/refcount/DMA quarantine model as GPU/display devices.
- Object counters, surface events and screenshots use
  `NEXS-NG-DIAGNOSTICS-AND-TESTING.md`; every create/map/commit/revoke/destroy
  transition must be observable without rendering from the kernel.
- Full work order and architecture gates:
  `NEXS-NG-INTEGRATED-IMPLEMENTATION-PLAN-2026-11-03.md`.


## Current cutover dependencies — 2026-11-12

1. Reconcile the function catalog with the current linked sources, including
   resource providers already implemented since the historical inventory.
2. Close demonstrated kernel mechanism defects (cwd bounds, transferred-handle
   provenance, wait/stack publication) and establish a reproducible dual-ISA
   baseline. Preserve failing evidence; do not certify a panic-free single run.
3. Reuse buffer_client/gfx_buffer and the existing page-backed capability
   resource. Define only missing surface view/commit operations and lifecycle
   events over the common object transport. Do not add a second allocator,
   scene registry or dedicated graphics syscall family.
4. Cut over one complete window operation family at a time: service state,
   SDK adapters, applications/tests and removal of the superseded kernel
   implementation/dispatcher/veneers in the same verified increment. A
   compatibility adapter, when required, lives in the SDK; no permanent legacy
   kernel graphics renderer or desktop policy may survive the completed cutover.
5. Migrate fonts/text/TTY and remaining raster/chrome/theme algorithms to their
   existing userland owners; preserve the emergency-only kernel path separately.
6. Qualify restart, process death, capability attenuation/revocation, resize,
   concurrent input and actual GUI output through nxadb on AMD64 and AArch64.

The historical [K] function catalog tags describe code retained in the old
implementation, not an exception to the current userland ownership requirement.
