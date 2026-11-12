# Coreutils Portability & Capability Layer per NexsOS1

Questo capability/portability layer si trova in `user/sys/lib/portability/coreutils/` insieme agli altri layer del sistema (`sdl2`, `lua`, `gnulib`).

Consente la compilazione modulare degli strumenti GNU Coreutils (`cat`, `echo`, `true`, `false`, `pwd`, `uname`, `sleep`, `mkdir`, `rmdir`, `unlink`, `sync`, ecc.) su **NexsOS1**, integrandosi con `libgnulib.a` e con le reali syscall e VFS di `os1.h`.
