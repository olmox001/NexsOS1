# Gnulib Capability & Portability Overlay per NexsOS1

L'overlay integra il sottoprogetto fissato in `user/lib/gnulib` con l'SDK in
`system/lib`, senza modificare le sorgenti upstream.

## Architettura e Layering

1. **Configurazione forzata (`gnulib_config_nexsos.h`)**:
   - Fornita via `-include user/portability/gnulib/gnulib_config_nexsos.h`.
   - Fissa il contesto di cross-compilazione. HAVE_* indica le scelte del
     porting e la presenza di simboli, non una certificazione POSIX.

2. **OS1 Real Glue Layer (`gnulib_os1_glue.c` / `gnulib_os1_glue.h`)**:
   - Collega le API di Gnulib al runtime `os1.h`, alle reali syscall di NexsOS1 (`_sys_*`), al VFS (ext4, procfs, regfs) e al capability/process engine (`OS1low_process_*`).
   - Sono ancora presenti stub, inclusi quelli FTS che restituiscono ENOSYS.
     Alcune API libc, come futimens, dichiarano successo senza persistenza dei
     timestamp. Queste lacune sono aperte e non vanno nascoste nei test.

3. **Integrazione Build (`overlay.mk`)**:
   - Compila la suite di moduli fondamentali Gnulib in un archivio statico `libgnulib.a`.
   - Linkato con la libreria in `system/lib` e gli stub syscall di architettura.
   - HAVE_FUTIMENS=1 seleziona il simbolo SDK. Il fallback upstream futimens.c
     non è incluso sotto lo stesso nome: chiamerebbe fdutimens, che con quella
     configurazione richiama futimens, esaurendo lo stack.
   - L'archivio viene ricreato in un file temporaneo e poi sostituito. Un semplice
     aggiornamento ar rcs conserverebbe membri rimossi dalla lista dei sorgenti.

Il test `make test-gnulib-archive` esercita questa rimozione sul recipe reale.
La verifica guest deve includere touch e truncate; un esito positivo di touch
non certifica la scrittura dei timestamp. La revisione FTS richiede il vero
header e le dipendenze descriptor-relative del modulo, non un secondo walker.
