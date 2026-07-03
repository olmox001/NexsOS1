# Userland Audit — 2026-07-02

> Full read-only audit of `user/sys/bin/*.c|*.h`, `user/sys/lib/*.c`,
> `include/api/{os1,caps,object,image,input,posix_types,syscall_nums}.h`, and
> `user/bin/*.c` (top-level; `user/bin/doom/*` internals skipped) against
> `docs/ASTRA.md` §1, §6.4, §6.5, §7.4 and
> `docs/direction/DIR-04-capabilities-and-services.md`.
>
> Repo state audited: branch `comprehensive-review`, HEAD `6e394cf`
> ("fix init spawn"), tag v0.0.4.4. **This document performs no edits and no
> commit**; it is the only new file this pass wrote.
>
> Severity scale used below (per instruction): **W1** trivial/doc,
> **W2** moderate (real limitation, no runtime risk), **W3** significant
> (bug/security weakness under plausible conditions), **W4** severe
> (broken core functionality / data-corruption / crash on a common path).
> This is a coarser 4-level subset of the repo's own `docs/review/TAXONOMY.md`
> W0–W5 scale (mapped 1:1 onto its W1–W4, dropping W0/W5 for this pass).
>
> A **prior** userland audit exists at `docs/review/analysis/08-userland.md`.
> It predates the capability/object-manager model (ASTRA §7.1), the
> `nxproc`/`nxntfy_srv` rename (`27cf792`), and every `nx*` stratified-service
> file below (`nxlauncher`, `nxwins`, `nxperm`, `nxinfo`, `nxmemstat`,
> `nxres`'s current form) — it still cites `proce.c`, `notification_server.c`
* and stale line numbers. It is **not corrected here** (out of scope: no edits
> to existing files) but should be treated as superseded by this document for
> the files both cover.

---

## 1. Classification table

Legend: **SERVICE** = SRL (ASTRA §6.4), supervised, a mechanism other
processes/the system depends on. **SYSTEM APP** = user-facing tool shipped
in `/sys/bin` but not itself a dependency of other processes. **USER APP** =
ships in `/bin`, USER-level preset. **HELPER LIB** = header-only or
link-only code, never itself a process. **DEV-TEST** = harness/demo under
`/bin`, developer tooling rather than a shipped user app.

### `user/sys/bin` (per-path preset: PLVL_ROOT, `kernel/core/syscall_dispatch.c:176-180`)

| Binary | Class | Role (1 line) | Spawn source (file:line) |
|---|---|---|---|
| `init.c` → `init.elf` (PID 1) | **SERVICE** (undivided U1+U2 today) | Boots + supervises the 4 core processes; owns `srv.notify_pid` registry publication | Kernel ELF loader (boot), not spawned by any userland call |
| `nxntfy_srv.c` | **SERVICE** (SRL notification) | IPC popup + registry-ring notification log; the endpoint every `notify()` call resolves via `srv.notify_pid` | `init.c:111` (boot), `init.c:211` (respawn, supervisor loop) |
| `nxui.c` | **SERVICE** (SRL Window Server / dock, ASTRA §7.3) | Enumerates windows, draws the dock, owns focus/minimize/restore *policy* over the compositor *mechanism* | `init.c:125` (boot), `init.c:234` (respawn) |
| `nxlauncher.c` | **SYSTEM APP** (spawner/launcher UI) | Resizable app-grid window; scans `/sys/bin`+`/bin` live, spawns on tile click | `init.c:142` (boot, gated `LAUNCHER_AUTOSTART`), `init.c:245` (respawn) |
| `nxshell.c` | **SYSTEM APP** (interactive shell/TTY) | Character-mode command dispatcher in its own window; the `exec`/unknown-command fallback for every other CLI tool | `init.c:152` (boot); **respawn compiled out** whenever `LAUNCHER_AUTOSTART` is 1 (`init.c:219-227`, `#ifndef LAUNCHER_AUTOSTART` guards the shell-respawn branch) |
| `nxproc.c` + `nxproc.h` | `.c`=**SYSTEM APP**, `.h`=**HELPER LIB** | One-shot `ps`/`kill` process-list CLI; `.h` is the reusable snapshot/render helper ASTRA §7.4 names as the pattern | `nxshell.c:284` (`ps` builtin) + reachable as a generic launcher tile (`nxlauncher.c` `scan_bin()`, not filtered) |
| `nxtop.c` | **SYSTEM APP** | Windowed 1×/3s realtime process monitor, consumes `nxproc.h` | **No spawn site anywhere** (grep-confirmed); only the generic shell PATH-fallback / launcher tile |
| `nxres.c` | **SYSTEM APP** (system-wide write effect) | CLI: resolution / compositor style / theme / zoom | **No spawn site**; only shell fallback — but the *only* one of the unsupervised 5 that appears in `nxshell.c:238`'s `help` text |
| `nxmemstat.c` + `nxmemstat.h` | `.c`=**SYSTEM APP** (service-shaped, unsupervised), `.h`=**HELPER LIB** | Live system-stats window / headless CSV logger; only userland path to the ROOT-gated `OS1_sys_stats()` | **No spawn site**; not in shell help; own header (`nxmemstat.c:12-15`) calls itself a "ROOT service" but nothing spawns/supervises it |
| `nxperm.c` + `nxperm.h` | `.c`=**SYSTEM APP** (introspection stub), `.h`=**HELPER LIB** | `whoami`/`levels`/`services`; **not** the login/su/UAC manager the maintainer envisions — self-labeled "FOUNDATION (introspection)" (`nxperm.c:3`, `nxperm.h:6,16-19`) | **No spawn site**; not in shell help |
| `nxinfo.c` + `nxinfo.h` | `.c`=**SYSTEM APP**, `.h`=**HELPER LIB** (single-consumer) | One-shot system summary (version/uptime/proc-count/resolution/pid/cwd) | **No spawn site**; not in shell help |
| `nxwins.c` | **SYSTEM APP** | `nxwins` / `nxwins focus <id>` — textual twin of the dock, extracted from the old shell `wins` builtin | **No spawn site**; advertised in `nxshell.c:241` help text but has **no dedicated builtin** (inconsistent with `ps`→`nxproc`'s pattern) |
| `regedit.c` | **SYSTEM APP (broken)** | Registry "Control Panel" viewer | **No spawn site**, **not even in shell help** — reachable only by manually clicking its auto-discovered launcher tile; **hangs forever on the first frame** (§3, finding U-1) |
| `nxnotify.c` | **SYSTEM APP** (thin CLI) | `nxnotify send/warn <msg>` (IPC via `notify()`), `nxnotify list` (reads the `sys.ntfy.log.*` registry ring `nxntfy_srv` writes) | **No spawn site**; advertised in `nxshell.c:242` help text, reached via generic fallback like `nxwins`/`regedit` |
| `fontman/fontman.c`, `nexs-fm/` | *(out of strict scope — subdirectories, not top-level `user/sys/bin/*.c`)* | TTF rasterizer (`fontman`) / file manager (`nexs-fm`), both built into `/sys/bin` per `Makefile:326-327` | Not investigated this pass; flagged only so the classification table isn't silently incomplete |

### `user/sys/lib` (linked into every ELF — HELPER LIB by definition)

| File | Role | Notable state |
|---|---|---|
| `lib.c` | Sole userland C runtime: syscall veneers, printf/vsnprintf, POSIX shims, notify()/registry wrappers, stb_image | Self-documents its own known issues (USR-LIB-01..05, USR-SEC-01/02, USR-BLOAT-01/02) at `lib.c:30-46` |
| `malloc.c` | First-fit + forward-only-coalescing heap allocator, `sbrk()`-backed | Self-documents USR-MALLOC-01..06 (`malloc.c:22-39`); confirmed real 8-byte-not-16 alignment bug (§3, finding L-2) |
| `font_lib.c` | Parses the packed-bitmap `.off` font format (unrelated to 3D-mesh `.off`); `#include`d into `lib.c`, never compiled standalone | Well-hardened `font_validate_buffer()`; two latent (not exploitable via current callers) overflow risks in the render path (§3, finding L-3) |
| `sdl_OS1_layer.c` | SDL 1.2→OS1 shim (video/keyboard/mouse; explicitly "no audio") | **Confirmed 100% dead code** — zero references anywhere, not compiled by any Makefile rule, not used by the real Doom port (§3, finding L-1) |

### `user/bin` (per-path preset: PLVL_USER; explicit `BIN_ELFS` list, `Makefile:332-343`)

All 21 files here are **DEV-TEST** harnesses/demos, not real user-facing applications, with one arguable exception:

| File | Purpose | Notes |
|---|---|---|
| `capipc.c`/`capipc_child.c`, `capkill.c`/`capkill_child.c`, `capreg.c`, `captest.c`, `nxtest.c`, `sandboxtest.c`/`sandboxchild.c`, `fdtest.c`, `writetest.c` | Capability/object/W^X/sandbox/fd/VFS regression tests | Clean; no significant findings |
| `counter.c`, `crash.c` | Compositor smoke test / deliberate fault-handler test | Both self-document the shared USR-BLOAT-01/02 root cause (lib.o drags in stb_image + DWARF for trivial binaries) |
| `demo3d.c` | Real-time fixed-point 3D cube rasterizer | The one file that reads as a genuine showcase app, not a PASS/FAIL harness |
| `forkbomb.c` | Quota/DoS exhaustion test | **Name is misleading but implementation is DIR-04-compliant**: uses `spawn()` in a loop (`forkbomb.c:65`), never `fork()` — confirmed zero occurrences of `fork(` in the file |
| `hello.c` | Bare-metal UART smoke test, no `os1.h`/libc at all | Outlier: doesn't use `main()`, writes to `0x09000000` via inline asm |
| `input_test.c`, `ipc_send.c`/`ipc_recv.c`, `stress.c` | Interactive/soak-test diagnostics | `ipc_send.c:22` still hardcodes a PID-3 fallback the `ipc_recv.c:11-13` comment says was fixed on the recv side only (asymmetric fix) |
| `test_init.c` | 10-line stub | **Confirmed genuinely dead**: absent from `BIN_ELFS` (`Makefile:332-343`), zero references anywhere in the tree |

---

## 2. Spawn/exec model (base for the planned `nxexec`)

### 2a. nxshell — child launch/tracking

**`spawn_search_args()`** (`user/sys/bin/nxshell.c:148-164`): resolves `argv[0]`
against `/bin/<name>` first, falls back to `/sys/bin/<name>`; absolute paths
(`name[0]=='/'`) bypass search entirely. Calls `spawn_args()` (→
`OS1low_process_spawn`, `lib.c:169-171` → `_sys_spawn` → kernel
`dispatch_spawn`, `kernel/core/syscall_dispatch.c:187-219`).

**`run_foreground(pid)`** (`nxshell.c:200-217`) — the debounce/detach logic:
```
while (1):
  if window_of_pid(pid) > 0: break        # child opened its own window → detached
  if wait(pid) != -1:        break        # child finished (dead/zombie/gone)
  try_recv(-1, &m); if IPC_TYPE_INPUT and payload[0]==0x03 (Ctrl-C):
      kill_process(pid); break
  yield()
```
`window_of_pid()` (`nxshell.c:204`, → `OS1_window_of_pid` → `SYS_WINDOW_OF_PID`,
kernel `syscall_dispatch.c:390-398` → `compositor_get_window_by_pid`) is the
**debounce**: a windowless CLI tool is polled in a tight `yield()` loop and
its Ctrl-C is captured by the shell; a tool that opens its own window is
detected on the *first* poll pass and the shell returns to the prompt
immediately, leaving the child running independently. Called from the `ps`
builtin (`nxshell.c:287`), `exec <prog>` (`nxshell.c:413`), and the generic
unknown-command fallback (`nxshell.c:436`) — i.e. **every** path that spawns
a foreground job in nxshell goes through `run_foreground`.

**stdout routing** — NOT implemented in userland at all; it is a **kernel-side,
spawner-agnostic mechanism**:
- `kernel/sched/process.c:733-746` (`process_create`): every freshly created
  process gets `proc->ctty_win = -1`, then, if it has a creator,
  `proc->ctty_win = (term > 0) ? term : creator->ctty_win` where
  `term = compositor_get_window_by_pid(creator->pid)`. This is
  **unconditional on every spawn**, regardless of who the spawner is —
  the kernel has no concept of "spawner is a shell."
- `kernel/core/object.c:652-662` (`OBJ_TYPE_CONSOLE` write handler, backing fd
  1/2): resolves the **caller's own window first**
  (`compositor_get_window_by_pid(current_process->pid)`); only if that is
  `<= 0` does it fall back to `current_process->ctty_win`.
- Net effect: `write(1, ...)` (used by `print`/`printf`, `lib.c:429,436`)
  always lands somewhere — the process's own window if it has one, else its
  *inherited* `ctty_win`, which is **whichever process spawned it, or that
  spawner's own inherited ctty**, propagated down the tree
  (`kernel/include/kernel/sched.h:99-104`).

**`window_of_pid` debounce, same fd-1 dependency**: `SYS_WINDOW_OF_PID`
(`kernel/core/syscall_dispatch.c:390-398`) just calls
`compositor_get_window_by_pid(pid)` — the identical PID→primary-window lookup
`sys_object_write`'s CONSOLE branch uses for the "own window first" check.
There is no IPC message and no separate protocol; it is a pure read of
compositor window-ownership state, called synchronously by whichever process
asks.

### 2b. nxlauncher — the "believes it IS a shell" bug, pinned down

`nxlauncher.c` spawns a clicked tile at `handle_click()`:
```c
// nxlauncher.c:896
int pid = spawn_args(g_apps[idx].path, 0, (char *const *)0);
if (pid <= 0) { printf("[launcher] spawn failed: ..."); }
else { OS1_window_minimize(g_win); }   // nxlauncher.c:904
```
There is **no `run_foreground` equivalent, no `window_of_pid` poll loop, no
wait, no stdout-forwarding UI, no Ctrl-C handling** — the launcher fires the
spawn and *immediately* backgrounds itself (`OS1_window_minimize(g_win)`,
`nxlauncher.c:904`) so the new app gets focus. It never checks whether the
child opened its own window.

**Exact mechanism of the bug**: kernel-side `ctty_win` inheritance
(`kernel/sched/process.c:741-746`) does not distinguish "my creator is an
interactive shell" from "my creator is a launcher." When nxlauncher spawns a
windowless CLI tool (anything that never calls `create_window`), that child's
`ctty_win` is set to `compositor_get_window_by_pid(nxlauncher_pid)` —
**nxlauncher's own window** — because nxlauncher itself has a window
(`nxlauncher.c:969`, `create_window(...)`). The child's subsequent
`write(1, ...)` then resolves (via `object.c:657-661`) to nxlauncher's window,
exactly as if nxlauncher *were* the controlling shell — because, from the
kernel's point of view, structurally it is: **any windowed spawner
unconditionally becomes ctty for its windowless children.** The child
"believes it is running under a shell" because the ctty-inheritance mechanism
makes no distinction between a POSIX-like interactive shell and any other
windowed process that happens to call `spawn()`.

Consequences visible in practice:
- Output from a windowless tool launched by clicking a launcher tile is
  delivered into the **launcher's own window** — but the launcher immediately
  minimizes itself right after spawning (`nxlauncher.c:904`), so that output
  is written into a now-backgrounded/hidden window the user cannot see
  without manually restoring the launcher from the dock.
- No Ctrl-C capture exists on this path (only `nxshell.c`'s `run_foreground`
  implements that), so a windowless tool launched from the launcher cannot be
  interrupted by the user at all.
- The launcher's own `printf("[launcher] spawn failed: %s\n", ...)`
  (`nxlauncher.c:898`) diagnostic itself is subject to the identical routing —
  it is written to the launcher's *own* window (own-window-first path,
  `object.c:657-659`), so it is at least visible pre-minimize, but any output
  the *child* produces after the minimize call is not.

**Root cause, restated for `nxexec`'s design**: the kernel's ctty model
(`sched.h:99-104`) is a single, undifferentiated "windowed creator" concept.
There is no `SPAWN_FLAG_INTERACTIVE_PARENT` / "needs a shell" bit anywhere in
`_sys_spawn`/`_sys_spawn_caps` (`os1.h:51-52`) or `dispatch_spawn`
(`syscall_dispatch.c:187-219`) that a spawner could set to say "I am a
launcher, not a terminal — do not make me this child's ctty." `nxexec`, per
the maintainer's stated split (standalone vs needs-shell), would need either
(a) a new spawn flag/verb that opts a child OUT of ctty inheritance, or (b) a
policy where non-shell spawners (nxlauncher) spawn with an explicit
"detached" mode that never sets `ctty_win`, forcing windowless children
spawned that way to fail closed (no output surface) rather than silently
landing in a soon-to-be-hidden window.

### 2c. How nxlauncher's respawn is gated in init.c

- `init.c:58` — `#define LAUNCHER_AUTOSTART 1`. A single macro gates **three**
  things at once: the initial `spawn("/sys/bin/nxlauncher")` at `init.c:142`
  (inside `#if LAUNCHER_AUTOSTART`), the respawn branch at `init.c:242-247`
  (inside the same `#if`), **and** — as a side effect of `#ifndef
  LAUNCHER_AUTOSTART` at `init.c:219` — the **shell's own respawn branch**
  (`init.c:222-226`) is compiled out whenever the launcher is on. With the
  default `LAUNCHER_AUTOSTART=1`, if `nxshell` dies, **nothing respawns it**
  (only the launcher and dock get supervised); flipping the macro to 0
  restores shell respawn but disables the launcher entirely. This coupling
  is undocumented as a trade-off anywhere except the inline comment at
  `init.c:219-227`, and is a genuine correctness gap in the supervisor (a
  crashed shell with the current default build is never recovered without a
  reboot).
- `pid_nxlauncher` is declared unconditionally (`init.c:135`) and pinned to
  `0` when the gate is off, specifically so `wait(0)` returns `-2` ("gone")
  and the `#if`-gated respawn block never fires — a deliberate, working
  pattern, not a bug.

---

## 3. Findings (dead/leftover code, stale comments, duplication, ASTRA/DIR-04 violations)

Each entry: `file:line`, severity, one-line micro-fix.

### Dead / leftover code

| ID | file:line | Sev | Finding | Micro-fix |
|---|---|---|---|---|
| D-1 | `user/sys/lib/sdl_OS1_layer.c` (whole file, 383 lines) | **W2** | 100% dead/orphaned SDL1.2→OS1 shim. Zero references anywhere in the repo; not compiled by any Makefile rule (no wildcard glob over `user/sys/lib/*.c`); the real Doom port uses its own `i_video.c`/`i_input.c`, not this file. Self-declared internal name (`// sdl12_os1_shim.c`, line 1) doesn't even match the on-disk filename. | Delete, or move to a clearly-unbuilt `contrib/`/`wip/` location with a note. |
| D-2 | `user/bin/test_init.c` (10 lines) | **W1** | Confirmed genuinely dead: absent from `BIN_ELFS` (`Makefile:332-343`), zero references anywhere else in the tree. | Delete. |
| D-3 | `user/sys/bin/regedit.c:104-114` | **W2** | `compositor_render()`/`yield()` after the blocking `recv(0,&msg)` are unreachable dead code — a direct consequence of finding U-1 below. | Fixed by U-1's fix (switch to `try_recv`). |
| D-4 | `user/sys/bin/regedit.c:86,88` | **W1** | `window_print(...)` calls are commented out and reference a function that **does not exist anywhere** in `os1.h`/`lib.c` — not a disabled-but-present API, aspirational only. Comments around it are accurate/honest about this (not stale), just noting the dead reference itself. | No action needed beyond what the file already documents; remove the commented-out calls once text rendering lands or is abandoned. |
| D-5 | `user/sys/bin/regedit.c:96-102` | **W1** | "Workaround: Use `notify()` to show current values when 'clicked'" — describes an idea that was **never implemented**; no `notify()` call exists anywhere in the file. | Remove the stale note or convert to a `// TODO:`. |
| D-6 | `user/sys/bin/nxmemstat.h:60-62` | **W1** | `unsigned long mb = 4096UL / 1024UL; (void)mb;` — computed, immediately discarded; the real conversion is done independently a few lines below without using `mb`. | Delete the 3 lines. |
| D-7 | `user/sys/bin/nxproc.h` (`nxproc_render_if_changed_inline`, ~lines 361-382) | **W2** | Defined but not called by either of its two consumers (`nxproc.c`, `nxtop.c`); potentially-dead exported helper in an otherwise fully-used header. | Grep the wider tree for any other consumer before deleting; else delete or mark "reserved for future use." |
| D-8 | `user/sys/bin/nxlauncher.c` filtered-app list vs `nxproc`/`nxwins`/`regedit`/`nxnotify` reachability | **W2** | `nxlauncher.c:502-503`'s `filtered_files` excludes only `init/nxntfy_srv/nxui/nxlauncher` — `nxproc`, `nxwins`, `regedit`, `nxnotify`, `nxtop`, `nxres`, `nxmemstat`, `nxperm`, `nxinfo` all surface as ordinary launcher tiles even though several are CLI-only tools whose stdout-only output is a no-op-looking UX trap when launched windowless from the launcher (see §2b). | Extend `filtered_files` to exclude CLI-only helpers, or give CLI tools a "needs terminal" flag `nxexec` can consult. |

### Stale / self-contradicting comments

| ID | file:line | Sev | Finding | Micro-fix |
|---|---|---|---|---|
| S-1 | `user/sys/bin/nxui.c:29-30,47-48` | **W3** | States nxui "is spawned by init at machine level" / "runs at PLVL_MACHINE" — **factually wrong**. `init.c:125` calls plain `spawn()`; per `level_for_path()` (`kernel/core/syscall_dispatch.c:176-180`) any `/sys/bin/*` path resolves to **PLVL_ROOT**, never PLVL_MACHINE. `init.c:119-123`'s own comment gets this right ("ROOT (not machine) keeps the dock killable + respawnable like the shell"), directly contradicting nxui.c's self-description. `docs/ASTRA.md:438` also says "ROOT service." The NOTE(GFX-NXUI-03) additionally frames the per-path preset as a *future* landing ("once... lands") when it has already shipped and is exactly what governs nxui's level today. | Change nxui.c:29-30/:47-48 to "PLVL_ROOT (per-path preset, F1)"; delete the now-resolved "once it lands" framing. |
| S-2 | `include/api/syscall_nums.h:56` | **W2** | Comment says `SYS_SET_DISPLAY_MODE` is `CAP_MACHINE`-gated; actual kernel enforcement (`kernel/core/syscall_dispatch.c:419-424`) checks `CAP_WINDOW`. | Update the comment to say `CAP_WINDOW`. |
| S-3 | `user/sys/bin/regedit.c:16,24` (USR-SEC-01) | **W2** | Claims "registry_read/write has no authentication; any process can overwrite any key" — no longer accurate for *writes*: `kernel/lib/registry.c` gates writes behind `CAP_REG_WRITE` at 4 sites, added by later commits (`b8a0275`, `d1ea6df`, etc.) that postdate regedit.c's own last edit (`74fd422`, a comment-only pass that didn't catch this). regedit.c itself only ever *reads* (`OS1_registry_get`, line 84), which is intentionally left ungated per `kernel/lib/registry.c`'s own comments — so the file's warning describes a risk it doesn't even exercise, for a mechanism (writes) that's since been hardened elsewhere. | Delete/rewrite: regedit only reads (ungated by design); the write-side gap this note describes has closed. |
| S-4 | `user/sys/bin/nxshell.c:238` vs `user/sys/bin/nxres.c:6-9` | **W2** | Shell help text: `"nxres <x> <y> - set resolution, zoom, style and theme"` — implies one `<x> <y>`-style invocation covers all four. Actual `nxres.c` CLI requires **separate subcommands**: `nxres style <name>`, `nxres zoom <pct>`, `nxres theme <name>`, `nxres <w> <h>`. | Rewrite the help line to show the real subcommand forms. |
| S-5 | `user/sys/bin/nxres.c:8,47,111` (usage strings) vs `nxres.c:18-19` (`style_names[]`) | **W1** | Usage text lists styles as `classic \| material \| glass \| minimal \| retro` (5), omitting `"nexs"` — present as `style_names[0]` and the kernel's default style (`STYLE_NEXS = 0`). | Add `nexs` to the three usage strings. |
| S-6 | `user/sys/bin/nxtop.c:13-18` | **W2** | Claims "the shell dispatches `top <pid>` by trying the explicit name first, then `nxtop`" — **no such dispatch exists**: `nxshell.c` has zero `cmd_buf[0]=='t'`/`str_eq(cmd_buf,"top")` branch (confirmed by grep), and `nxtop.c`'s own `main(void)` (line 23) takes no argv at all, so a `<pid>` argument would be silently dropped even if the claimed path existed. | Correct the comment to describe the actual (argument-less, generic-fallback-only) invocation. |
| S-7 | `user/sys/bin/nxtop.c:37-40` | **W1** | Comment: "REFRESH RATE (1Hz): block for one second"; actual call is `OS1_sleep(3000)` — 3 seconds, ~0.33 Hz, not 1 Hz. | Either change the sleep to 1000 or fix the comment to "~0.33Hz (3s)". |
| S-8 | `include/api/posix_types.h:2` | **W1** | File-header comment says `kernel/include/kernel/types.h`; actual path is `include/api/posix_types.h` — stale/copy-pasted path comment. | Fix the path in the header comment. |
| S-9 | `docs/ASTRA.md:451-454`, `docs/direction/DIR-04-capabilities-and-services.md:79` | **W2** | Both docs say "Planned, same pattern: `nxinfo`... and `nxperms`" (note: even the *name* is wrong — the real file is `nxperm`, not `nxperms`) — but `nxinfo.c/.h` and `nxperm.c/.h` are **already implemented and built** (`Makefile:325-329`). The architecture docs this audit was told to treat as ground truth are themselves stale relative to the current tree. | Update ASTRA.md §7.4 / DIR-04 to move `nxinfo`/`nxperm` from "planned" to "established (introspection-only; full vision pending)," and fix the `nxperms`→`nxperm` name. |
| S-10 | `user/sys/bin/nxntfy_srv.c:24-29` (top-of-file "5 s auto-hide") vs `nxntfy_srv.c:188-190` (`>= 2000` ms) and the module doc at line 9 ("2 seconds") | **W2** | The USR-NOTIFY-02 design-note block says "5 s auto-hide" twice; the actual code and a second, correct inline comment both say 2 seconds (`2000` ms). Self-contradiction within one file. | Fix the "5 s" occurrences in the top-of-file note to "2 s". |
| S-11 | `user/sys/bin/nxntfy_srv.c:42,69,72` | **W1** | Popup position hardcodes `720` (screen width) in a comment ("Screensize is 720 * 1280") and in the `create_window` call itself — never queries `OS1_display_info()`, unlike `nxlauncher.c:946` and `nxui.c` which both do. Mispositions the popup off-screen if resolution ≠ 720-wide (reachable via `nxres <w> <h>`). | Query `OS1_display_info()` for width instead of the literal `720`. |
| S-12 | `user/sys/bin/nxinfo.h:8,27` | **W2** | Claims nxinfo.h is "A REUSABLE helper layer that both user and system apps may link against" — in the whole repo it has exactly **one** consumer (`nxinfo.c` itself); contrast `nxproc.h`, which 3 files genuinely consume. Reuse claim is aspirational, not demonstrated. | Soften the doc claim, or fold `nxinfo.h` back into `nxinfo.c` until a second consumer exists. |
| S-13 | `user/sys/bin/init.c:22-26` (USR-INIT-02) | **W3** | Confirmed accurate, not stale, but worth restating precisely: `user/sys/bin/init.cfg` (source) lists `/notify_srv.elf` and `/nxshell` — **neither path exists** on the real rootfs (actual: `/sys/bin/nxntfy_srv`, `/sys/bin/nxshell`) — and `init.c` never reads the file at all (hardcodes both spawns, `init.c:111,152`). The file is nonetheless copied into the build at `Makefile:455` (`cp user/sys/bin/init.cfg $(BUILD_DIR)/rootfs/etc/`), so a stale, never-consumed config ships on every build. | Either wire init.c to read init.cfg with corrected paths, or delete init.cfg + its Makefile copy step to stop shipping dead config. |

### Duplicated logic

| ID | file:line | Sev | Finding | Micro-fix |
|---|---|---|---|---|
| P-1 | `user/sys/bin/nxperm.h:103-108` vs `user/sys/bin/nxinfo.h:97-102` | **W2** | Byte-for-byte-equivalent `_emit(win, s)` sink-routing helper (`win<0` → `printf`, else `printf_win`) independently defined in both headers (and the same decision is re-implemented a third way inside `nxproc.h`'s two full render functions). | Factor one shared `nx_emit(win, s)` into a common small header all four `nx*.h` helpers include. |
| P-2 | `user/sys/bin/nxui.c:61-63` vs `user/sys/bin/nxlauncher.c:58-62` | **W2** | `TILE=40`/`TILE_GAP=12`/`TILE_RADIUS=6` independently `#define`d in both files; `nxlauncher.c:58-59`'s own comment claims they "match nxui" — true today, but nothing enforces it structurally; a future edit to one desyncs the claim silently. | Hoist into a shared `dock_geometry.h` both files include. |
| P-3 | `user/sys/bin/nxmemstat.h:119-147` (`nxmemstat_render_if_changed`) vs `user/sys/bin/nxproc.h:400-425` (`nxproc_render_if_changed`) | **W2** | Near-identical snapshot→signature-compare→conditional-render control flow, independently implemented per-helper rather than factored into one generic function-pointer-parameterized helper. | Not urgent at 2 instances; extract if a 3rd/4th such helper appears. |
| P-4 | `user/sys/bin/nxperm.h:42-50` (`nxperm_level_mask`) vs `kernel/sched/process.c:599-604` (`level_ceiling[]`) | **W2** | Userland hand-copies the kernel's real capability-ceiling table rather than reading it live; the header's own comment (`nxperm.h:39-41`) admits this ("mirrored here for the userland view"). A kernel-side edit to `level_ceiling[]` silently desyncs nxperm's reporting. | Add a syscall/registry-exposed read of the live ceiling table, or at minimum a `_Static_assert`/comment cross-reference tying the two definitions together. |
| P-5 | `user/bin/counter.c`, `user/bin/crash.c` (BLOAT-01/02) | **W2** | Both files self-document the identical root cause (lib.o unconditionally drags in stb_image/stb_easy_font + unstripped DWARF for trivial binaries) as if it were a per-file issue; it's one shared root cause in `lib.c`/the link rules. | Single fix in `user/sys/lib/lib.c` (gate STB behind a feature macro) + a `--gc-sections`/strip step in the Makefile link rule, not per-file notes. |

### ASTRA / DIR-04 violations

| ID | file:line | Sev | Finding | Micro-fix |
|---|---|---|---|---|
| U-1 | `user/sys/bin/regedit.c:104-111` | **W3** | `recv(0, &msg)` is a **blocking** call (no timeout) with nothing ever sending IPC to regedit — confirmed via `os1.h:175-176` (`recv` vs. the already-existing non-blocking `try_recv`). The window hangs forever after its first frame; self-documented by the file's own comments but never fixed. | Swap `recv(0,&msg)` → `try_recv(0,&msg)` — zero new API surface needed. |
| U-2 | `user/sys/bin/nxshell.c:320-342` (USR-SEC-02) | **W3** | `kill <pid>` parses a decimal PID directly from user input and calls `kill_process(pid)` with **no capability check in the shell** — relies entirely on the kernel-side gate. Confirmed the kernel *does* gate this (`OBJ_CTL_KILL` needs `OS1_RIGHT_DESTROY`, `kernel/core/object.c:702-722`) via the older ambient `kill_process()`→`_sys_kill` path rather than the capability path, so the actual protection depends on which underlying syscall `kill_process()` resolves to — worth confirming `_sys_kill`'s own gate (out of this pass's file set) rather than assuming the shell adds anything. | Route `nxshell`'s `kill` through `OS1low_handle_create`+`OBJ_CTL_KILL` for symmetry with the capability model, or document explicitly which syscall enforces it and why the shell needs no local check. |
| U-3 | `kernel/core/syscall_dispatch.c:419-424` (`SYS_SET_DISPLAY_MODE`), `:480-496` (`SET_STYLE`), `:497-506` (`SET_ZOOM`), consumed by `user/sys/bin/nxres.c` | **W3** | All three are capability-checked (`CAP_WINDOW`) rather than ambient-trusted in the literal sense, but `CAP_WINDOW` is in **every** privilege level's default ceiling including `PLVL_GUEST` (`kernel/sched/process.c:599-604`, `level_ceiling[PLVL_GUEST] = CAP_WINDOW`) — so the gate restricts nothing in practice: any process at any level, including the lowest-trust guest, can change the machine's global display resolution/style/theme/zoom. The dispatcher's own comment calls this "a system-level action" needing `CAP_WINDOW` as if that were meaningfully restrictive. | Gate on a level check (`PLVL_ROOT` or stronger) or a dedicated `CAP_DISPLAY` bit not in the guest ceiling, if display changes are meant to be privileged; otherwise correct the comment to stop implying restriction. |
| U-4 | `user/sys/bin/nxntfy_srv.c:118-186` (event loop) | **W3** | No receive-side validation on `msg.from` at all — any process that can read `srv.notify_pid` (everyone; registry reads are ungated) can post unlimited `IPC_TYPE_NOTIFY` messages. `msg.from` itself cannot be spoofed (kernel sets it, `kernel_ipc_send`/`process.c`), so *attribution* in the log is honest, but *the right to post* is unchecked — the 16-slot ring log (`nxntfy_srv.c:171`, `idx = ring_idx & 0x0F`) can be trivially evicted/flooded by a hostile low-privilege process. | Add a simple per-`msg.from` rate limit (e.g. 250ms minimum between accepted notifications from the same PID) independent of the already-tracked USR-SEC-01 registry-write-auth gap. |
| U-5 | `caps.h` vocabulary vs DIR-04 §2 | **W2** | DIR-04 explicitly wants `proc_*/fs_*/window_*/input_*` **family-per-verb** capabilities (e.g. a `CAP_PROC_KILL` distinct from spawn). Actual `caps.h` (32 lines) has 5 flat, coarse bits (`CAP_SPAWN/FS_WRITE/IPC_ANY/WINDOW/REG_WRITE`) — no `CAP_PROC_KILL` (kill is gated purely via the newer `OS1_RIGHT_DESTROY` object right, a parallel and not-yet-unified mechanism), no `CAP_WINDOW_CREATE`/`_DRAW` split, no `CAP_NOTIFY`/`CAP_NET` despite DIR-04's own example list naming both. DIR-04's own "Status" section already admits this is "Remaining," so this finding is a confirmation, not new information — but it directly explains U-3 (a single coarse `CAP_WINDOW` bit covering both "make a window" and "change the whole display mode" is why the latter ends up ambient-by-default). | Tracked already in DIR-04; no new action beyond what the doc itself schedules. |
| U-6 | `user/sys/bin/nxui.c:240-262` | **W1** | The dock pins the nxlauncher tile to position 0 by matching `wi[j].title` against the literal string `"nxlauncher"` (`strncmp(...,"nxlauncher",10)`) — a hardcoded, string-matched special case in an otherwise-generic dock, rather than a flag/capability the compositor or launcher itself asserts. Self-aware and commented, not accidental, but a DIR-04-adjacent "ambient identity by title string" pattern worth flagging. | If more special-cased tiles are ever added, replace the title-string match with a `WININFO_*` bit or a dedicated `OBJ_CTL` query the launcher sets on its own window. |

---

## 4. Proposed services-vs-apps division

Grounded in exactly what `init.c` v0.0.4.4 does today
(`user/sys/bin/init.c:101-263`): one undivided `main()` that (a) spawns
`nxntfy_srv`, `nxui`, `nxlauncher`, `nxshell` in a fixed hardcoded order
(`init.c:111,125,142,152`), (b) owns and re-publishes `srv.notify_pid` on
every (re)spawn (`register_notify_pid`, `init.c:78-85`), and (c) runs a single
`while(1)` supervisor loop (`init.c:195-260`) polling `wait()` on all four
PIDs every 50ms and respawning on death — with the shell's respawn branch
compiled out whenever `LAUNCHER_AUTOSTART` is on (§2c).

### U1 — `system_init` (proposed)

Takes over exactly the low-level-service half of what `init.c` does today:

- **Spawns, at ROOT + explicit per-service caps** (not the current blanket
  per-path ROOT preset): `nxntfy_srv` only, to start. `nxui` and
  `nxlauncher` are borderline — nxui is squarely SRL (§1's SERVICE row) and
  arguably belongs here too, while nxlauncher is a system app that happens to
  need early autostart; a clean split puts nxui in U1 and nxlauncher in U2.
- **Owns `srv.notify_pid` registry publication** — this logic
  (`register_notify_pid`, `init.c:78-85`) is inherently a *service
  bootstrap* concern (discovery-endpoint registration), not a UI-launch
  concern, and belongs in U1 regardless of where the final U1/U2 line falls
  for nxui itself.
- **Does not touch `nxshell`/`nxlauncher`** — those are U2's job.

### U2 — `nxinit` (proposed)

Takes over the system-app-launch half:

- **Spawns**: `nxlauncher`, `nxshell` (and `nxui` if the U1/U2 split above
  puts the dock here instead — a judgment call, since nxui is
  service-shaped but is *also* the first thing a user visually interacts
  with, unlike nxntfy_srv which is invisible until triggered).
- **Reads `LAUNCHER_AUTOSTART`-equivalent config** from a real file (fixing
  finding S-13/USR-INIT-02: `init.cfg` should live here, be actually read,
  and have its paths corrected) rather than a compile-time `#define`
  (`init.c:58`) that silently changes supervisor behavior (§2c's shell-respawn
  coupling) as a side effect.

### `nxrespawn` (planned) — where it fits

Currently, respawn logic is inlined in `init.c`'s `while(1)` loop
(`init.c:195-260`): four near-identical `wait(pid)` → `if (r==pid || r==-2)`
→ `spawn(path)` blocks, differing only in which PID/path and whether the
branch is `#if`-gated. This is **exactly** the shape a dedicated
`nxrespawn` service should absorb:

- **Rate-limiting** (USR-INIT-03, `init.c:27-30`, confirmed no backoff exists
  today: "a service that crashes immediately will be respawned in a tight
  loop, saturating the process table") is the concrete gap `nxrespawn` closes
  — a per-PID/per-path respawn-attempt counter with exponential or fixed
  backoff, something the current 50ms-flat-interval loop cannot express
  without duplicating logic four times.
  Both `system_init` and `nxinit` would register their spawned services'
  (pid, path) pairs with `nxrespawn` instead of each running their own copy
  of the poll-and-respawn loop — collapsing today's one hardcoded loop (and
  tomorrow's two, once split) into one shared, rate-limited engine.
- The `register_notify_pid`-style "re-publish a live endpoint on every
  respawn" pattern (`init.c:61-77`) generalizes naturally: `nxrespawn` could
  own a generic "on respawn, re-publish PID to registry key K" table entry
  per supervised service, rather than notify being the one special-cased
  service with this logic hand-written in `init.c`.

### `nxexec` (planned) — where it fits

Per the maintainer's split (standalone vs needs-shell), `nxexec` is the
natural home for the two divergent spawn/launch models §2 documents:

- **`nxshell`'s model** (`run_foreground` + `window_of_pid` debounce +
  Ctrl-C capture, `nxshell.c:200-217`) is a *terminal-attached* launch —
  `nxexec` should expose this as a reusable primitive (e.g.
  `nxexec_run_foreground(path, argv)`) so any future terminal-like consumer
  doesn't reimplement the poll loop.
- **`nxlauncher`'s model** (bare `spawn_args` + immediate self-minimize,
  `nxlauncher.c:896,904`, §2b) is a *detached* launch that currently
  accidentally inherits ctty semantics it never intended (the core bug this
  audit pins down). `nxexec` is the right layer to introduce the missing
  distinction: a `standalone` launch mode that explicitly does **not**
  become the child's ctty (requires a kernel-side opt-out of the
  unconditional `ctty_win` inheritance at `kernel/sched/process.c:741-746`,
  since today nothing in the spawn ABI lets a caller decline being ctty for
  its child) versus a `needs-shell` launch mode that behaves like
  `run_foreground` and accepts ctty responsibility deliberately. Until that
  kernel-side flag exists, the safest userland-only mitigation is finding
  D-8 (exclude CLI-only tools from the launcher's generic tile list) plus
  giving `nxlauncher` its own `run_foreground`-equivalent for the subset of
  tools it still allows through.

---

## Scope note

`fontman/fontman.c` and `nexs-fm/` (both built into `/sys/bin` per
`Makefile:326-327`) live in subdirectories of `user/sys/bin/`, not at its top
level, so per the task's literal file-glob (`user/sys/bin/*.c|*.h`) they were
**not** read this pass. They are named in §1's classification table only so
the table isn't silently incomplete; a follow-up pass should cover them
before the services-vs-apps split in §4 is finalized, since both are
`/sys/bin` binaries with unclear SERVICE/SYSTEM-APP status under the same
ASTRA §6.4 question this audit answers for everything else.
