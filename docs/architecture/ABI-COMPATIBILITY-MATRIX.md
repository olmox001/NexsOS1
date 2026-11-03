# Legacy ABI Compatibility Contract — NEXS Next Generation

Status: **inventory from the working legacy tree; implementation contract for the replacement kernel**  
Source baseline: **b495da3a711e5af64dd826c29c6da4777c487074**  
Scope: keep existing userland source and its generated ELF binaries unchanged while replacing the kernel and moving in-kernel services to supervised userland servers.

## 1. Non-negotiable compatibility rule

The replacement kernel must implement the current NexsOS ABI v1 as a compatibility personality. Existing syscall numbers, register conventions, return-value conventions, structure layouts, standard descriptors, and observable error behavior are frozen.

The new kernel may implement these calls through kernel primitives or capability-protected IPC to userland services. It must not require edits to existing application source or relinking against a different API to boot the current root filesystem.

Do not reuse retired syscall numbers. New interfaces are additive and versioned; the v1 table remains available until a deliberate ABI-v2 transition.

## 2. System-call entry ABI

| Architecture | Trap | Number register | Arguments | Return |
|---|---|---|---|---|
| x86-64 | SYSCALL | RAX | RDI, RSI, RDX, R10, R8, R9 | RAX |
| AArch64 | SVC #0 | X8 | X0..X5 | X0 |

The user stubs are in **user/arch/amd64/syscall.S** and
**user/arch/aarch64/syscall.S**. Kernel entry/return paths are architecture
specific; the dispatcher consumes a normalized register frame.

**Raw syscall result:** success is the syscall's result; failure is a negative
OS1 errno value. The POSIX layer in **user/sys/lib/lib.c** converts selected
negative results to **-1** and sets **errno**. The **OS1_*** API often exposes
the negative error directly. Do not collapse these two conventions.

## 3. Frozen syscall-number map

### Process, execution, scheduling and identity

| Number | Name | v1 behavior to preserve | Replacement owner |
|---:|---|---|---|
| 93 | SYS_EXIT | Terminate caller and schedule another task | Kernel process/thread core |
| 169 | SYS_GET_TIME | Monotonic milliseconds since boot | Kernel clock primitive |
| 172 | SYS_GETPID | Current process ID | Kernel process core |
| 220 | SYS_SPAWN | Load and start an ELF child with argv and optional fd redirection | Process manager service over kernel create/address-space primitives |
| 221 | SYS_KILL | Capability/ancestry-checked process termination | Kernel process core |
| 222 | SYS_GETPROCS | Copy an array of ps_info snapshots to user memory | Kernel process introspection |
| 223 | SYS_YIELD | Yield current scheduling quantum; anti-spin policy applies | Kernel scheduler |
| 224 | SYS_GET_IDENTITY | Return caller's privilege/capability identity | Kernel capability core |
| 225 | SYS_SYSSTATS | Copy versioned os1_sysstats snapshot | Kernel counters/diagnostics |
| 234 | SYS_SPAWN_CAPS | Spawn with an explicitly requested, attenuated capability mask | Process manager + kernel capability core |
| 247 | SYS_WAIT | Non-blocking child wait/reap contract used by libc's polling waitpid | Kernel process core |

Current kernel process-pool ceiling is 128 slots; non-privileged creators have a
32-live-child quota and 8 reserved recovery slots. **include/api/os1.h** exposes
**MAX_PROCESSES=64**; preserve that source-level constant for old callers even
if the new kernel supports more processes.

### Filesystem, namespace and memory

| Number | Name | v1 behavior to preserve | Replacement owner |
|---:|---|---|---|
| 56 | SYS_OPEN | Open path; return a file handle with requested rights/flags | VFS service via a kernel file-proxy capability |
| 57 | SYS_CLOSE | Close a handle and release its reference | Kernel handle table + object backend |
| 62 | SYS_LSEEK | Change the shared open-file-description offset | VFS service |
| 63 | SYS_READ | Read from a handle; may block and retry after wakeup | VFS/console/pipe/port backend |
| 64 | SYS_WRITE | Write to a handle; preserve POSIX -EPERM → -EBADF mapping | VFS/console/pipe/port backend |
| 216 | SYS_SBRK | Return old program break; grow/shrink the caller's heap | Kernel VM primitive; user allocator policy stays in userland |
| 250 | SYS_REGISTRY | Legacy key/value registry operations | Registry service via compatibility RPC |
| 255 | SYS_CHDIR | Resolve and verify directory, update per-process cwd | VFS/namespace service |
| 256 | SYS_GETCWD | Return the caller's cwd | VFS/namespace service |
| 261 | SYS_PIPE | Create read/write pipe handles | Kernel bounded IPC/pipe object |
| 263 | SYS_STAT | Return the 16-byte abi_stat layout | VFS service via compatibility RPC |

Current SYS_SBRK maps 4 KiB pages, returns the previous break, rejects
overflow, and limits the heap to 0xBF000000. The legacy user stack occupies
[0xC0000000, 0xC0100000) (1 MiB); the loader accepts PT_LOAD addresses in
[0x70000000, 0xC0000000). Keep this virtual-address contract for v1 ELFs.
The new VM implementation may use different physical page sizes internally,
but must preserve these user virtual addresses and the sbrk behavior.

### Time

| Number | Name | v1 behavior to preserve | Replacement owner |
|---:|---|---|---|
| 257 | SYS_NANOSLEEP | Sleep for a nanosecond duration against an absolute monotonic deadline | Kernel timer/wait-queue primitive |
| 258 | SYS_CLOCK_GETTIME | Return 64-bit nanoseconds: clock 0 monotonic, clock 1 caller CPU time; unknown IDs fall back to monotonic | Kernel clocksource + per-task accounting |

The hardware timer remains a minimal kernel/HAL primitive. Higher-level wall-clock,
RTC synchronization, alarms and power policy can be userland services.

### IPC and object capabilities

| Number | Name | v1 behavior to preserve | Replacement owner |
|---:|---|---|---|
| 230 | SYS_SEND | PID-addressed legacy IPC send | Kernel compatibility IPC |
| 231 | SYS_RECV | Blocking PID-addressed legacy receive | Kernel compatibility IPC |
| 233 | SYS_TRY_RECV | Non-blocking legacy receive | Kernel compatibility IPC |
| 235 | SYS_HANDLE_CREATE | Resolve a named object in a namespace and install a rights-limited handle | Kernel capability table + service proxy |
| 236 | SYS_HANDLE_DUP | Duplicate a handle with rights attenuation | Kernel capability table |
| 237 | SYS_HANDLE_CLOSE | Close a capability handle | Kernel capability table |
| 238 | SYS_CAP_QUERY | Query the rights/type of a handle | Kernel capability table |
| 239 | SYS_CAP_GRANT | Grant/transfer attenuated rights to another process | Kernel capability transfer |
| 240 | SYS_OBJECT_READ | Read from FILE/PROCESS/WINDOW/CONSOLE/PIPE/PORT/REGKEY objects | Kernel IPC primitive or service proxy |
| 241 | SYS_OBJECT_WRITE | Write to the same object family | Kernel IPC primitive or service proxy |
| 242 | SYS_OBJECT_WAIT | Wait/poll on an object | Kernel wait queues + service event proxy |
| 243 | SYS_OBJECT_CTL | Type-specific object control | Kernel or owning service |
| 262 | SYS_PORT_SEND_CAPS | Send a message and transfer capability handles atomically | Kernel IPC/capability core |

The legacy object namespaces are FS, PROC, WIN and PORT. The object types include
FILE, PROCESS, WINDOW, CONSOLE, PIPE, PORT and REGKEY. Rights are READ, WRITE,
WAIT, DESTROY, DUPLICATE, TRANSFER and CREATE. Existing coarse privilege bits
are CAP_SPAWN, CAP_FS_WRITE, CAP_IPC_ANY, CAP_WINDOW and CAP_REG_WRITE; the
level presets and their attenuation rules are defined in **include/abi/caps.h**.

Current resource bounds (minimum compatibility guarantees, not desired NG limits):
- 32 handle slots per process (NPROC_HANDLES).
- 16 named ports; 32 queued messages per port; 32-byte port names.
- 4 KiB anonymous pipe ring buffer.
- 16 MiB maximum object read/write request.
- One legacy IPC message is 88 bytes.

The new kernel should use generation-tagged integer handles (never user-visible
kernel pointers), a bounded and configurable handle capacity, per-object
references, and sharded/per-CPU fast paths. Old fd/handle values remain opaque
non-negative integers; descriptors 0, 1 and 2 retain stdin/stdout/stderr
semantics.

### Graphics, display, input and fonts

| Number | Name | v1 behavior to preserve | Replacement owner |
|---:|---|---|---|
| 200 | SYS_DRAW | Legacy root-framebuffer rectangle draw | Compositor/display service |
| 202 | SYS_WINDOW_ENUM | Copy window_info[] to caller | Compositor service |
| 210 | SYS_CREATE_WINDOW | Create owned window; return legacy window ID | Compositor service |
| 211 | SYS_WINDOW_DRAW | Draw into a window | Compositor service |
| 212 | SYS_COMPOSITOR_RENDER | Request compositor render | Compositor service |
| 213 | SYS_WINDOW_BLIT | Blit caller-supplied pixel data | Compositor service; shared-memory fast path |
| 214 | SYS_WINDOW_SET_FLAGS | Set owner-authorized window flags | Compositor service |
| 215 | SYS_DESTROY_WINDOW | Destroy caller-owned window | Compositor service |
| 217 | SYS_WINDOW_WRITE | Write text to a window/console | Compositor/terminal service |
| 218 | SYS_WINDOW_OF_PID | Resolve a process's window | Compositor service |
| 219 | SYS_WINDOW_GRID | Legacy window-grid operation | Compositor service |
| 232 | SYS_SET_FOCUS | Set focus subject to capability/ownership rules | Compositor/input service |
| 253 | SYS_SET_FONT | Replace global font; validates and copies user blob | Font service |
| 270 | SYS_DISPLAY_INFO | Return display dimensions/mode | Display service |
| 271 | SYS_SET_DISPLAY_MODE | Global mode change gated by CAP_WINDOW; current guest preset also includes CAP_WINDOW, so this is not a strong privilege boundary | Display service; add a dedicated CAP_DISPLAY in ABI v2 |
| 272 | SYS_WINDOW_RESIZE | Resize an owned/authorized window | Compositor service |
| 273 | SYS_DISPLAY_POLL | Machine-level display/input polling | Input/display service |
| 274 | SYS_SET_STYLE | Change desktop style, gated by the same broadly granted CAP_WINDOW | Compositor + registry services |
| 275 | SYS_SET_ZOOM | Change desktop zoom, gated by the same broadly granted CAP_WINDOW | Compositor + registry services |

These syscalls are a compatibility facade only. The replacement kernel must not
reintroduce a compositor, font manager or desktop policy into its trusted core.
Bulk pixel/font transfers should use capability-authorized shared memory or
bounded copy-in/copy-out, never trust a raw user pointer or retain it after the
syscall returns.

## 4. Frozen structure layouts

The following layouts are read by both kernel and userland. Existing
NX_ASSERT_OFFSET/NX_ASSERT_SIZE checks in **include/abi/posix_types.h**
already freeze ipc_message and abi_stat; extend this discipline to every
cross-boundary structure in the SDK.

| Structure | Current layout | Required rule |
|---|---|---|
| ipc_message | 88 bytes; from@0, type@4, data1@8, data2@16, payload@24 (64 bytes) | Exact offsets and size on both ISAs |
| abi_stat | 16 bytes; size@0, type@8, explicit pad | Exact offsets and size |
| window_info | 92 bytes: seven 32-bit fields + 64-byte title | Add explicit offset/size assertions |
| spawn_redir | 12 bytes: three 32-bit fields | Add explicit offset/size assertions |
| ps_info | Fixed-width pid/state/priority/name/CPU-time snapshot | Add explicit offset/size assertions |
| os1_sysstats | Versioned prefix: version, struct_size, then 64-bit counters | Preserve prefix; readers use struct_size |
| timespec | 64-bit seconds and nanoseconds in the OS1 time API | Freeze in SDK; do not inherit host libc layout |

No pointer-sized field may cross the ABI unless its width is explicitly fixed.
Use fixed-width integer types, explicit padding, and compile-time assertions
built by both cross-compilers.

## 5. ELF and process-start contract

The current userland outputs are **static ELF64 ET_EXEC** binaries:
- x86-64 EM_X86_64; AArch64 EM_AARCH64.
- No PT_INTERP dynamic linker is required.
- Link address is around 0x80000000; the lowest load page may be
  0x7FFFF000.
- The loader creates a 1 MiB user stack at
  [0xC0000000, 0xC0100000).
- argc/argv are placed on the initial stack and passed as the first two
  user arguments; environment propagation is currently a registry-based
  userland convention, not an ELF envp contract.
- Spawn clamps to 16 arguments, 128 bytes per argument, and at most 8 fd
  redirections. Redirection flags are masked to the defined three bits.

The current loader maps 4 KiB pages, creates W^X user mappings, and rejects
PT_LOAD segments outside the legacy user VA window. The NG loader must retain
the v1 ELF behavior while hardening validation (ELF type/version, header and
program-header bounds, integer overflow, p_filesz <= p_memsz, alignment,
overlap, and all-or-nothing load rollback). A malformed or short-read image
must never become runnable.

## 6. Existing POSIX personality: preserve what exists, do not invent support

The userland C library is the project's custom **user/sys/lib/lib.c**, not musl.
It translates OS1 syscalls into the POSIX subset used by current applications.
Some functions are deliberately stubs: examples include fork and the exec
family (ENOSYS/ENOTSUP), mknod, mkfifo, fchdir, copy_file_range, and fdopendir.
Other compatibility functions return documented dummy/no-op values (for
example resource limits and priority). Preserve current results for the
original userland; do not silently claim full POSIX support.

There is no current public pthread/thread-creation syscall, no mmap ABI,
no fork/execve image replacement, no sockets, and no named FIFOs. The
new kernel may add thread, VM-object and network interfaces additively, but
must not make existing binaries depend on them.

## 7. Kernel/service split required by v1 compatibility

| Legacy behavior | NG owner | Kernel responsibility retained |
|---|---|---|
| VFS, Ext4, path resolution, file offsets, stat, cwd | vfsd / filesystem services | Capability handles, IPC, safe buffer mapping, boot rootfs handoff |
| Registry key/value API | registryd | Service endpoint capability and caller identity |
| Windows, drawing, display mode, style, zoom | compositord / displayd | IPC, shared memory, process identity, IRQ delivery |
| Font loading/raster data | fontd or compositor service | Validate/map bounded transfer; no retained user pointers |
| Input events | inputd | Interrupt routing, event delivery, capability checks |
| Device discovery tree | devmgr | Minimal boot-time FDT/ACPI/PCI discovery and immutable snapshot handoff |
| Hardware drivers | Sandboxed driver services | MMIO mapping, IRQ capabilities, DMA/IOMMU domains, scheduling and isolation |
| User heap/malloc policy | Userland allocator/runtime | Address spaces, page mapping, fault handling, legacy sbrk adapter |
| Process policy/supervision | system_init / process manager | TCB, context switch, address-space lifecycle, scheduler, process IDs |
| Timer policy/RTC/power policy | timed / powerd | Monotonic clocksource, per-CPU deadline timers, interrupt and CPU power primitives |
| Physical page allocation / page tables | Minimal kernel VM core | Buddy/frame allocation, map/unmap/protect, TLB shootdown, DMA pinning |

A small boot supervisor must start the VFS and core services before it starts the
unchanged legacy **/sys/bin/init**. The existing init binary then continues to
launch the existing notification, UI, launcher and shell programs unchanged.

## 8. Compatibility acceptance gates

The NG kernel is not allowed to replace the legacy kernel until all of these
pass on both x86-64 and AArch64:

1. Compile the unchanged userland and all currently wired libraries from the
   pinned submodule revisions.
2. Boot the exact same nxshell.elf, nxui.elf, nxlauncher.elf and
   integration apps; no userland source patch.
3. Run syscall conformance tests for every number in §3, including negative
   errno mapping, blocking/retry behavior, rights attenuation, and close/reap
   races.
4. Compare the structure layouts in §4 with cross-compiled static assertions.
5. Validate all v1 ELF load/stack/argv cases and malformed ELF rejection.
6. Validate service restarts and failure semantics: a crashed VFS/compositor/
   driver service must not corrupt the kernel or strand unrelated processes.
7. Validate 4-core SMP, IPC under contention, shared-buffer lifetime, TLB
   shootdown and DMA isolation.
8. Record per-operation WCET budgets per exact CPU/platform profile; unknown
   hardware timing must be bounded by explicit device timeouts and fallback,
   not represented as a universal cycle guarantee.

## 9. Inventory-derived cautions

- The current handle table is fixed at 32 slots and object manager mutations
  share a global spinlock. This is the legacy baseline, not the NG design.
- Named ports are capped at 16, queue depth 32, name length 32 bytes; anonymous
  pipes use a 4 KiB ring. Preserve at least these guarantees for old apps.
- The current scheduler has 128 process slots, one schedulable context per
  process, per-parent quotas and reserved recovery slots. The v1 ABI has no
  thread creation call.
- SYS_GET_TIME is monotonic milliseconds, while SYS_CLOCK_GETTIME
  returns nanoseconds. Do not conflate them.
- SYS_WAIT is not a blocking POSIX wait primitive; the current libc polls it
  with yield/sleep logic. Preserve its exact return values.
- SYS_SET_FONT currently copies and validates the user blob into kernel-owned
  memory, including a second validation after copy to prevent TOCTOU. A font
  service must preserve this ownership safety.
- SYS_READ can block and re-enter the syscall after a wakeup. A service-backed
  implementation must preserve retry, cancellation and descriptor lifetime
  semantics.
- The userland build currently integrates SDL2, Lua, Gnulib, Coreutils and DOOM;
  other initialized external trees (for example Mesa, Wine/D3D and musl) are
  not automatically built by make all today. Do not claim those ports are
  already active in the runtime.
