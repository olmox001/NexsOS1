# 2026-07-11 — design: capability-owned GPU-context object

Increment 5 of the plan in `STATE.md`: design the GPU-context object BEFORE
any new ABI number or hardware-accelerated path exists. This is a design
record only — it deliberately assigns no syscall numbers, no `OBJ_TYPE_`
values and no rights bits; those are chosen at implementation time against
the then-current shared ABI in `<object.h>`.

## Constraints inherited from the programme

- No ambient-authority GPU syscall (`STATE.md`, non-negotiable). Every GPU
  operation is invoked on a handle whose rights are checked per ASTRA 6.1/6.2.
- The kernel keeps only fundamental logic: contexts, surfaces, fences and the
  submit path. API semantics (GL state machines, D3D devices, SDL renderers)
  stay in userland personalities.
- No D3D11/12 claim until this object gives a real submit/fence contract
  (`README.md`: an API with no executable synchronization semantics must not
  exist).

## Objects

Three kernel objects, following the existing object-manager model
(`kernel/include/kernel/object.h`: kobject + handle slot = capability with a
separable rights subset, `sys_handle_create/dup/close`, `sys_cap_grant`):

1. **GPU context** — the unit of GPU work ownership. Owns a command queue on
   the provider, an address space of bound surfaces, and accounting. One
   process may hold several contexts; a context never outlives its final
   handle. Rights: submit, bind-surface, query, grant.
2. **GPU surface** — a provider-neutral image resource (initially ARGB8888
   only, matching `gfx_surface`). Owned by a context or standalone;
   shareable cross-process by `sys_cap_grant` with reduced rights (e.g.
   sample-only, no CPU map). Rights: cpu-map, bind, present-source, grant.
3. **GPU fence** — the synchronization primitive a submit returns. Waitable
   through the existing handle-wait mechanisms; signalled by the provider's
   completion interrupt. Rights: wait, query.

The compositor consumes `present-source` surfaces; it stays a
scene/composition service and acquires no API state (`README.md` invariant).

## Operation mapping

Express operations through the EXISTING object ABI first (`STATE.md` ASTRA
sequence step 3):

- create/dup/close/grant → the existing handle syscalls, new object types.
- context/surface property queries → the existing object-info path (as
  `struct window_info` does for windows, §6.7).
- CPU map of a surface → the existing VM object-mapping primitive if it can
  carry (object, rights) — only if it cannot, a single new `OS1low_` mapping
  primitive is justified.
- submit + fence → the one genuinely new `OS1low_` primitive
  (`submit(context_handle, command_buffer, out_fence)`); nothing else gets a
  new ABI number in the first iteration.

## Migration seam (already in place)

`os1_video_platform.h` advertises `OS1_VIDEO_BACKEND_GPU_CONTEXT = 2` as a
reserved backend. The consumers are already dispatch-ready:

- the SDL2 `nexsos` driver overlay talks only to the portability header;
- the D3D9 presentation chain (`os1_d3d9_present.c`) selects an ops table by
  the advertised backend — a GPU-context backend is one more table;
- `OS1_gpu_*` policy (queue selection, format negotiation) belongs in
  libos1/SRL, not in the kernel and not in the personalities (ASTRA step 4).

## Provider reality check

The active provider is a VirtIO-GPU 2D scanout. Until a provider with real
command submission exists (VirtIO-GPU 3D/virgl or host GPU passthrough), the
GPU-context object must NOT be implemented — a software emulation of
submit/fence would recreate the "API without executable semantics" problem
this design exists to prevent. Implementation is gated on:

1. a provider exposing a hardware queue;
2. the fence interrupt path;
3. kernel unit tests for handle lifecycle/rights on the new types;
4. `make run` validation on both architectures, as for every increment.
