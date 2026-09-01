# Coreutils port audit for NexsOS1

## Current verified count

The repository's official upstream list is generated from GNU coreutils itself:

- upstream total: 110 programs
- currently present in the NexsOS1 rootfs: 52
- currently missing: 58

This count was checked against the real output tree under build/<arch>/rootfs and normalized to the public command names (`cu_echo` -> `echo`, etc.).

## Real system model we need to respect

The port is not a generic POSIX host emulation. It is a kernel-backed VFS with a real privilege model:

- `/sys/bin` is the machine/system tree and is effectively privileged/root-backed
- `/bin` is the user-facing tree and is not privileged in the same way
- `/home` is the writable userland area
- execution is mediated by the OS1 execution service (`nxexec`) and by the kernel spawn path, not by a POSIX `fork()` + `exec()` runtime
- capability checks and creator-clamp rules are enforced by the kernel process model and by the VFS path policy

That matters because GNU coreutils assumes a Unix-like environment in which a command can rely on an ordinary process model, a standard `fork/exec`, and full file semantics. In NexsOS1, the compatibility layer must adapt to the real ABI and service model rather than mimic a Linux host environment by accident.

## Portability layer contract

The real porting boundary is here:

- `user/sys/lib/portability/gnulib/gnulib_config_nexsos.h`
- `user/sys/lib/portability/coreutils/coreutils_config_nexsos.h`
- `user/sys/lib/portability/gnulib/gnulib_os1_glue.c`
- `user/sys/lib/portability/coreutils/coreutils_os1_glue.c`

These files are the correct place to adapt Gnulib/Coreutils to NexsOS1. The upstream source trees under `user/bin/coreutils` and `user/sys/lib/gnulib` should remain untouched unless a real platform contract is proven missing.

## What is already intentionally stubbed

The current compatibility layer is intentionally minimal and ASCII-safe:

- locale behavior is forced into a C/UTF-8-compatible baseline
- multibyte helpers treat strings as byte-oriented for the largely ASCII coreutils subset
- filesystem semantics are bounded to the NexsOS1 VFS model
- system-level callers are mapped to `nxexec` / VFS / capability-aware paths rather than to Linux host libc assumptions

This is correct for the first phase, but it must be documented and audited per utility family. Programs that require `fts`, full `stat`, ACL semantics, `chroot`, `cp -a`, `ln -s`, `ls`, `df`, `du`, `sort`, `split`, `date`, or full file-tree traversal need to be checked against the new NexsOS1 VFS and capability layer before being declared stable.

## Remaining work plan

1. Keep the official audit script as the gate for real progress.
2. Fix the portability headers so every missing applet has a clear compatibility contract.
3. Map each remaining missing utility to a real NexsOS1 capability/VFS behavior instead of a Linux-like stub.
4. Re-check the rootfs and only then declare a utility stable.
5. Promote user/system binaries to the real `/sys/bin` execution model via the OS1 executor and path policy.

## Real items to tackle next

The next batch should focus on the families that are structurally missing from the current build and are known to cross the VFS and exec boundary:

- directory traversal and metadata: `ls`, `du`, `df`, `stat`, `dir`, `dircolors`, `vdir`
- copy/move/link: `cp`, `ln`, `mv`, `rm`, `install`, `ginstall`
- archive and checksum: `date`, `dd`, `sum`, `cksum`, `sha*sum`, `md5sum`
- text processing and formatting: `sort`, `split`, `tac`, `ptx`, `numfmt`, `od`, `printf`, `pr`
- privilege and identity: `id`, `groups`, `runcon`, `chcon`, `who`, `users`

These are the next real compatibility targets, not more generic host shims.
