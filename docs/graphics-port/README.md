# Graphics portability programme

This directory is the durable record for the OpenGL, D3D9 and SDL2 port. It
uses the ASTRA split: the kernel exports minimal, capability-mediated objects;
API personalities and their compatibility layers remain in userland.

## Current boundary

The active VirtIO-GPU provider is a 2D ARGB8888 scanout over PCI on amd64 and
VirtIO-MMIO on AArch64. `demo3d` is therefore a userland software renderer
which presents through `SYS_WINDOW_BLIT`; it is not a kernel 3D engine.

The first portability boundary is `user/sys/lib/portability/os1_video_platform.h`.
SDL2, Mesa/OpenGL and the D3D9 personality must depend on that header only;
upstream source trees are never patched for NexsOS glue. The header currently
advertises only the software presentation backend. Its backend selector is the
intentional migration point for a future ASTRA GPU-context object.

## Profiles

| Personality | Initial profile | Later switch |
| --- | --- | --- |
| SDL | SDL2 video/event driver over OS1 windows | GPU-context renderer |
| OpenGL | Mesa software pipeline, ARGB presentation | hardware Gallium driver |
| Direct3D | D3D9 compatibility layer, software presentation | D3D11/12 only after a real GPU submit/fence contract |

No D3D11/12 claim is made while the kernel has only a VirtIO-GPU 2D provider.
Adding it first would create an API with no executable synchronization,
resource, shader or queue semantics.

## ASTRA kernel sequence

1. Keep the kernel's internal `gfx_surface` contract provider-neutral.
2. Model GPU contexts and shared surfaces as object-manager objects with
   handles and capability checks.
3. Add only the required `OS1low_` object/VM primitives when a service cannot
   express an operation through the existing object ABI.
4. Move high-level `OS1_gpu_*` policy into libos1/SRL, then select the GPU
   backend in the portability adapter.

The compositor remains a scene/composition service. It must not acquire
OpenGL/D3D parsing or API state; themes and styles are presentation policy over
the same surface contract.
