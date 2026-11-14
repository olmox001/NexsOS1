# 2026-07-12 — OpenGL front opened: seam, Mesa repin and findings

New increment record (documentation is append-only).

## Survey findings that shape the front

1. **Mesa 26.x removed OSMesa.** The submodule was pinned at 26.2.0-devel,
   whose tree no longer contains `src/gallium/frontends/osmesa` (removed at
   `25.1-branchpoint~1687`, commit `027ccd963b1`). OSMesa —
   render-into-caller-buffer software GL — is exactly the profile the
   programme targets ("Mesa software pipeline, ARGB presentation"), and the
   modern alternatives (dri/gbm loaders) are dlopen-shaped and unfit for a
   freestanding static port.
2. **Repin: `nexsos-port-opengl` → `mesa-25.0.7`**, the newest release
   still shipping OSMesa (and gallium `softpipe` as its software
   rasterizer). The branch carried no port commits, so the move is free.
   Local submodule updated; the fork branch move needs a maintainer
   force-push (see "pending" below).
3. **Bonus finding — Gallium Nine.** `mesa-25.0.7` also ships
   `src/gallium/frontends/nine`, a native Direct3D 9 implementation over
   gallium. For the D3D9 personality this is a candidate source
   dramatically lighter than Wine's d3d9→wined3d stack, sharing the same
   softpipe backend the GL profile needs. Decision deferred to the D3D9
   personality increment; the Wine submodule stays as the reference for the
   API surface.
4. **Threads gate, resolved by policy.** KTHREAD-STATUS.md (S-STAB)
   forbids kernel threads; userland processes are the model. Mesa core
   requires C11-thread symbols but a single-context software GL exercises
   them only as guards, so the port plan uses a single-thread c11-threads
   stub in the portability layer (mtx/cnd trivial, thrd_create = error) —
   no Mesa patching, no kernel threads.

## Delivered in this increment

`user/sys/lib/portability/opengl/os1_gl_platform.{h,c}` — the presentation
seam, compiled into the userland library on both architectures:

- an ARGB8888 top-down CPU colour buffer bound to an OS1 window — the exact
  shape `OSMesaMakeCurrent` + `OSMESA_Y_UP=0` binds;
- `swap` = full-buffer present + compositor render over os1_video_platform;
- `resize` follows the nxlauncher adopt-and-realloc contract (buffer pointer
  invalidated, re-fetched via `os1_gl_surface_buffer`);
- backend field mirrors `os1_video_backend`; the CPU buffer is the declared
  swap-in point for the ASTRA GPU-context object, symmetric with the D3D9
  presentation chain.

No Mesa headers are referenced: the seam builds ahead of the library and is
exercised for real when the OSMesa cross-build lands (next increments:
meson cross-file profile for OSMesa+softpipe over the OS1 libc, the
c11-threads stub, then a glcube demo through the seam).

## Pending maintainer action

The fork branch move requires a force-push the automation is not allowed to
perform:

    git -C user/sys/lib/opengl push -f origin nexsos-port-opengl

After it, the superproject gitlink bump to `mesa-25.0.7` (742a20f48c5) gets
committed; until then the gitlink intentionally stays at the old pin so
recursive clones keep working.

## Validation

`make` and `make ARCH=amd64` green with the seam in USER_LIB_O; `make run`
on both architectures: kernel tests passing, services up, no panic (the
seam adds no runtime path until a GL client exists).
