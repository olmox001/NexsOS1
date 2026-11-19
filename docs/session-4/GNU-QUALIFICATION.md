# Official GNU conformance vs. SDK tests

Official GNU source trees exist in user/LIBEXTERNAL/gnulib/tests and user/BINEXTERNAL/coreutils/tests. Existing guest gnulibtest and coreutilstest are locally authored integration tests. The Makefile test-gnulib-archive target is an archive test, not upstream GNU make check; test logs must label those scopes honestly.

Canonical porting boundary is system/lib/portability/gnulib and system/lib/portability/coreutils, plus normal SDK object clients. Never patch vendored GNU algorithms solely to mask a missing NexsOS1 contract. Keep all ABI adapters in SDK and userland; no POSIX in kernel.

Required upstream runner:
1. Inventory upstream test sources, versions, fixtures and host-only requirements.
2. Compile unchanged eligible upstream C tests using the target compiler, actual SDK headers and built Gnulib archive, installed in dedicated guest test catalog.
3. Run via nxadb against both QEMU images and record original expected exit status, output, timeout and crashes. Preserve PASS, FAIL, SKIP and XFAIL separately.
4. For upstream Coreutils Perl/shell harnesses, create a guest-command bridge executing target binaries rather than host macOS binaries, with isolated fixtures.
5. Make official test lane mandatory alongside existing guest suite and Power12 strict. Do not count an inventory scan, host-library execution or compile-only target as a guest test pass.

Concrete open cases: nice requires real process replacement and scheduler priority; current execvp returns ENOTSUP. The null provider and regular-file descriptor duplication are implemented. nohup now redirects its output and reaches the missing exec operation. POSIX identity getters still report zero. Ownership mutations return ENOTSUP pending a real principal and inode ownership model. Upstream tests are not declared passed yet.
