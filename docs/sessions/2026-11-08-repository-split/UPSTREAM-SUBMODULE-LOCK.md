# Upstream submodule state before SDK/source relocation

## .gitmodules
[submodule "user/bin/base-nexs"]
	path = user/bin/base-nexs
	url = https://github.com/olmox001/base-nexs.git
	branch = os1-support
[submodule "user/bin/kilo"]
	path = user/bin/kilo
	url = https://github.com/olmox001/kilo.git
	branch = nexsos-port
[submodule "user/bin/doom"]
	path = user/bin/doom
	url = https://github.com/olmox001/doomgeneric.git
	branch = nexsos-port
[submodule "user/sys/lib/sdl"]
	path = user/sys/lib/sdl
	url = https://github.com/olmox001/SDL.git
	branch = nexsos-port-sdl2
[submodule "user/sys/lib/opengl"]
	path = user/sys/lib/opengl
	url = https://github.com/olmox001/mesa.git
	branch = nexsos-port-opengl
[submodule "user/sys/lib/direct3d"]
	path = user/sys/lib/direct3d
	url = https://github.com/olmox001/wine.git
	branch = nexsos-port-d3d9
[submodule "user/sys/lib/lua"]
	path = user/sys/lib/lua
	url = https://github.com/olmox001/nxlua.git
	branch = nexsos-por
[submodule "user/sys/lib/musl"]
	path = user/sys/lib/musl
	url = https://github.com/olmox001/musl.git
	branch = nexsos-port
[submodule "user/bin/busybox"]
	path = user/bin/busybox
	url = https://github.com/olmox001/busybox.git
	branch = nexsos-port
[submodule "user/sys/lib/gnulib"]
	path = user/sys/lib/gnulib
	url = https://github.com/olmox001/gnulib.git
	branch = nexsos-port
[submodule "user/bin/coreutils"]
	path = user/bin/coreutils
	url = https://github.com/olmox001/coreutils_NexsOS1.git
	branch = nexsos-port

## recursive submodule status
 0e0d035247a583cdaf545305aa9f3d4dc346fb02 user/bin/base-nexs (V0.1.7-2-g0e0d035)
 41c950e96d0502b07acf34f63e69712f55a25ed0 user/bin/base-nexs/reference (fast_release-14-g41c950e)
 1a64f6a20aaf6ea4dbba68bbfa8cc1ab7e5c57c4 user/bin/busybox (1_12_0-8278-g1a64f6a20)
 26a3a40600498b9687bd155713697c02f7f5ebd2 user/bin/coreutils (heads/nexsos-port)
+de054f5dc8906dbac0054e113d7d4fd0430a3398 user/bin/coreutils/gnulib (v1.0-3305-gde054f5dc8)
 f75381ece567b683d2894659630f5559e5b0ba9d user/bin/doom (heads/nexsos-port)
 780958a03285142f3c7df0ef8f58f5a8fb5b81e3 user/bin/kilo (heads/nexsos-port)
 6eb2e4c32cc9e271856146df11ed3a5c2cf29234 user/sys/lib/direct3d (wine-11.13)
 de054f5dc8906dbac0054e113d7d4fd0430a3398 user/sys/lib/gnulib (v1.0-3305-gde054f5dc8)
 97aa15e32617242e6d9f7de6f5ee654e0844fe38 user/sys/lib/lua (heads/nexsos-por)
 bccff41864cb1b051d5864ba664bdcaccb97e00a user/sys/lib/musl (v1.2.6-1-gbccff418)
 742a20f48c59e8649533c84c4d49dd95b403f5da user/sys/lib/opengl (mesa-25.0.7)
 ce4fbcf9be388d514d02808a6becabe0988d7c8b user/sys/lib/sdl (release-2.32.0-223-gce4fbcf9b)

## dirty nested working trees
user/bin/coreutils
 M gnulib
