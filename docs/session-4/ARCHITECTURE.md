# ASTRA implementation map and ordered gates

## Observed state

- Kernel process: privilege level and cap bitmap, but no verified real UID/GID/groups in the PCB. SDK getuid/getgid always return 0; passwd/group enumerators only provide a hardcoded root record. The current mutation entry points return ENOTSUP. The UID/GID getters still report zero.
- VFS: capability and path-level ACL at vfs_write_allowed, with shared /home rather than UID/GID-based inode access. Ext4 inode mode is present but the effective user-owner policy is incomplete.
- nxmanageDD is the existing MACHINE-supervised manager with PID1-authenticated service registration, slot/generation, restart and registry state. nxinit consumes the per-image services.conf. The manifest requires an explicit capability mask. The owned PROCESS spawn accepts both arguments and a capability ceiling. A hosted terminal applies the USER ceiling when spawning its payload.
- nxcompositor, nxttyd, nxpower and surface/layout-v1 are existing compositor, terminal, power-UI and overlay hooks to integrate, not duplicate.
- The upstream Gnulib and Coreutils test sources exist. Current guest gnulibtest and coreutilstest are custom tests, not an executed official GNU suite.

## Input service authority and application input isolation

Hardware queues are owned by the **existing** kernel input provider and the two existing USERKIT routers: `nxkeyboardd` (keyboard translation/focus) and `nxmoused` (pointer/focus/capture). Acquisition of `input/keyboard` and `input/pointer` requires the corresponding **service-only** capability (`CAP_KEYBOARD_RAW` or `CAP_POINTER_RAW`) delegated explicitly by each image's immutable manifest and validated by `nxmanageDD` and the input resource provider. `CAP_WINDOW` is not an input-hardware authority. `nxkeyboardd` does not receive graphics authority merely to read hardware input; `nxmoused` retains its independent window/focus authority.

Ordinary processes, including shells, never require an input capability declaration in a service catalog. `nxkeyboardd` routes translated `IPC_TYPE_INPUT` into the focused process's established per-process queue; for terminal windows, the existing `nxttyd` forwards to the session controller. Pointer routing remains in `nxmoused`. No new input daemon, global per-app authorization gate, duplicate ring buffer, input syscall or compositor-specific keyboard engine is introduced. Security is enforced at the device handle's acquisition and at the real process/window/TTY IPC destinations, not by denying all user input.

A TTY host owns its window and publishes a family-scoped output port before launching its child. Each interactive shell uses the port of its own host. The one-shot terminal entry in the image manifest opens the initial window. Input delivery and subsequent terminal launches are independent of that entry's lifetime. The process queue admits translated keyboard events from the keyboard service and from the receiver's parent or delegated job owner. Raw pointer events require the pointer service's capability. These checks cover PID sends and writes to PROCESS handles.

**Required tests:** direct raw-device acquisition denied for an ordinary CAP_WINDOW process; both routers with their respective device capabilities can acquire correct devices; actual QEMU keyboard and pointer input reach focused application/TTY on both architectures through nxadb; focus change must redirect the next event. No auth policy or input refactor counts as complete based on service startup logs alone.

## Component boundaries

1. Kernel mechanism: kernel-stamped, immutable principal identity (UID/GID/group set and generation), true per-object rights and revocation, owner-aware filesystem access and secure process-context transition. No usernames, password logic or OS-specific popup rules inside the kernel. Entropy comes from existing HAL.
2. USERKIT: nxauthd as a narrowly privileged credential/policy broker; extend existing nxmanageDD with per-service identity, explicit capability ceiling, exact executable identity, startup/exit generation, singleton and revocation. Each resource is a capability object, not an ambient service-name privilege.
3. SDK: identity/account/group and VFS clients, unified exec/descriptor adaptation for GNU, user/account settings, service IPC and popup library. Unsupported operations fail closed instead of returning false success; no duplicate POSIX policy in the kernel.
4. NexsOS1-main: login GUI, nxsettings and popup style. Universal popup contract has blocking requester-surface modal and nonblocking system overlay; the constrained terminal-choice path must offer equivalent options for nxadb. nxui and nxbar stay outside USERKIT.
5. Secure nxadbd: real in-guest daemon, authenticated and revocable session over verified transport (USB only after device/protocol study); never unauthenticated debug shell. Host remembered credentials belong in OS-protected secret storage, not source, logs or command line.
6. Storage: migrate current /home to user1 via backed-up installer migration after genuine owner metadata and authorization exist. Do not hardcode user1 path exceptions.

## Sequential deliverables and acceptance

| Phase | Work | Release gate |
| --- | --- | --- |
| S4.0 | Document code, stop fake-success identity/ownership shims | Explicit negative libc tests, Power12 changed gate, AMD64 and ARM64 QEMU |
| S4.1 | Real process principal/UID/GID/group and VFS ownership | Both users have correct metadata and cross-user denial, concurrency and handle tests |
| S4.2 | Narrow service authority and MACHINE policy | Existing manager supervises all services with args+capability spawn, singleton/restart and escalation tests |
| S4.3 | Cryptographic trust chain and nxauthd | HAL entropy health, audited CSPRNG/KDF vectors, protected persistence, rate limit, rollback/replay protection |
| S4.4 | Provisioning, Unix login, su, named groups, home migration | Real TTY login/logout, capability revocation, VFS privacy |
| S4.5 | Modal/system popup, remembered per-scope grants, settings | Scoped allow-once/permanent/deny/revoke and no UI spoof/focus bypass |
| S4.6 | Authenticated USB nxadbd and power object | No anonymous host access; secure paired session and authorized shutdown/reboot |
| S4.7 | GNU execution/VFS/FD compatibility and upstream suites | nice/nohup real behavior, official GNU tests executed inside both QEMU guests |
| S4.8 | Full Power12 and release | Strict 0 blockers, clean dual arch builds, NXPaX, nxadb and UI acceptance |

No secrets are checked in: user shsh is an account-provisioning goal, not a preset credential. The user-supplied temporary password must not be embedded in any repo, disk image, stdout or fixture. Root password has not been supplied; its enrollment is interactive and distinct from MACHINE. Memory-hard password KDF with CSPRNG salt, key erasure, locked-down verifier storage and credential rotation are prerequisites, not assumed current features.
