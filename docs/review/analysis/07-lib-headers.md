> STATUS: agent-generated, **maintainer spot-checked & corrected** (2026-06-02).
> Correction: LIB-MATH-01 downgraded **W3→W2 (latent)** — the buggy `FP_PI` does corrupt the
> compiled `sin_fp`/`cos_fp`, but its only consumer (`kernel/graphics/draw3d.c`) is **not in
> the Makefile build**; no compiled kernel code calls `sin_fp/cos_fp`, so there is no live
> runtime impact today. LIB-KTEST-01, ABI-HDR-01, LIB-REG-02 verified against source — confirmed.

# Subsystem Analysis 07 — Kernel Library (`kernel/lib/`) + Public API Headers (`include/api/`)

> Severity/kind tags are defined in [`../TAXONOMY.md`](../TAXONOMY.md).
> Evidence basis: **[static]** read-only source; **[inferred]** reasoned from source, assumption stated inline.
> No build or runtime execution was performed; all findings are [static] or [inferred].

| | |
|---|---|
| **Subsystem** | Kernel Library + Public ABI Headers |
| **Library sources** | `kernel/lib/string.c` (332), `kernel/lib/crc32.c` (18), `kernel/lib/vsnprintf.c` (193), `kernel/lib/printk.c` (129), `kernel/lib/stack_protector.c` (19), `kernel/lib/math.c` (175), `kernel/lib/registry.c` (130), `kernel/lib/ktest.c` (31), `kernel/lib/ktest_samples.c` (17), `kernel/lib/utf8.c` (31), `kernel/lib/fdt.c` (213) |
| **Library headers** | `kernel/include/kernel/{string,printk,math,registry,fdt,test}.h` |
| **Public ABI headers** | `include/api/{os1,errno,fcntl,unistd,stdio,stdlib,string,strings,ctype,assert,inttypes,posix_types,stdbool,math,font,font_lib,graphics,input,elf}.h`, `include/api/sys/{stat,types}.h` |
| **Kernel headers overview** | `kernel/include/kernel/*`, `kernel/include/arch/*`, `kernel/include/drivers/*`, `kernel/include/graphics/*` |
| **Third-party (excluded)** | `include/api/stb_image.h`, `include/api/stb_easy_font.h` — vendored, not analyzed per scope rules |

---

## 1. Purpose & Role

This subsystem covers two distinct layers:

1. **Kernel library** (`kernel/lib/`) — the in-kernel support functions: string primitives, formatted output (`vsnprintf`/`printk`), stack-smash protection, fixed-point math, a flat key-value registry, a device-tree parser, a lightweight unit-test runner, a CRC-32, and a UTF-8 decoder. These underpin virtually all other kernel subsystems.

2. **Public ABI headers** (`include/api/`) — the userland-facing contract. These declare syscall numbers, POSIX-like types, C standard-library wrappers, and OS-specific APIs (windowing, IPC, registry, graphics). They are the only interface between userland programs and the kernel.

---

## 2. Data Flow

```
userland program
      │
      │  #include <os1.h> / <stdio.h> / <string.h> ...
      │
user/sys/lib/lib.c ──▶ _sys_* wrappers ──▶ syscall instruction
                                                    │
                        kernel/core/syscall_dispatch.c  ◀──────┘
                                    │
             ┌──────────────────────┼──────────────────────┐
        sys_registry         sys_file_*            printk / vsnprintf
        registry.c           ext4 / vfs.c          printk.c / vsnprintf.c
             │
     registry_store[128]   (flat K-V, fixed size)
```

FDT pipeline (aarch64 only):

```
QEMU DTB ──▶ kernel_main (x0_arg) ──▶ fdt_init(boot_fdt_ptr)
                                              │
                    fdt_get_mem_regions ◀──────┤──▶ fdt_count_cpus
                                              │
                    aarch64/platform.c ◀──────┘
                    (mem-regions → pmm, cpu count → SMP init)
```

---

## 3. What Works (static)

- **string.c**: Correct, safe, complete coverage of standard string/memory primitives. `strlcpy`/`strlcat` are provided (many kernels omit these). NULL-pointer checks throughout. `memmove` correctly handles both overlap directions.
- **crc32.c**: Compact, standard Sarwate table-less CRC-32. Correct polynomial (0xEDB88320). Sufficient for its uses (VirtIO/GPT checksums).
- **vsnprintf.c**: Bounded. All write loops guard `written < (int)size - 1`. No `%n` specifier. The `format(printf, ...)` attribute on `printk`/`snprintf` (`printk.h:25,27`) means the compiler catches format/argument mismatches in kernel call sites.
- **printk.c**: The recursive-printk guard is correctly ordered — IRQs are disabled and the lock taken *before* setting `in_printk` (`printk.c:59-67`), closing the SMP interleave window identified by the comment at `:55-58`. `panic` sends an IPI before printing so output is not interleaved with other CPUs (`printk.c:107-110`).
- **stack_protector.c**: `__stack_chk_fail` panics with the return address, surfacing the corruption site.
- **utf8.c**: Handles 1-, 2-, 3-, and 4-byte sequences; validates continuation bytes on all paths.
- **ktest**: The `.ktests` ELF section trick correctly auto-collects test cases; `ktest_run_all` is actually called at boot (`kernel/main.c:87`).
- **fdt.c**: Correctly implements big-endian ↔ host byte-swap for the DTB fields; the `fdt_get_mem_regions` parser handles `#address-cells` and `#size-cells` from the root node, supporting both 32- and 64-bit address/size encodings.
- **Shared type foundation**: `posix_types.h` is included by both `kernel/include/kernel/types.h` and `include/api/os1.h`, giving a single source of `pid_t`, `off_t`, `ino_t`, `ipc_message`, etc. across the kernel/userland boundary.

---

## 4. Central Issues

### 4.1 No coherent ABI contract

The public API has no versioning, no stable syscall ABI guarantee, and multiple internal contradictions (see §5 ABI findings). Syscall numbers in `os1.h` are not enforced against the dispatch table. Type declarations are scattered and sometimes duplicated verbatim. The `gets` signature is non-standard. `errno` is declared but never set by the kernel. This is the "no coherent ABI structure" the project owner described.

### 4.2 Registry is a flat store with no file semantics

`registry.c` implements a 128-slot, string-key/string-value table with a single spinlock. It bears no resemblance to a Plan 9 namespace. There is no permission check (deferred by a comment), no enumeration API, no tree structure, and no connection to `vfs.h`. See §5 detailed entry LIB-REG-01 and §6 for the Plan 9 gap analysis.

### 4.3 FDT is aarch64-only; AMD64 uses no device tree

The amd64 implementations of `fdt_init` and `fdt_find_in_memory` are explicit stubs returning `-1`/`0`. Memory detection on amd64 uses multiboot2. FDT is used productively on aarch64: it drives `pmm` memory-region discovery and SMP CPU counting. The parser is minimal — it covers exactly the two properties the system needs.

---

## 5. Findings

| ID | Sev | Kind | Location | Summary |
|----|-----|------|----------|---------|
| LIB-MATH-01 | W2 | BUG (latent) | `kernel/include/kernel/math.h:16`; `kernel/lib/math.c` | In kernel builds `math.h:16` defines `FP_PI 411775` (=2π in 16.16; mislabeled "π"), so `math.c`'s `#ifndef FP_PI` fallback (`205887`) is skipped → compiled `sin_fp`/`cos_fp` reflect about 2π and are wrong for angles in (π/2, 2π]. ***Maintainer: latent*** — the only consumer `graphics/draw3d.c` is **not compiled** (absent from Makefile); no live kernel caller exists. Fix the constant before any 3D code is built. Userland `os1.h:170` already has the correct `205887`. |
| LIB-MATH-02 | W1 | DOC | `kernel/include/kernel/math.h:16` | Comment says "3.14159 × 131072" but 131072 = 2^17, so the value is 2π × 2^16. Misleading. |
| LIB-VSNPRINTF-01 | W1 | BUG | `kernel/lib/vsnprintf.c:41,49-55` | Sign character is emitted without subtracting 1 from `width`; `%05d` of -42 formats as `"-00042"` (6 chars) instead of `"-0042"`. |
| LIB-VSNPRINTF-02 | W1 | REFINE | `kernel/lib/vsnprintf.c:73` | Returns chars written (< `size`), not the chars-needed-if-unbounded. Callers cannot detect truncation. Not POSIX-conformant. |
| LIB-VSNPRINTF-03 | W0 | MISSING | `kernel/lib/vsnprintf.c` | `%o` (octal) and `%e`/`%f` format specifiers are absent. Octal affects kernel use; float is reasonable to omit (no FPU in kernel). |
| LIB-VSNPRINTF-04 | W1 | BAD-IMPL | `kernel/lib/vsnprintf.c:176-177` | `%p` hardcodes 16-digit field width regardless of `size - written`; on a near-full buffer the width guards prevent writing `0x` but the `print_num` call still runs with a near-zero `size` argument. |
| LIB-PRINTK-01 | W2 | REFINE | `kernel/lib/printk.c:70,75` | `cpu->printk_buf` is 2048 bytes (`cpu.h:29`). Prefix is 6 bytes. `vsnprintf` receives `2048 - pfx` which for long messages silently truncates without any indication. No counter for dropped/truncated messages. |
| LIB-SSP-01 | W3 | SECURITY | `kernel/lib/stack_protector.c:9` | `__stack_chk_guard` is a compile-time constant (`0x595e9fbd94fda766`). It is never randomized at boot, negating SSP against an attacker who knows the binary. |
| LIB-MATH-03 | W1 | REFINE | `kernel/lib/math.c:123-126` | `sin_fp` range-reduces with linear subtraction of 2π; for very large inputs this is O(n) rather than a single modulo. |
| LIB-REG-01 | W3 | WRONG-DESIGN | `kernel/lib/registry.c:18-19` + `kernel/include/kernel/registry.h:6-8` | Flat fixed-size store (128 slots, 64-byte keys, 128-byte values): no tree, no enumeration, no file semantics, no permissions. See §5.1 detailed entry. |
| LIB-REG-02 | W3 | SECURITY | `kernel/lib/registry.c:10` | No permission check on registry writes. Any process can call `sys_registry(REG_OP_WRITE, "system.hostname", ...)` and overwrite system configuration. The comment defers this to "if needed later." |
| LIB-REG-03 | W1 | BAD-IMPL | `kernel/lib/registry.c:13,16` | `<kernel/vmm.h>` is included twice. |
| LIB-REG-04 | W2 | MISSING | `kernel/lib/registry.c` | No API to enumerate keys (list all entries). Userland cannot discover the registry contents without knowing key names in advance. |
| LIB-FDT-01 | W2 | REFINE | `kernel/lib/fdt.c:96-167` | No bounds-checking of offsets against `fdt_ptr->totalsize`. A malformed DTB (even from QEMU) can produce an out-of-bounds `p`/`end` pointer dereference in `fdt_get_mem_regions`. |
| LIB-FDT-02 | W1 | MISSING | `kernel/lib/fdt.c` | Memory reservation map (`off_mem_rsvmap`) is never parsed. If the firmware reserves RAM regions the kernel could overwrite them. |
| LIB-FDT-03 | W1 | STUB | `kernel/lib/fdt.c:48-52,73-77` | AMD64 implementations of `fdt_init` and `fdt_find_in_memory` return -1/0 unconditionally. This is expected behavior; the stubs are explicitly guarded by `#else`. |
| LIB-FDT-04 | W2 | SECURITY | `kernel/lib/fdt.c:55-70` | `fdt_find_in_memory` scans raw RAM byte-by-byte checking for a magic value but does **not** then validate `totalsize` or structure offsets of the candidate. A crafted RAM layout could produce a false positive pointing into arbitrary memory. |
| LIB-FDT-05 | W0 | DOC | `kernel/lib/fdt.c:31-32` | Comment "// ... just manual print for now to avoid dependency on sprintf if not ready" is stale; `printk`/`snprintf` are fully available at the point `fdt_init` is called. |
| LIB-KTEST-01 | W3 | BUG | `kernel/lib/ktest.c:23-26,29` | If a test function panics or returns early via `KASSERT`, `ktest_run_all` still increments `passed`. The final summary always prints N PASSED / 0 FAILED regardless of actual pass/fail count. |
| LIB-UTF8-01 | W2 | SECURITY | `kernel/lib/utf8.c:17-29` | Continuation bytes in 2-, 3-, and 4-byte sequences are validated for the `0x80` pattern, but the function reads `s[1]`, `s[2]`, `s[3]` without knowing the string length. A non-NUL-terminated input or a partial sequence at a buffer boundary will read past the end. |
| LIB-UTF8-02 | W1 | MISSING | `kernel/lib/utf8.c` | No overlong encoding rejection (e.g. U+0000 encoded as 2-byte `0xC0 0x80`), no surrogate-pair rejection, no > U+10FFFF check. |
| ABI-SYS-01 | W3 | WRONG-DESIGN | `include/api/os1.h:19-46` vs `kernel/core/syscall_dispatch.c` | Syscall numbers in `os1.h` are not enforced against the dispatch table by any shared header or `_Static_assert`. A number change in one location silently breaks the ABI. |
| ABI-SYS-02 | W2 | BAD-IMPL | `include/api/os1.h:93` | `void write(int fd, const char *buf, size_t count)` — returns void; callers cannot detect short writes or errors. POSIX specifies `ssize_t`. Asymmetric with `long read(...)`. |
| ABI-SYS-03 | W2 | MISSING | `include/api/errno.h` | `errno.h` declares `extern int errno` but provides no error-code definitions (those live in `posix_types.h`). No syscall stub sets `errno`. Callers who check `errno` after any syscall always see 0. |
| ABI-SYS-04 | W2 | WRONG-DESIGN | `include/api/posix_types.h:126-134` | AArch64-specific `dsb sy/ld/st`/`isb` inline assembly is in a shared public header. Compiling userland for AMD64 with this header included fails to assemble. [inferred: AMD64 userland programs include `os1.h` → `posix_types.h`; AMD64 does not have `dsb`. Assumption: AMD64 userland is compiled with the same include path.] |
| ABI-SYS-05 | W1 | DOC | `include/api/posix_types.h:2` | File banner says "kernel/include/kernel/types.h" but the file lives at `include/api/posix_types.h`. |
| ABI-SYS-06 | W2 | BAD-IMPL | `include/api/os1.h:150-160,173-175` | `sin_fp`, `cos_fp`, `fixmul` are declared **twice** in the same header: once as `int32_t` (`:150-152`) and again as `int` (`:173-175`). These are incompatible signatures on 64-bit targets. |
| ABI-SYS-07 | W2 | BUG | `include/api/os1.h:51-57` + `kernel/include/kernel/sched.h:15-22` | `struct ps_info` is duplicated verbatim. A field change in one copy silently misaligns the layout seen by userland vs. the kernel. |
| ABI-SYS-08 | W2 | SECURITY | `include/api/os1.h:165` | `char *gets(char *s, int size)` — `gets` is deprecated/removed from C11 for being inherently unsafe. The non-standard second parameter does not make it safe (the kernel-side implementation is unknown but the declared interface invites unchecked reads). |
| ABI-SYS-09 | W1 | BAD-IMPL | `include/api/stdio.h:53` | `#define fprintf(f, ...) printf(__VA_ARGS__)` silently discards the stream. `fprintf(stderr, ...)` goes to stdout; errors are invisible. `vfprintf` is declared but the macro bypasses it. |
| ABI-SYS-10 | W1 | MISSING | `include/api/errno.h` | No `errno.h` content beyond the bare `extern int errno` declaration. POSIX-compat programs that `#include <errno.h>` expecting `EINVAL`, `ENOMEM` etc. will not compile unless they also pull in `os1.h`→`posix_types.h`. |
| ABI-SYS-11 | W1 | STUB | `user/sys/lib/lib.c:364,386-387` | `system(cmd)` always returns 0; `remove(path)` and `rename(old,new)` are always no-ops. Declared in `stdlib.h`/`stdio.h` but silently do nothing. |
| ABI-SYS-12 | W0 | MISSING | `include/api/elf.h` | `EM_X86_64` (62) is absent from `include/api/elf.h` — only `EM_AARCH64` (183) is defined. AMD64 programs calling `exec` cannot validate the ELF machine type without pulling in the kernel-internal `kernel/include/kernel/elf.h`. |
| ABI-HDR-01 | W2 | BUG | `kernel/include/kernel/elf.h:1` | `#ifndef _KERNEL_ELF_H` is present but `#define _KERNEL_ELF_H` is missing. Both files share the guard token: `include/api/elf.h:1` also uses `_KERNEL_ELF_H`. If `api/elf.h` is included first in a TU, `kernel/elf.h`'s body is silently skipped, leaving `EM_X86_64` (62) undefined — which breaks `ARCH_TYPE EM_X86_64` in `arch/amd64/include/arch/arch.h:9`. |
| ABI-HDR-02 | W2 | BAD-IMPL | `kernel/include/kernel/sched.h:8` | `#pragma GCC optimize("O2")` in a shared header forces O2 on **every** translation unit that includes `sched.h`. This overrides per-file or whole-kernel optimization settings and may suppress debug information. |
| ABI-HDR-03 | W1 | BAD-IMPL | `kernel/include/kernel/types.h:11` + `kernel/include/kernel/elf.h:4` | Deep relative includes (`../../../../include/api/...`) bind header layout to directory structure. A tree reorganization silently breaks the build. |
| ABI-HDR-04 | W1 | BAD-IMPL | `kernel/include/kernel/arch.h` + `kernel/include/kernel/hal_unified.h` | Both headers expose parallel wrapper APIs over the same `arch_impl_*` primitives (IRQ save/restore, TLB, cache, spinlocks, barriers — all defined in both). Callers mix `arch_*` and `hal_*` names for the same operations. |
| ABI-HDR-05 | W0 | DOC | `kernel/include/kernel/spinlock.h:4` | File comment says "Simple spinlock implementation for AArch64" but the file is compiled for both architectures. |

---

### 5.1 Detailed Entry — LIB-REG-01 + LIB-REG-02: Registry vs. Plan 9 File Namespace `[static]`

**What exists today:**

`registry.c` is a fixed-size flat key-value store: a 128-element array of `struct registry_entry {char key[64]; char value[128]; int used;}` protected by a single spinlock. The only operations are `registry_set(key, value)` and `registry_get(key, buf, size)`, exposed via `sys_registry(op=0|1, key, value, size)`.

Keys use dotted notation (`"theme.color"`, `"system.hostname"`, `"mouse.sensitivity"`) suggestive of a namespace hierarchy, but the store is flat — there is no tree, no parent, no child. Lookup is O(n) linear scan. Capacity is hard-capped at 128 entries (8 KB of static RAM). There is no delete, no rename, no enumerate, and no permission check (the comment at `registry.c:10` defers access control to "if needed later", meaning any process can overwrite `"system.hostname"` — LIB-REG-02).

**The Plan 9 model:**

In Plan 9, *everything is a file*. Configuration, hardware state, and system parameters are exposed as files in a synthetic filesystem (`/env`, `/dev`, `/proc`). A client reads or writes `/env/TERM` rather than calling a special syscall. Permissions follow standard file permissions. The namespace is per-process and mountable: a process can bind its own synthetic FS over any path. New "registry entries" are added by creating new files, not by calling a kernel API. The plan enables:
- Discovery (listdir gives all keys)
- Composition (mount a foreign FS over `/env`)
- Security (per-file permissions, per-process namespaces)
- Orthogonality (no special `sys_registry` syscall; standard `open`/`read`/`write`)

**The gap:**

| Dimension | Today's registry | Plan 9 target |
|---|---|---|
| Data model | Flat 128-slot array | Hierarchical directory tree |
| Enumeration | None | `readdir` on any node |
| Permissions | None (any process writes) | Per-node `uid`/`gid`/`mode` |
| Namespace | Global singleton | Per-process, mountable |
| Interface | Bespoke syscall (SYS_REGISTRY=250) | Standard `open`/`read`/`write`/`readdir` |
| Values | Strings only, ≤ 128 bytes | Arbitrary byte streams (files) |
| Growth | Hard cap 128 | Dynamic (bounded by heap) |
| Persistence | In-memory only | Optionally backed by storage |

**Path forward** (see §6):

The dotted key hierarchy maps naturally to a directory tree. The existing `vfs.h` declares `vfs_resolve_path` but provides no mount or synthetic-FS API. The correct architecture is to expose a `/reg` or `/env` pseudo-filesystem through the VFS layer, served by a kernel synthetic-FS driver, with each key as a regular file under a path derived by replacing `.` with `/`. The bespoke `sys_registry` syscall can then be removed once the VFS path (`open`/`read`/`write`) is wired to the same store.

---

### 5.2 Detailed Entry — ABI Coherence Assessment `[static]`

**Verdict:** The `include/api/` headers form a collection of per-concern shims, not a coherent ABI. Five structural defects drive this:

**1. No single syscall contract.** Syscall numbers live in `os1.h:19-46`. The dispatch table in `kernel/core/syscall_dispatch.c` uses numeric literals with comments (`case 251: /* FILE_WRITE */`). No shared header, no enum, no `_Static_assert` connects these. A maintainer can change `SYS_FILE_WRITE` from 251 to 260 in `os1.h` and the old value still gets dispatched silently. All 20+ syscalls are in this state.

**2. Unstable type layout.** `struct ps_info` is duplicated in `os1.h:51-57` and `kernel/include/kernel/sched.h:15-22`. `struct ipc_message` is shared via `posix_types.h` (correct). The pattern is inconsistent: some structures are shared, others are copied.

**3. Arch-specific asm in the public ABI.** `posix_types.h:126-134` defines `mb()`, `rmb()`, `wmb()` as AArch64 `dsb` instructions. This header is included by `os1.h`, which is included by all userland programs. AMD64 userland cannot include `os1.h` if these macros are active during compilation for AMD64. [inferred: AMD64 userland may currently avoid this by conditionals, but no `#ifdef ARCH_AARCH64` guard is present in `posix_types.h`.]

**4. Conflicting function signatures.** `os1.h` declares `sin_fp`, `cos_fp`, and `fixmul` twice — as `int32_t` (lines 150-152) and as `int` (lines 173-175). On a 64-bit ABI these may have the same calling convention, but the compiler sees duplicate declarations with potentially different types and may warn or error depending on strictness. The second set at `:173-175` appears to be a leftover copy-paste that was never removed.

**5. POSIX facade without POSIX semantics.** The headers are named `stdio.h`, `stdlib.h`, `string.h`, etc. but diverge from POSIX in ways that will silently break ported code:
- `write()` returns `void` (should be `ssize_t`)
- `errno` is declared but never set
- `fprintf` is a macro that discards the stream
- `remove`, `rename`, `system` are declared but no-op
- `getenv` always returns NULL
- `sscanf`/`vsscanf` are implemented (in `lib.c:297-355`) — this is a strength
- `strdup` is implemented (in `lib.c:190`) — another strength

The ABI looks POSIX-like enough to attract ports, but the divergences will produce hard-to-debug silent failures.

**What works:** Types are single-sourced via `posix_types.h`. The `ipc_message` struct is shared kernel/userland. `printk`/`snprintf` carry `format(printf,...)` GCC attributes for compile-time checking. The `os1.h` umbrella header provides a reasonable starting point.

---

### 5.3 Detailed Entry — FDT / Device-Tree State `[static]`

**What is implemented:**

`fdt.c` provides four functions. On AArch64 (`#ifndef ARCH_AMD64`):

- `fdt_init(addr)`: Validates the `0xd00dfeed` magic, stores `fdt_ptr`. Falls back to a RAM scan via `fdt_find_in_memory` if `addr == 0`.
- `fdt_find_in_memory(start, end)`: Byte-scans aligned addresses for the big-endian magic (`0xedfe0dd0`); does not validate `totalsize` or structure offsets of the candidate (LIB-FDT-04).
- `fdt_get_mem_regions`: Walks the structure block, finds all `memory@*` nodes, reads `reg` properties respecting `#address-cells`/`#size-cells`. Correctly handles 32- and 64-bit encodings. Used by `arch/aarch64/platform.c:46` to feed `pmm`.
- `fdt_count_cpus`: Counts `cpu@*` nodes under the `cpus` node. Used by `arch/aarch64/platform.c:112` to determine SMP count.

On AMD64: `fdt_init` and `fdt_find_in_memory` return `-1`/`0` unconditionally. No device-tree is used. Memory comes from multiboot2.

**What is missing / incomplete:**

- **No IRQ controller discovery.** The GIC base address and interrupt map are not parsed from the DT. These are currently hardcoded in `arch/aarch64/platform.c` via `HAL_*` constants.
- **No `compatible` string matching.** There is no generic device probing mechanism; drivers must know their MMIO addresses a priori from HAL constants, not from DT data.
- **No reserve map** (`off_mem_rsvmap`) is parsed (LIB-FDT-02). Firmware-reserved RAM (e.g. ATF secure memory, GPU memory carveout) can be overwritten by the PMM.
- **No bounds validation** on structure offsets (LIB-FDT-01). A malformed DTB can produce out-of-bounds reads; however, in the current boot path the DTB is the QEMU-generated blob, which is well-formed. Risk is low for now but rises if real hardware is targeted.

**Who calls what:**

The call chain is: `kernel_main` (aarch64) → `fdt_init(x0_arg)` → `arch_platform_early_init()` → `aarch64/platform.c:fdt_get_mem_regions` + `fdt_count_cpus`. The FDT result feeds directly into PMM initialization, making it on the critical boot path for aarch64. On amd64, `main.c:79-80` sets `boot_fdt_ptr = 0` and calls `fdt_init(0)` which immediately returns `-1`.

**Assessment:** FDT handling is purposefully minimal — it extracts exactly what the kernel needs (memory map, CPU count) and no more. This is appropriate for the project's current scope. The gaps (IRQ, `compatible`, reserve map) will matter when targeting real AArch64 hardware beyond the `virt` QEMU machine.

---

### 5.4 Detailed Entry — LIB-MATH-01: FP_PI Divergence `[static]`

When `KERNEL` is defined (i.e., in kernel builds), `math.c:8-9` includes `<kernel/math.h>`. That header defines `FP_PI 411775` with the comment "3.14159 × 131072" (`kernel/math.h:16`). Since 131072 = 2^17, this value is actually *2π × 2^16* (≈ 6.2832 × 65536), not *π × 2^16* (≈ 3.1416 × 65536 = 205887).

`math.c:25-27` then has `#ifndef FP_PI / #define FP_PI 205887 / #endif`. Because `kernel/math.h` defined it already, this correct value is never used in kernel builds.

Consequence: `sin_fp` (`math.c:123-136`) range-reduces with `while (x > FP_PI) x -= FP_2PI`, where `FP_2PI = 411775`. When `FP_PI == FP_2PI`, the range-reduction loop condition `x > 411775` triggers only for angles beyond 2π — inputs between 0 and 2π pass through unreduced.

The subsequent reflection at `:132-136` (`if x > half_pi: x = FP_PI - x`) uses the wrong pole: it computes `FP_PI(411775) - x` instead of `true_pi(205887) - x`. For any input where `|x| > π/2` (approximately `x > 102944` in 16.16), the reflection maps to the wrong quadrant. For example, `sin_fp(205887)` (= π in correct 16.16 notation) folds to `411775 - 205887 = 205888` (≈ π), and the Taylor series at ≈ π returns approximately 0.53 (≈ 34800 in 16.16) instead of 0. The bug corrupts `sin` for all angles in `(π/2, 2π]` — three-quarters of the useful domain — **in the compiled `sin_fp`/`cos_fp` functions**. *Maintainer note (latent):* the only kernel caller, `kernel/graphics/draw3d.c:114-115`, is **not in the Makefile build**, and no other compiled kernel code calls `sin_fp`/`cos_fp` (verified by grep), so there is **no live runtime impact today**; the bug becomes live the moment any 3D/rotation code is compiled in.

Userland (`os1.h:170`) defines `FP_PI 205887` (correct). Math is correct in userland; broken in kernel builds.

---

## 6. Refactor Direction

### 6.1 Registry → Plan 9 file namespace

| Step | Action |
|---|---|
| 1. Define synthetic-FS API | Extend `vfs.h` with `vfs_mount_synthetic(path, ops)` and a `struct synth_ops {read, write, readdir, open, close}`. |
| 2. Implement a `/reg` pseudo-FS | A kernel module that maps the flat `registry_store[]` into a directory tree, replacing `.` separators with `/`. Each entry becomes a regular file; reads return the value, writes update it. |
| 3. Add permissions | Each synthetic file gets `uid`/`gid`/`mode` at creation. Kernel entries (e.g. `system.hostname`) are owned by uid=0, mode=0644. User entries are mode=0666. |
| 4. Add enumeration | Implement `readdir` on the synthetic FS; directories list their children. |
| 5. Retire `sys_registry` | Once `/reg` is mountable and accessible via standard `open`/`read`/`write`, remove syscall 250 and the `sys_registry` wrapper. |
| 6. Per-process namespaces | Longer-term: each process gets a private namespace where it can bind its own synthetic FS over `/reg`, as in Plan 9's `bind`/`mount`. |

### 6.2 Coherent versioned ABI

| Step | Action |
|---|---|
| 1. Single syscall-number header | Create `include/api/syscall_nr.h` with `#define __NR_read 63` etc. Use `_Static_assert(__NR_read == 63, ...)` in both the dispatch table and the user stub. |
| 2. Shared struct header | Move `struct ps_info` and `struct ipc_message` to a dedicated `include/api/os1_types.h`; include it from both `os1.h` and `sched.h`. Remove the duplicate. |
| 3. Fix `posix_types.h` ABI leakage | Move `dsb`/`isb` barriers behind `#ifdef ARCH_AARCH64`; or remove them from the public header entirely (kernel-internal code uses `arch_mb()` etc.). |
| 4. Fix `write` return type | Change to `ssize_t write(int fd, const void *buf, size_t count)`. Propagate the kernel's return value through the stub. |
| 5. Implement `errno` | Let syscall stubs call a `__set_errno(int e)` helper when the kernel returns a negative value. Expose `errno` via a TLS slot or a per-process pointer. |
| 6. Fix duplicate declarations | Remove the second `sin_fp`/`cos_fp`/`fixmul` block from `os1.h:173-175`. |
| 7. Fix `kernel/elf.h` guard | Add `#define _KERNEL_ELF_H` on line 2. |
| 8. Remove `#pragma optimize` from `sched.h` | Move it to the `.c` file or drop it. |

### 6.3 FDT / device-tree completeness

| Step | Action |
|---|---|
| 1. Validate DTB offsets | Check that `off_dt_struct + size_dt_struct <= totalsize` and `off_dt_strings + size_dt_strings <= totalsize` before parsing. |
| 2. Parse reserve map | Iterate `off_mem_rsvmap` entries and pass them to the PMM as `MEM_REGION_RESERVED`. |
| 3. Add `compatible` lookup | Implement `fdt_find_node_by_compatible(compatible_string)` to allow driver probing of GIC, serial, and timer from DT. |
| 4. IRQ parsing | Parse `interrupts` and `interrupt-parent` to auto-discover GIC base + IRQ numbers, removing hardcoded `HAL_*` platform constants. |

### 6.4 Library hardening

| Finding | Fix |
|---|---|
| LIB-MATH-01 | Change `kernel/include/kernel/math.h:16` to `#define FP_PI 205887` (π × 2^16). Verify `sin_fp` correctness with a ktest. |
| LIB-SSP-01 | Read a hardware entropy source (RNDR on AArch64, RDRAND on AMD64) at boot and XOR the result into `__stack_chk_guard`. |
| LIB-KTEST-01 | Add a `ktest_failed` counter; set it in `KASSERT`; decrement `passed` if the test exits via `KASSERT`. Report the real counts. |
| LIB-UTF8-01 | Add a `len` parameter to `utf8_decode` or document that it requires a NUL-terminated string with at least 4 bytes of read-ahead. |
| LIB-REG-02 | Enforce permissions in `sys_registry`: check `current_process->permissions & PROC_PERM_ROOT` before allowing writes to keys prefixed `system.` or `kernel.`. |

---

## 7. Verification Notes

All analysis is **[static]**. No build or runtime execution was performed in this review session.

The following would be needed for a **[verified]** pass:

- Build both arches with `-Werror`; the broken `kernel/elf.h` guard (ABI-HDR-01) will cause the missing `#define _KERNEL_ELF_H` to allow `include/api/elf.h` (which uses the *same* guard token) to set the flag first. A subsequent include of `kernel/elf.h` in the same TU is then silently skipped, leaving `EM_X86_64` undefined — which breaks `#define ARCH_TYPE EM_X86_64` in `kernel/arch/amd64/include/arch/arch.h:9`. The `sin_fp`/`cos_fp`/`fixmul` redeclaration in `os1.h` may produce conflicting-type warnings under `-Wstrict-prototypes`.
- Runtime: call `sin_fp(205887)` (the correct numeric value of π × 2^16) from a ktest and assert the result is ≈ 0 (within ±64 in 16.16 representation). In a kernel build with `FP_PI = 411775`, the reflection step computes `411775 − 205887 = 205888` and Taylor-evaluates at ≈ π, returning ≈ 34800 (~0.53), so the assertion fails — exposing LIB-MATH-01. Using `sin_fp(FP_PI)` would not expose the bug because the broken reflection maps 411775→0 correctly by accident. The bug is also confirmed by the 3D renderer caller (`kernel/graphics/draw3d.c:114-115`) using `k_sin_fp`/`k_cos_fp`, which delegate to `sin_fp`/`cos_fp` — all rotation math in the kernel is wrong for angles in (0, 2π].
- Runtime: call `registry_set("x", "y")` from a non-root process and assert it fails. Currently it succeeds (LIB-REG-02).
