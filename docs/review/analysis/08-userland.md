> STATUS: agent-generated, **maintainer spot-checked & corrected** (2026-06-02).
> Corrections applied: USR-INIT-01 downgraded **W3→W1** — the PID-reuse hazard is **not
> live** (`next_pid` is a monotonic counter, never reused: `kernel/sched/process.c:20,233`).
> USR-MALLOC-01 mechanism corrected (the `memset` uses the wrapped-small `total`; the
> overflow is the *caller* writing past the undersized buffer). USR-FONTMAN-01,
> USR-BLOAT-01/02, USR-MALLOC-05 independently verified against source — confirmed.

# Subsystem Analysis 08 — Userland (init, shell, services, lib, apps)

> Severity/kind tags are defined in [`../TAXONOMY.md`](../TAXONOMY.md).
> Evidence basis: **[verified]** confirmed by artifact inspection; **[static]** read-only source analysis; **[inferred]** reasoned conclusion with stated assumption.

| | |
|---|---|
| **Subsystem** | Userland: init, services, user library, apps |
| **Sources** | `user/sys/bin/init.c` (56), `user/sys/bin/nxshell.c` (263), `user/sys/bin/proce.c/.h`, `user/sys/bin/notification_server.c` (68), `user/sys/bin/regedit.c` (68), `user/sys/bin/nxfont/nxfont.c` (169), `user/sys/lib/lib.c` (413), `user/sys/lib/lib.h`, `user/sys/lib/malloc.c` (114), `user/sys/lib/font_lib.c` (115), `user/sys/lib/syscall.S` (207), `user/arch/aarch64/syscall.S` (231), `user/arch/amd64/syscall.S` (207), `user/bin/counter.c` (48), `user/bin/crash.c` (9), `user/bin/demo3d.c` (308), `user/bin/ipc_send.c` (45), `user/bin/ipc_recv.c` (33), `user/bin/input_test.c` (59), `user/bin/writetest.c` (38), `user/bin/test_init.c` (10) |
| **Headers** | `include/api/os1.h` (179), `user/sys/lib/lib.h` |
| **Build** | `Makefile` (CFLAGS: `-O2 -g -fno-omit-frame-pointer`, no `--gc-sections`, links `USER_LIB_O` into every ELF) |
| **Build artifacts** | **[verified]** `build/aarch64/counter.elf` = 503,712 bytes; `build/aarch64/crash.elf` = 502,432 bytes; `build/aarch64/init.elf` = 504,192 bytes. |

---

## 1. Purpose & Role

The userland consists of three tiers:

1. **Process 1 (init)** — spawns system services, runs a supervisor loop watching for child exits and respawning them.
2. **System services** — `shell` (interactive TTY), `notify_srv` (IPC-based notification popup), `regedit` (registry control panel), `nxfont` (TTF rasterizer / font uploader), `proce` (process list helper compiled into shell).
3. **User library (`lib.c`, `malloc.c`, `font_lib.c`, `syscall.S`)** — the entire C runtime, formatting layer, STB stacks, and arch-specific syscall stubs are compiled into a single `lib.o` + `malloc.o` pair linked into every ELF. There is no shared-library mechanism.

---

## 2. Data Flow

```
Kernel ELF loader
       │
       ▼
 _start (arch/*/syscall.S)
       │  bl main / call main
       ▼
  init main (init.c)
       │  spawn()
       ├──▶ notify_srv ──▶ registry_write("srv.notify_pid", ...)
       └──▶ shell ──────▶ reads keyboard FD 0; calls spawn/kill/ls/cat...
                               │ notify()
                               └──▶ send(notify_pid, msg) ──▶ notify_srv recv loop

  Every process ──▶ lib.o (lib.c + malloc.c + font_lib.c + stb_*.h)
                          │ _sys_*() wrappers
                          └──▶ arch/*/syscall.S ──▶ svc/syscall trap
```

---

## 3. What Works

- **[static]** `malloc.c` implements first-fit splitting and forward coalescing — a genuine improvement over the kernel's bump-only `kmalloc`. The allocator handles the common DOOM/font workload correctly for sequential allocate-free patterns.
- **[static]** `notify_srv` uses `try_recv` (non-blocking) + `yield()` (`notification_server.c:41,64`), producing a correct event loop that does not busy-spin. The auto-hide timer logic (lines 57–60) is clean.
- **[static]** `demo3d.c` is a competent fixed-point software rasterizer with backface culling, flat shading, and a correct scan-line fill (lines 110–200). Fixed-point math is used throughout the kernel lib; userland reuses it correctly.
- **[static]** Syscall numbers are consistent across `user/arch/aarch64/syscall.S`, `user/arch/amd64/syscall.S`, and `include/api/os1.h`. Both arch wrappers handle the 4th-argument `rcx→r10` rewrite on amd64 (`amd64/syscall.S:79,91,98,...`).
- **[static]** `init.c`'s supervisor loop is a non-blocking poll, not a blocking sleep — `process_wait()` returns -1 when the child is alive and only the matching PID on death/zombie reap (`kernel/sched/process.c:710-741`). The structure is correct in intent.
- **[static]** `font_lib.c` correctly walks UTF-8 via `utf8_decode` and draws per-glyph with advance tracking, giving a functional text renderer when a font is loaded.

---

## 4. Central Invariants (the things to fix first)

**A. No isolation exists at any layer.** Every userland process runs with full ambient authority: arbitrary IPC to any PID, read/write access to the global registry with no authentication, the ability to spawn arbitrary paths, and the ability to kill any process by PID. The ABI has no capability structure. This is the baseline state that must change before any service can be called "sandboxed."

**B. The entire library is monolithically linked into every ELF.** `lib.o` contains `STB_IMAGE` (full JPEG/PNG/GIF/BMP decoder) and `STB_EASY_FONT` compiled in, with no `--gc-sections` to remove unused code. A 9-line `crash.c` becomes a 502KB ELF because it carries the entire image decoder stack. This blocks the project's "system services as small isolated components" goal.

---

## 5. Findings

| ID | Sev | Kind | Location | Summary |
|----|-----|------|----------|---------|
| USR-INIT-01 | W1 | REFINE | `init.c:39–53` | Supervisor loop is a **correct** non-blocking poll today. *Maintainer correction:* the PID-reuse hazard the draft flagged is **not live** — `next_pid` is monotonic (`process.c:20,233`), PIDs are never reused, and init is single-threaded with cooperative `yield`. Residual (defensive): the loop assumes monotonic PIDs and would need a generation/owner check if PID recycling is ever introduced. |
| USR-INIT-02 | W3 | MISSING · DOC | `init.c:5–6`; `init.cfg:6,9` | `init.cfg` is never read despite file_read existing; the cfg paths (`/notify_srv.elf`, `/nxshell`) don't match actual rootfs layout (`/sys/bin/notify_srv`, `/sys/bin/nxshell`). Dead config file, ignored by code. |
| USR-INIT-03 | W2 | BAD-IMPL | `init.c:39–53` | No mechanism to limit respawn rate: a crashing service respawns immediately and unconditionally; a tight crash-respawn loop will saturate the process table (`MAX_PROCESSES = 64`, `os1.h:16`) with zombies. |
| USR-SEC-01 | W3 | SECURITY | `lib.c:64–65`, `notification_server.c:33`, `lib.c:115` | Global registry has no caller authentication. `notify_srv` writes its PID to `srv.notify_pid`; `notify()` reads that key to route messages. Any process can overwrite `srv.notify_pid` and intercept or forge system notifications. |
| USR-SEC-02 | W3 | SECURITY | `lib.c:47–48`, `shell.c:135–149` | `send()` and `kill_process()` accept arbitrary PIDs with no capability check. Shell parses a decimal argument directly from user input and passes it to `kill_process` (shell.c:139–148), allowing any user to kill any system service. |
| USR-SEC-03 | W3 | WRONG-DESIGN | All services | No sandboxing exists. Services run with flat memory, unrestricted IPC, and full spawn authority. There are no seL4-style capability tokens, no namespace isolation, and no resource quotas. Assessment: zero isolation. |
| USR-MALLOC-01 | W3 | SECURITY | `malloc.c:106-108` | `calloc(nmemb, size)` computes `total = nmemb * size` with **no overflow check** → undersized allocation. *Maintainer correction of mechanism:* the `memset` uses the same wrapped-small `total` (so the memset itself is in-bounds); the corruption is the **caller** — expecting `nmemb*size` bytes — overflowing the short buffer. Classic calloc-overflow → heap corruption. |
| USR-MALLOC-02 | W2 | BAD-IMPL | `malloc.c:77–80` | Forward coalescing assumes `block->next` is physically contiguous with current block. This is only true if `next` was split from current; it is false when `next` was a separate `sbrk` call. Corrupts the free list silently. |
| USR-MALLOC-03 | W2 | WRONG-DESIGN | `malloc.c:20–67` | No backward coalescing (acknowledged in comment at line 82–83). An `alloc/free/alloc/free` sequence with alternating sizes fragments the heap permanently; freed blocks before the current one are never merged. |
| USR-MALLOC-04 | W2 | WRONG-DESIGN | `malloc.c:54–67` | Heap never shrinks: sbrk'd pages are never returned to the kernel even when fully free. Long-running services will grow indefinitely. |
| USR-MALLOC-05 | W2 | BAD-IMPL | `malloc.c:27–28` | Comment claims "16-byte alignment for performance/compatibility." `size` is rounded to 16B, but the `block_header_t` struct is 24 bytes on LP64 (8+4+4\_pad+8), so the returned payload address is at offset +24 — 8-byte aligned, not 16. Callers expecting 16-byte alignment (SIMD, DMA) get silent misalignment. |
| USR-MALLOC-06 | W1 | BAD-IMPL | `malloc.c:94` | `realloc` compares `block->size >= size` using the rounded allocated size (may be slightly larger than original request). This is safe but the comment (line 98–100) incorrectly describes it as "slightly larger than original request" — it can be much larger after a split. |
| USR-BLOAT-01 | W2 | BAD-IMPL · PERF | `lib.c:13–25`; `Makefile:300–311` | `STB_EASY_FONT_IMPLEMENTATION` and `STB_IMAGE_IMPLEMENTATION` are compiled unconditionally into `lib.o`. Every ELF links `lib.o`. With no `--gc-sections`, unused decoders are retained. **[verified]** `counter.elf`: 66KB `.text` (52KB is stb\_image/stb\_easy\_font), 354KB `.debug_*`, total 503KB. |
| USR-BLOAT-02 | W2 | BAD-IMPL | `Makefile:29` | `-g` (full DWARF) and `-fno-omit-frame-pointer` with no `-s` (strip) and no `--gc-sections`. **[verified]** 354KB of the 503KB `counter.elf` is `.debug_*` DWARF data; actual code+data is 171KB; stripped binary would be ~147KB. |
| USR-ABI-01 | W2 | WRONG-DESIGN | `user/sys/lib/syscall.S` vs `user/arch/aarch64/syscall.S` | Two aarch64 syscall files exist. Makefile builds only `user/arch/$(ARCH)/syscall.o` (`Makefile:83,265`); `user/sys/lib/syscall.S` is dead code. The dead copy is also incomplete: missing `_sys_set_font`, `_sys_list_dir`, `_sys_chdir`, `_sys_getcwd` that the arch copy has. Proved divergence. |
| USR-ABI-02 | W2 | WRONG-DESIGN | `include/api/os1.h:92–137` | The public ABI mixes kernel-internal types (`ipc_message`, `input_event_t`, `FILE`) with system call wrappers in a single flat header. There is no separation between ABI-stable syscall surface and library convenience layer; any ABI change forces recompile of every binary. |
| USR-SHELL-01 | W2 | BAD-IMPL | `shell.c:111–113` | `ls [path]` parsing uses hardcoded character offsets (`cmd_buf[2] == ' '`, `&cmd_buf[3]`) instead of token splitting, consistent with 13 other command parsers in the same function. Command dispatch is a 200-line if-else chain with no extensibility. |
| USR-SHELL-02 | W2 | MISSING | `shell.c:21` | Command history buffer is 128 characters (`cmd_buf[128]`), single-line only. No command history, no tab completion, no argument splitting. The shell is a line editor, not a programmable interpreter. |
| USR-REGEDIT-01 | W3 | BUG · STUB | `regedit.c:58–61` | `recv(0, &msg)` is called in the main loop of regedit without the non-blocking flag; `recv()` is blocking (it does `_sys_recv` with no timeout). The comment on line 59 admits the problem: "Non-blocking check? No, recv blocks. We need an event loop." The window will freeze waiting for IPC. |
| USR-FONTMAN-01 | W3 | MISSING | `nxfont.c:165–166` | nxfont keeps the raw TTF buffer alive with `while(1) yield()` because "the kernel just points to it (for now, as per sys_set_font hack)". The kernel holds a raw pointer into userland heap memory. If nxfont exits, the kernel dereferences a freed buffer — use-after-free in kernel space. |
| USR-FONTMAN-02 | W2 | BAD-IMPL | `nxfont.c:6,6` (duplicate) | `#include <font.h>` appears twice at lines 5 and 6. Harmless with include guards but indicates copy-paste sloppiness. |
| USR-FONTMAN-03 | W2 | BAD-IMPL | `nxfont.c:56–57` | `acos()` is approximated as `(1.0 - x) * (π/2)` — "Very rough approximation for font rasterization" per its own comment. The correct identity for acos(cos(θ)) ≠ this formula for θ outside [0,π/2]. stb\_truetype uses acos in curve subdivision; severe approximation error will produce malformed glyph outlines. |
| USR-LIB-01 | W2 | BAD-IMPL | `lib.c:54–57` | lib.c `#include`s `kernel/lib/vsnprintf.c`, `kernel/lib/math.c`, `kernel/lib/string.c`, and `font_lib.c` directly via path. Breaks the userland/kernel separation boundary; any change to kernel lib internals silently affects userland. |
| USR-LIB-02 | W2 | BAD-IMPL | `lib.c:140–141` | `fclose()` checks `(size_t)fp > 10` to guard against NULL-like sentinel values. This is a fragile magic-value check — a real NULL should be guarded with `fp != NULL`. |
| USR-LIB-03 | W1 | BAD-IMPL | `lib.c:234` | `graphics_draw_text` declares `static char buffer[99999]` — a 100KB static buffer. This bloats `.bss` across every binary that calls it and is retained even when text rendering is not used (no gc-sections). |
| USR-LIB-04 | W1 | STUB | `lib.c:363–365` | `mkdir`, `system`, and `getenv` are stubs that return 0/NULL silently. `atof` is implemented as `(double)atoi(nptr)` — truncates decimal fractions. These create silent wrong-behaviour for any caller expecting correct semantics. |
| USR-LIB-05 | W1 | DOC | `lib.c:380–381` | `vfprintf` ignores the `stream` argument and always writes to fd 1 (UART/stdout). Callers writing to `stderr` silently get stdout. |

---

## 6. Detailed Entries

### USR-INIT-01 — init supervisor loop (maintainer-corrected: hazards NOT live) `[static]`

`init.c:39–53`:
```
while (1) {
    if (wait(pid_shell) == pid_shell) {   // L41
        pid_shell = spawn("/sys/bin/nxshell"); // L43
    }
    if (wait(pid_notify) == pid_notify) { // L47
        pid_notify = spawn("/sys/bin/notify_srv"); // L49
    }
    yield(); // L52
}
```

The kernel's `process_wait()` (`kernel/sched/process.c:710–741`) is **non-blocking**: it returns -1 if the named process is alive, the PID if it is a zombie/dead (and reaps it), or -2 if not found. The loop is therefore a correct poll — it does not block. The owner's concern about "sequential blocking wait" is partially denied: there is no blocking stall.

**Maintainer correction (2026-06-02): the two hazards below are NOT live.** `next_pid` is a
monotonic counter that never resets or reuses freed PIDs (`kernel/sched/process.c:20` and
`:233` `proc->pid = next_pid++`), and `process_pool` is indexed by *slot*, not PID. So a
respawned shell can never collide with `pid_notify`, and (init being single-threaded with
cooperative `yield`) there is no recycle to race. **USR-INIT-01 is therefore downgraded to
W1/REFINE.** The genuinely actionable init issues are USR-INIT-02 (`init.cfg` ignored) and
USR-INIT-03 (no respawn backoff). The draft's original hypothetical analysis is retained
below for context only:

1. **PID reuse hazard.** After `pid_shell` dies and is reaped at L41, `spawn()` assigns a new PID to `pid_shell` at L43. If the kernel's PID allocator reuses a recently freed PID (MAX_PROCESSES is 64; `kernel/sched/process.c:process_pool` is an array indexed by PID), the new shell PID could coincide with `pid_notify`. On the very next L47 check, `wait(pid_notify)` would reap the freshly spawned shell, not the notification server. The result is silent process table corruption.

2. **No inter-iteration atomicity.** Between L41 and L47 there is no locking; if the scheduler runs between them and a process dies-and-PID-recycles, the second `wait()` may reap the wrong process. Under a single-core scheduler with cooperative yield this is unlikely, but it is structurally unsound.

**Fix direction:** Store each service's PID in a table with a generation counter; use a sentinel `pid == -1` after reap; introduce an exponential backoff before respawn; or switch to a `wait(-1)` (wait-any) model with a lookup table to identify which service died.

---

### USR-INIT-02 — init.cfg is dead code with wrong paths `[static]`

`init.c:4–6` (comment):
> "TODO: Read /init.cfg when file syscalls are implemented. For now, uses hardcoded spawn list."

`file_read` (`lib.c:72`, syscall 252) is fully implemented and used by shell's `cat` command (`shell.c:172`). The TODO is stale.

The shipped `init.cfg` (`init.cfg:6,9`) lists:
```
/notify_srv.elf
/nxshell
```

These paths are wrong for the current rootfs layout. `Makefile:363` strips `.elf` extensions; `Makefile:349` places system binaries in `/sys/bin/`. The working paths are `/sys/bin/notify_srv` and `/sys/bin/nxshell`. `init.cfg` is copied to `/etc/` in the rootfs (`Makefile:351`) but is never opened by init.

---

### USR-SEC-03 — No isolation: full ambient authority for all processes `[static]`

No sandboxing primitive exists anywhere in the userland ABI. Concrete evidence:

- **Registry:** `registry_write(key, val)` (`lib.c:65`) dispatches `SYS_REGISTRY` with op=1 and no caller authentication. Any process may overwrite any key. `notify_srv` stores its PID in `srv.notify_pid` (`notification_server.c:33`); `notify()` (`lib.c:115`) reads that key to find the notification endpoint. Any process can redirect all system notifications to itself by writing `srv.notify_pid`.

- **IPC:** `send(pid, msg)` (`lib.c:47`) delivers to any PID. There are no capability tokens, no send rights, and no receive filters. A process can spoof messages to any service.

- **Spawn:** `spawn(path)` (`lib.c:35`) launches any ELF from the filesystem. Shell grants this to any typed command (`shell.c:184–198`).

- **Kill:** `kill_process(pid)` (`lib.c:36`) terminates any process. Shell exposes this directly via the `kill <pid>` command (`shell.c:135–149`).

The project goal is seL4-style capability isolation. Today's state is the opposite: capability is implicit and universal. Every service is equivalently privileged.

---

### USR-MALLOC-01 — calloc integer overflow → heap corruption `[static]`

`malloc.c:107–113`:
```c
void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;   // ← no overflow check
    void *ptr = malloc(total);
    if (ptr) memset(ptr, 0, total); // ← memset uses the overflowed total
    return ptr;
}
```

If `nmemb * size` overflows (e.g., `nmemb = 0x80000001`, `size = 2` on 32-bit; or realistic large values on 64-bit), `total` wraps to a small value. `malloc(small)` succeeds; `memset(ptr, 0, actual_large_value)` writes beyond the allocated block, corrupting the heap. Fontman calls `realloc(bitmap, bitmap_capacity)` (`nxfont.c:115`) with a doubling strategy; a large font could trigger this path via `calloc` in stb\_truetype.

**Fix:** `if (nmemb && size > SIZE_MAX / nmemb) return NULL;` before the multiply.

---

### USR-BLOAT-01/02 — ~500KB binary size: root causes `[verified]`

**Artifact:** `build/aarch64/counter.elf` = 503,712 bytes (owner-stated ~500KB). The 9-line `crash.c` compiles to 502,432 bytes. Section breakdown (verified via `aarch64-none-elf-size`):

| Section | Size |
|---------|------|
| `.text` | 66,268 B |
| `.rodata` | 3,896 B |
| `.data` | 512 B |
| `.bss` | 100,032 B |
| `.debug_*` (7 sections) | **353,665 B** |
| **Total ELF** | **524,630 B** (file on disk: 503,712 B) |

**Root cause 1 (dominant): `-g` debug DWARF, never stripped.**
`Makefile:29`: `COMMON_FLAGS` includes `-g` and `-fno-omit-frame-pointer`. The link rule (`Makefile:320`) passes no `-s` (strip) and no `--strip-debug`. DWARF accounts for 354KB of the 503KB ELF — **70% of total size**.

**Root cause 2: STB libraries compiled into lib.o, linked into every ELF, no gc-sections.**
`lib.c:13–24` compiles `STB_EASY_FONT_IMPLEMENTATION` and `STB_IMAGE_IMPLEMENTATION` unconditionally. `Makefile:300–311` links `lib.o` into every ELF. `Makefile:23–29` has no `-ffunction-sections -fdata-sections`; the link rule has no `--gc-sections`. **Verified:** 52KB of the 66KB `.text` in `counter.elf` consists of stb\_image and stb\_easy\_font symbols (115 stb symbols confirmed by `nm`). A `counter` binary that only draws rectangles carries a full JPEG/PNG/GIF/BMP decoder.

**Root cause 3 (minor): 100KB `.bss` per binary.**
`graphics_draw_text` in `lib.c:234` declares `static char buffer[99999]`, adding ~100KB `.bss` to every linked binary regardless of whether text rendering is used.

A stripped binary (`strip --strip-debug`) would be ~147KB. With `--gc-sections` and splitting STB into a separate lib or behind a header guard, typical service binaries should reach <50KB stripped.

---

## 7. Refactor Direction

| Goal (yours) | Userland implication |
|---|---|
| **seL4-style sandboxed services** | Introduce a capability token in every IPC message and registry call; add per-service namespaces; remove `kill_process` from the public shell ABI; assign minimal authority at spawn time (USR-SEC-01/02/03). |
| **Plan 9 "everything is a file"** | Replace the bespoke registry, notify, and window management syscalls with a `/dev` or `9P`-style namespace: services expose named channels; clients open file descriptors to them; access control is on the namespace, not the caller. The current ABI has 30+ one-off syscalls that should collapse into open/read/write/ioctl on named objects. |
| **Coherent ABI** | Split `include/api/os1.h` into a stable syscall layer (numbers + structs only) and a userland library layer (convenience wrappers). Build `lib.o` as a proper archive with `--gc-sections`; move STB includes behind compile-time guards or into separate optional objects. Retire the dead `user/sys/lib/syscall.S` (USR-ABI-01). |
| **Small isolated service binaries** | Require `-ffunction-sections -fdata-sections --gc-sections` in user ELF link flags; strip debug in release builds (`--strip-debug`; keep symbols in a `.dwp` side file); split STB into separate compilation units or use a host-side tool to avoid bundling image decoders in every process. |
| **Robust init/supervision** | Adopt a generation-counter PID table, exponential backoff on respawn, rate-limiting (max N respawns per second), and implement `init.cfg` parsing now that `file_read` is available. |
| **nxfont kernel safety** | The kernel must copy the font data at `sys_set_font` time rather than keeping a raw pointer into userland heap. nxfont's infinite loop workaround can then be removed (USR-FONTMAN-01). |

**Suggested sequencing:**
1. Strip debug symbols from release ELFs; add `--gc-sections` to the user link flags (immediate size win, no API change).
2. Fix calloc overflow (USR-MALLOC-01); verify malloc coalesce assumption (USR-MALLOC-02).
3. Implement `init.cfg` parsing and PID-reuse-safe supervisor loop.
4. Copy font data in kernel at `sys_set_font`; remove nxfont's infinite loop.
5. Split the ABI header; retire dead `user/sys/lib/syscall.S`.
6. Design the capability/namespace layer; migrate registry + IPC + windows onto named-object VFS.
7. Rebuild services as minimal sandboxed ELFs with declared-authority capability sets.

---

## 8. Verification Notes

- All findings above are **[static]** unless marked **[verified]**.
- **[verified]** Binary sizes confirmed from `build/aarch64/` artifacts (June 2026 build): counter=503KB, crash=502KB, init=504KB, nxfont=773KB (includes stb\_truetype).
- **[verified]** Section and symbol breakdown obtained via `aarch64-none-elf-size -A` and `aarch64-none-elf-nm --size-sort`; stb\_image+stb\_easy\_font total confirmed as 52KB of 66KB `.text` in `counter.elf`.
- **[verified]** `process_wait()` semantics confirmed by reading `kernel/sched/process.c:710–741`; function is non-blocking (returns -1 if alive).
- **[inferred — assumes LP64 struct layout]** `block_header_t` alignment analysis (USR-MALLOC-05) assumes `sizeof(size_t)=8, sizeof(int)=4, sizeof(ptr)=8` on aarch64.
- **[inferred]** `acos()` approximation error in nxfont (USR-FONTMAN-03) depends on stb\_truetype's use patterns; full error magnitude not quantified without a test case.
