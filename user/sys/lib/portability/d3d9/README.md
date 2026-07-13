# D3D9 presentation backend

`os1_d3d9_present.{h,c}` is the presentation seam for the Direct3D 9
personality: a swapchain with a CPU ARGB8888 backbuffer and D3D9
Lock/Unlock/Present/Reset semantics over `os1_video_platform.h`. Dispatch is
backend-independent (an ops table selected from the platform's advertised
backend), so the software presenter can later be swapped for the ASTRA
GPU-context object without touching personality callers.

This is deliberately NOT a Direct3D API implementation: device/context/shader
dispatch belongs to the D3D9 compatibility layer sourced from the Wine
submodule on its `nexsos-port-d3d9` branch. Only the OS-facing presentation
glue lives here, per the non-negotiable constraint that upstream trees are
never patched for NexsOS integration.

`demo3d` is the reference client: it renders its cube into the locked
backbuffer and presents through the swapchain, exercising the chain end to
end on both architectures.
