# Gnulib Capability & Portability Overlay per NexsOS1

Questo capability/portability layer consente la compilazione e l'integrazione di GNU Gnulib su **NexsOS1** senza modificare il submodule upstream `user/sys/lib/gnulib`.

## Architettura e Layering

1. **Configurazione forzata (`gnulib_config_nexsos.h`)**:
   - Fornita via `-include user/sys/lib/portability/gnulib/gnulib_config_nexsos.h`.
   - Neutralizza le assunzioni host di autoconf/gnulib e dichiara la conformità al modello standard C99/C11 e POSIX di NexsOS1.

2. **OS1 Real Glue Layer (`gnulib_os1_glue.c` / `gnulib_os1_glue.h`)**:
   - Collega le API di Gnulib al runtime `os1.h`, alle reali syscall di NexsOS1 (`_sys_*`), al VFS (ext4, procfs, regfs) e al capability/process engine (`OS1low_process_*`).
   - Nessun mock o fake stub: tutte le operazioni sono connesse direttamente ai descrittori e servizi del sistema operativo.

3. **Integrazione Build (`overlay.mk`)**:
   - Compila la suite di moduli fondamentali Gnulib in un archivio statico `libgnulib.a`.
   - Linkato con `user/sys/lib/lib.c`, `user/sys/lib/malloc.c`, `user/sys/lib/math.c` e `syscall.o`.
