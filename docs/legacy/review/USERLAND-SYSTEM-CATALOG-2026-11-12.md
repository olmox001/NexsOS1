# Userland/system source catalog — 2026-11-12

Snapshot made before implementation changes. Source and Makefile are evidence; legacy design documents are historical references, not instructions from the maintainer. Submodules are catalogued at their pinned revision, not represented as reviewed line by line.

## Current ownership and review order

| Domain | Implementation | Access/protocol | Review obligation |
| --- | --- | --- | --- |
| Notifications | user/sys/services/nxntfy_srv.c | system/lib/extensions/services/notify_client.c; nxnotify; notifytest | First: delivery, sender identity, queue bounds, receipts, UI independence |
| Keyboard | user/sys/services/nxkeyboardd.c | nxkeymap; nxkeyboard_layout.h currently under services | Move shared layout into SDK extensions before service changes |
| Execution | user/sys/services/nxexecd.c | execsvc_client.c; nxexec | Review staged resolver/delegation and fallback semantics |
| Time | user/sys/services/nxtimerdd.c | time_client.c; nxtimerdiag | Capability transport, finite waits, malformed requests |
| Diagnostics | user/sys/services/nxpaxd.c | nxpax_service.h; nxpax | Distinguish kernel telemetry from per-process debugger and ADB (not implemented) |
| Graphics | nxcompositor, nxmoused, nxttyd | surface_client.c, buffer_client.c, gfx_buffer.c, gfx/* | Resize/present handshake, backing lifetime, input saturation |
| Fonts | system/lib/font_lib.c; nxfont currently rasterizes/uploads | font_lib.h, contract/font.h | Reuse packed font validation/rendering; asynchronous nxfontdd and SDK extension; emergency kernel font only after consumers migrate |
| Images/video | extensions/image/image.c; extensions/video/video.c | image.h, os1vid.h, nximage | Existing codec algorithms and buffer ownership |
| libc | lib.c, malloc.c, math.c, posix_environment.c, common/* | system/include/api | Error recursion, bounds, fd/cap lifetime, checked I/O; no service implementation duplication |
| UART | kernel/HAL/arch/*/devices/uart_*.c | drivers/console.c, drivers/uart.h | Move device providers outside HAL; preserve early/fault console; usermode driver deferred by request |

The manifest currently installs eight dedicated servers, plus supervised UI/initialization programs. The old ownership audit and tools/test_nxservice_layout.py still describe seven: the structural test must be brought in line with the real TTY addition and optional authority field.

## First-party files (complete tracked/untracked inventory under system and user)

### system/

133 files.

- `system/arch/aarch64/simd_probe.S`
- `system/arch/aarch64/syscall.S`
- `system/arch/amd64/simd_probe.S`
- `system/arch/amd64/syscall.S`
- `system/docs/AUDIT_lib.c.md`
- `system/include/api/alloca.h`
- `system/include/api/assert.h`
- `system/include/api/byteswap.h`
- `system/include/api/ctype.h`
- `system/include/api/dirent.h`
- `system/include/api/elf.h`
- `system/include/api/errno.h`
- `system/include/api/error.h`
- `system/include/api/execsvc.h`
- `system/include/api/fcntl.h`
- `system/include/api/float.h`
- `system/include/api/fnmatch.h`
- `system/include/api/font_lib.h`
- `system/include/api/getopt.h`
- `system/include/api/glob.h`
- `system/include/api/graphics.h`
- `system/include/api/grp.h`
- `system/include/api/image.h`
- `system/include/api/input.h`
- `system/include/api/inttypes.h`
- `system/include/api/langinfo.h`
- `system/include/api/lib.h`
- `system/include/api/locale.h`
- `system/include/api/math.h`
- `system/include/api/nxmanage.h`
- `system/include/api/nxpax_service.h`
- `system/include/api/os1.h`
- `system/include/api/os1_buffer.h`
- `system/include/api/os1_d3d9_present.h`
- `system/include/api/os1_gfx.h`
- `system/include/api/os1_gfx_buffer.h`
- `system/include/api/os1_gl_platform.h`
- `system/include/api/os1_input.h`
- `system/include/api/os1_surface.h`
- `system/include/api/os1_theme.h`
- `system/include/api/os1_time_service.h`
- `system/include/api/os1_tty.h`
- `system/include/api/os1_video_platform.h`
- `system/include/api/os1vid.h`
- `system/include/api/poll.h`
- `system/include/api/pwd.h`
- `system/include/api/selinux/context.h`
- `system/include/api/selinux/label.h`
- `system/include/api/selinux/selinux.h`
- `system/include/api/setjmp.h`
- `system/include/api/signal.h`
- `system/include/api/stdbit.h`
- `system/include/api/stdbool.h`
- `system/include/api/stdckdint.h`
- `system/include/api/stdcountof.h`
- `system/include/api/stdio.h`
- `system/include/api/stdlib.h`
- `system/include/api/string.h`
- `system/include/api/strings.h`
- `system/include/api/sys/ioctl.h`
- `system/include/api/sys/mman.h`
- `system/include/api/sys/random.h`
- `system/include/api/sys/resource.h`
- `system/include/api/sys/stat.h`
- `system/include/api/sys/statfs.h`
- `system/include/api/sys/statvfs.h`
- `system/include/api/sys/time.h`
- `system/include/api/sys/types.h`
- `system/include/api/sys/uio.h`
- `system/include/api/sys/utsname.h`
- `system/include/api/sys/wait.h`
- `system/include/api/termios.h`
- `system/include/api/time.h`
- `system/include/api/uchar.h`
- `system/include/api/unistd.h`
- `system/include/api/wchar.h`
- `system/include/api/wctype.h`
- `system/include/contract/caps.h`
- `system/include/contract/font.h`
- `system/include/contract/nx_abi.h`
- `system/include/contract/nxpax.h`
- `system/include/contract/object.h`
- `system/include/contract/os1_compositor.h`
- `system/include/contract/os1_material_colors.h`
- `system/include/contract/os1_time.h`
- `system/include/contract/posix_types.h`
- `system/include/contract/style_names.h`
- `system/include/contract/surface.h`
- `system/include/contract/syscall_nums.h`
- `system/include/contract/sysstats.h`
- `system/lib/common/string.c`
- `system/lib/common/vsnprintf.c`
- `system/lib/extensions/image/image.c`
- `system/lib/extensions/services/README.md`
- `system/lib/extensions/services/buffer_client.c`
- `system/lib/extensions/services/execsvc_client.c`
- `system/lib/extensions/services/gfx_buffer.c`
- `system/lib/extensions/services/notify_client.c`
- `system/lib/extensions/services/surface_client.c`
- `system/lib/extensions/services/time_client.c`
- `system/lib/extensions/video/video.c`
- `system/lib/font_lib.c`
- `system/lib/gfx/alpha.c`
- `system/lib/gfx/chrome.c`
- `system/lib/gfx/decoration.c`
- `system/lib/gfx/raster.c`
- `system/lib/gfx/theme.c`
- `system/lib/gfx/tty_model.c`
- `system/lib/internal.h`
- `system/lib/lib.c`
- `system/lib/malloc.c`
- `system/lib/math.c`
- `system/lib/os1LUA_lib.c`
- `system/lib/os1LUA_lib.h`
- `system/lib/portability/d3d9/README.md`
- `system/lib/portability/d3d9/os1_d3d9_present.c`
- `system/lib/portability/lua/locale.h`
- `system/lib/portability/lua/lua_portability.c`
- `system/lib/portability/lua/lua_portability.h`
- `system/lib/portability/opengl/os1_gl_platform.c`
- `system/lib/portability/os1_video_platform.c`
- `system/lib/portability/sdl2/README.md`
- `system/lib/portability/sdl2/SDL_config_nexsos.h`
- `system/lib/portability/sdl2/SDL_nexsosevents.c`
- `system/lib/portability/sdl2/SDL_nexsosframebuffer.c`
- `system/lib/portability/sdl2/SDL_nexsostimer.c`
- `system/lib/portability/sdl2/SDL_nexsosvideo.c`
- `system/lib/portability/sdl2/SDL_nexsosvideo.h`
- `system/lib/portability/sdl2/overlay.mk`
- `system/lib/posix_environment.c`
- `system/lib/tests/gfx_chrome_test.c`
- `system/lib/tests/material_palette_contract_test.py`
- `system/lib/tests/tty_model_test.c`

### user/sbin/

1 files.

- `user/sbin/nxinit.c`

### user/sys/

66 files.

- `user/sys/bin/init.cfg`
- `user/sys/bin/nxassoc.h`
- `user/sys/bin/nxbar.c`
- `user/sys/bin/nxenvinit.c`
- `user/sys/bin/nxexec.c`
- `user/sys/bin/nxexec.h`
- `user/sys/bin/nxfilem/draw.c`
- `user/sys/bin/nxfilem/events.c`
- `user/sys/bin/nxfilem/fileops.c`
- `user/sys/bin/nxfilem/main.c`
- `user/sys/bin/nxfilem/nxfilem.h`
- `user/sys/bin/nxfilem/sort.c`
- `user/sys/bin/nxfilem/state.c`
- `user/sys/bin/nxfilem/ui.c`
- `user/sys/bin/nxfont/fonts/Orkney Bold Italic.ttf`
- `user/sys/bin/nxfont/fonts/Orkney Bold.ttf`
- `user/sys/bin/nxfont/fonts/Orkney Light Italic.ttf`
- `user/sys/bin/nxfont/fonts/Orkney Light.ttf`
- `user/sys/bin/nxfont/fonts/Orkney Medium Italic.ttf`
- `user/sys/bin/nxfont/fonts/Orkney Medium.ttf`
- `user/sys/bin/nxfont/fonts/Orkney Regular Italic.ttf`
- `user/sys/bin/nxfont/fonts/Rewir-Light.off`
- `user/sys/bin/nxfont/fonts/Rewir-Light.ttf`
- `user/sys/bin/nxfont/fonts/basic.ttf`
- `user/sys/bin/nxfont/nxfont.c`
- `user/sys/bin/nxicon.h`
- `user/sys/bin/nximage.c`
- `user/sys/bin/nxinfo.c`
- `user/sys/bin/nxinfo.h`
- `user/sys/bin/nxjobs.h`
- `user/sys/bin/nxkeymap.c`
- `user/sys/bin/nxlauncher.c`
- `user/sys/bin/nxline.h`
- `user/sys/bin/nxmemstat.c`
- `user/sys/bin/nxmemstat.h`
- `user/sys/bin/nxnotify.c`
- `user/sys/bin/nxpax.c`
- `user/sys/bin/nxperm.c`
- `user/sys/bin/nxperm.h`
- `user/sys/bin/nxpower.c`
- `user/sys/bin/nxproc.c`
- `user/sys/bin/nxproc.h`
- `user/sys/bin/nxreg.c`
- `user/sys/bin/nxres.c`
- `user/sys/bin/nxres.h`
- `user/sys/bin/nxroleprobe.c`
- `user/sys/bin/nxsettings.c`
- `user/sys/bin/nxshell.c`
- `user/sys/bin/nxtestrunner.c`
- `user/sys/bin/nxtimerdiag.c`
- `user/sys/bin/nxtop.c`
- `user/sys/bin/nxui.c`
- `user/sys/bin/nxwins.c`
- `user/sys/boot/entry`
- `user/sys/boot/manager.conf`
- `user/sys/boot/services.conf`
- `user/sys/services/nxcompositor.c`
- `user/sys/services/nxexecd.c`
- `user/sys/services/nxkeyboard_layout.h`
- `user/sys/services/nxkeyboardd.c`
- `user/sys/services/nxmoused.c`
- `user/sys/services/nxntfy_srv.c`
- `user/sys/services/nxpaxd.c`
- `user/sys/services/nxtimerdd.c`
- `user/sys/services/nxttyd.c`
- `user/sys/services/tests/keyboard_layout_test.c`

### user/bin/

40 files.

- `user/bin/buffertest.c`
- `user/bin/capipc.c`
- `user/bin/capipc_child.c`
- `user/bin/capipc_protocol.h`
- `user/bin/capkill.c`
- `user/bin/capkill_child.c`
- `user/bin/capreg.c`
- `user/bin/captest.c`
- `user/bin/coreutils_test.c`
- `user/bin/counter.c`
- `user/bin/crash.c`
- `user/bin/crash1.c.old`
- `user/bin/crash2.c.old`
- `user/bin/demo3d.c`
- `user/bin/env.c`
- `user/bin/fdtest.c`
- `user/bin/forkbom.c.old`
- `user/bin/forkbomb.c`
- `user/bin/gfxalphatest.c`
- `user/bin/gfxbuffertest.c`
- `user/bin/gnulibtest.c`
- `user/bin/hello.c`
- `user/bin/input_test.c`
- `user/bin/ipc_recv.c`
- `user/bin/ipc_send.c`
- `user/bin/libctest.c`
- `user/bin/luatest.c`
- `user/bin/notifytest.c`
- `user/bin/nxempire.c`
- `user/bin/nxtest.c`
- `user/bin/portidle_child.c`
- `user/bin/raptor.c`
- `user/bin/restest.c`
- `user/bin/sandboxchild.c`
- `user/bin/sandboxtest.c`
- `user/bin/sdltest.c`
- `user/bin/simdchild.c`
- `user/bin/simdtest.c`
- `user/bin/stress.c`
- `user/bin/writetest.c`

### user/lib/

3 files.

- `user/lib/pl_mpeg/pl_mpeg.h`
- `user/lib/stb/stb_easy_font.h`
- `user/lib/stb/stb_image.h`

### user/portability/

31 files.

- `user/portability/coreutils/README.md`
- `user/portability/coreutils/configmake.h`
- `user/portability/coreutils/coreutils_config_nexsos.h`
- `user/portability/coreutils/coreutils_os1_glue.c`
- `user/portability/coreutils/coreutils_os1_glue.h`
- `user/portability/coreutils/overlay.mk`
- `user/portability/coreutils/patches/README.md`
- `user/portability/coreutils/patches/system.h.patch`
- `user/portability/coreutils/system.h`
- `user/portability/coreutils/unitypes.h`
- `user/portability/coreutils/uniwidth.h`
- `user/portability/coreutils/version.c`
- `user/portability/coreutils/version.h`
- `user/portability/gnulib/README.md`
- `user/portability/gnulib/config.h`
- `user/portability/gnulib/fts_.h`
- `user/portability/gnulib/gmp.h`
- `user/portability/gnulib/gnulib_config_nexsos.h`
- `user/portability/gnulib/gnulib_os1_glue.c`
- `user/portability/gnulib/gnulib_os1_glue.h`
- `user/portability/gnulib/hard_locale_compat.c`
- `user/portability/gnulib/locale_compat.c`
- `user/portability/gnulib/malloc/scratch_buffer.gl.h`
- `user/portability/gnulib/mbs_compat.c`
- `user/portability/gnulib/obstack.h`
- `user/portability/gnulib/os1_time.c`
- `user/portability/gnulib/overlay.mk`
- `user/portability/gnulib/parse-datetime.c`
- `user/portability/gnulib/patches/README.md`
- `user/portability/gnulib/patches/obstack.h.patch`
- `user/portability/gnulib/utime.h`

## Pinned external components

| Path | Revision |
| --- | --- |
| `user/bin/base-nexs` | `0e0d035247a583cdaf545305aa9f3d4dc346fb02` |
| `user/bin/busybox` | `1a64f6a20aaf6ea4dbba68bbfa8cc1ab7e5c57c4` |
| `user/bin/coreutils` | `26a3a40600498b9687bd155713697c02f7f5ebd2` |
| `user/bin/doom` | `d2cd8c1c5c08b2fc575db4eed238d2fea9755aae` |
| `user/bin/kilo` | `27c132fcacb14873c81259132f6baeb34f8bf770` |
| `user/lib/direct3d` | `6eb2e4c32cc9e271856146df11ed3a5c2cf29234` |
| `user/lib/gnulib` | `de054f5dc8906dbac0054e113d7d4fd0430a3398` |
| `user/lib/lua` | `97aa15e32617242e6d9f7de6f5ee654e0844fe38` |
| `user/lib/musl` | `bccff41864cb1b051d5864ba664bdcaccb97e00a` |
| `user/lib/opengl` | `742a20f48c59e8649533c84c4d49dd95b403f5da` |
| `user/lib/sdl` | `ce4fbcf9be388d514d02808a6becabe0988d7c8b` |

## Documentation provenance

Current audits in docs/architecture and this review must be checked against current sources. docs/review/analysis, docs/direction, July reports and migration plans can explain previous intent but do not establish present behavior. In particular nxfont still claims the kernel retains raw user pointers, while kernel/graphics/font.c already copies and validates the blob. The runtime and structural gates remain independent of compilation success.

## Acceptance ledger

- Initial staged and unstaged patches preserved separately in /tmp/nexs-review-{staged,unstaged}.patch; original status in /tmp/nexs-review-status.txt. All changes unstaged for incremental review.
- No PR has been published and no existing local work has been discarded.
- Pending: clean builds and shell interaction via nxadb for aarch64 and amd64; full guest suite; resize screenshots/input; Power12/NXPAX gates.


## Repository ownership update — 2026-11-12

The source inventory above remains the ownership catalog for the first-party
SDK and userland implementation, but `system/` is now the pinned NX-SDK
gitlink at `13d5168d88e432f79e4fb457d5c81eb863201616`, not a source directory
owned directly by the integration repository. NX-SDK additionally owns its
standalone Makefile, `sdk.mk`, contract revision snapshots and local contract
checker. `kernel/` is the pinned NX-CORE gitlink at
`7d4045e5d9be39381ce1ebe669567bfb9c449856`.

The integration repository continues to own `user/`, including the eight
dedicated service executables, system commands, boot/service manifest and
third-party application/library gitlinks. The service count remains 14 named
manifest entries, 8 dedicated server entrypoints and 7 SDK service-client
modules. `tools/test_nxservice_layout.py` now validates the canonical
`system/sdk.mk` client manifest rather than requiring a duplicate linker list
in the superproject Makefile.
