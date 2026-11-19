# Session 4 — implementation checkpoint (2026-11-19)

This checkpoint is intentionally **not** a release certification. All changes are UNSTAGED; no commit, branch switch, remote operation or push is authorized.

## Mechanisms implemented in real source

- NX-CORE/NX-SDK PROCESS owned spawn ABI: backward-compatible v1, v2 adds explicit child capability ceiling with argv; enforced in kernel and used by the existing `nxmanageDD`. No additional process creation syscall.
- The existing USERKIT init/service catalogs require explicit per-service `caps:` declarations; 14 main-image entries and 9 demo1 entries are parsed strictly and passed to the existing manager. No implicit `CAP_ALL` fallback. Input routers alone receive their respective raw device capabilities, distinct from `CAP_WINDOW`; ordinary apps receive input in existing focused IPC/TTY buffers without an input capability. This input refactor requires fresh graphical QEMU interactive verification before being called accepted.
- SDK identity and inode owner mutation APIs fail closed with ENOTSUP instead of fabricating successful UID/GID or owner changes.
- PIPE object readiness uses kernel-backed capability query; SDK `poll()` performs bounded descriptor checks and truthful EOF/error reporting.
- Kernel handle duplication implements attenuated `dup`, atomic target-replacement `dup2`, and minimum-descriptor `F_DUPFD`. Unsupported flag operations such as `F_GETFL` return ENOTSUP rather than fabricated state.
- `/dev/null` is a dedicated capability-backed NULL resource with actual read-EOF/write-discard semantics, not a regular ext4 fixture.
- A voluntary exit releases object handles before the process is published as a zombie, separating PIPE/PORT EOF from scheduler reaping.
- A genuine upstream Gnulib source test (`test-isblank.c`) is built for each target and executed as the 21st guest suite case. This does **not** constitute the full official Gnulib/Coreutils test suites.
- `gfxcontroltest` drains CLI output concurrently with child reaping and keeps all original command/execution validation criteria.

## Verification policy and evidence

Use `make run ARCH=amd64 RUN_DISPLAY=default` and `make run ARCH=aarch64 RUN_DISPLAY=default` with UART logs and real nxadb/QMP screenshots. All guest tests must run on both targets, and screenshots must be inspected in addition to exit statuses. For every failure preserve original test ID, output, child status and trace. No fabricated PASS/SKIP outcomes.

Host tests include `tools/test_spawn_owned.py`, `tools/test_service_manifest_caps.py`, `tools/test_pipe_status.py`, `tools/test_handle_dup.py` and `tools/test_null_resource.py` (real implementation extracted, ASan/UBSan). Guest FD tests cover PIPE readiness, invalid handles and ABI versions, duplicate handles and exact dup2, NULL device rights, and two process-exit/EOF cases.

Latest incremental Power12 gate: **33 scoped files, zero blocking and zero report-only findings**. Full-project strict Power12 remains **NOT qualified** (5,000+ legacy findings). Do not equate incremental gating with full certification.

Earlier completed guest checks: AMD64 kernel 36/36, AArch64 kernel 37/37; Coreutils 46/50, with `nice`, `nohup`, `chown`, `chgrp` failing. Graphics cases remain intermittent across modes/architectures. New process-exit/EOF guest tests still require final dual-architecture confirmation.

## Mandatory unfinished security work

1. Secure entropy supply on both architectures and audited DRBG/KDF; the existing SDK clock/PID-derived `getrandom()` **must not** be used for passwords, authentication, session tickets or pairing secrets. No credential issuer can be enabled on this source.
2. Principal identity with immutable UID/GID/group membership and generation, kernel-controlled grant/revocation, and mandatory owner/mode/ACL enforcement in the VFS object provider and ext4 inode metadata. Level/role is not a verified user identity.
3. USERKIT `nxauthd` with protected persisted account verifiers, rate limiting and authenticated capability grants. `root` and `shsh` must be enrolled without committing plaintext or static password hashes; machine authority stays distinct from root.
4. Real TTY login/session/logout and per-user home migration without hardcoded path exceptions.
5. Compositor-owned process-modal and system-modal popup protocols, with terminal-choice parity and stored, revocable, per-principal/per-app resource authorization. UI cannot grant authority by itself.
6. Authenticated, encrypted and revocable in-guest nxadbd over verified USB transport, never an unsecured host debug shell.
7. POSIX compatibility still missing true in-place exec, scheduler priority semantics, truthful filesystem ownership changes, some descriptor flags; these block GNU `nice`/`nohup` and the ownership commands. Existing libc stub `execvp()` returns ENOTSUP. Do not replace it with an untracked spawn/exit imitation.
8. Run the full unchanged official GNU test suites inside both guest architectures, not just the single upstream C case and the local Coreutils acceptance set.
9. Clean dual-target builds, strict Power12 on the entire codebase, NXPaX, interactive nxadb, UI screenshots and negative security tests before any staging/commit.

## Isolation and ownership

Keep these contracts in NX-CORE/NX-SDK/USERKIT, preserve the existing manager and object layer, and do not introduce parallel syscalls, VFS or compositor. All source and comments remain English. Any change in scope requires a new measured test cycle; only verified file groups may be staged when explicitly authorized.
