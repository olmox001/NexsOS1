# Batch 2026-07-16 — libc/filesystem stabilization + mkdir/rmdir + capability A2

Branch: `comprehensive-review-sdl` · Verifica: build 2 arch pulite; runtime
aarch64 verificato per rename-crash (sparito) e `mkdir`/`ls` (funzionanti).

Convenzione: [K] kernel · [L] libc/userland lib · [A] app · [H] header/ABI ·
[D] doc. Le voci marcate (maintainer) sono modifiche applicate direttamente
dal maintainer in questo stesso blocco.

## Correzioni di crash/blocco (layer condivisi)

- [K] `kernel/lib/vsnprintf.c` — supporto C99 `*` per width e precision
  (`%*d`, `%.*s`, negativi gestiti). Root cause del crash rename di nxfilem:
  `%.*s` non consumava l'int → il `%s` successivo dereferenziava `dir_len`
  (0x11) come puntatore → Data Abort EL0. Fix condivisa kernel+userland
  (stesso TU). Header doc aggiornato.
- [L] `user/sys/lib/lib.c` — stdio write-buffering per stream posizionali:
  `FILE.wbuf[4096]` + `wcount`, coalescing in `fwrite`, flush su
  `fflush`/`fclose`/`fseek` e prima di ogni `fread`; console (fd 0/1/2)
  invariata e non bufferizzata. Causa del "freeze" save di doom rimossa
  (una syscall per fwrite → una per 4 KiB).
- [L] `user/sys/lib/lib.c` — euristica `(size_t)fp > 10` sostituita con
  NULL-check corretti in fclose/fread/fseek/ftell/clearerr (USR-LIB-02).
- [H] `include/api/stdio.h` — `FILE` esteso con `wcount`/`wbuf`,
  `FILE_WBUF_SIZE 4096` (block-aligned con ext4).

## Directory end-to-end (mkdir/rmdir)

- [K] `kernel/fs/ext4.c` — `ext4_create` supporta `VFS_TYPE_DIR`: inode
  `S_IFDIR|0755`, blocco dati seminato con `.`/`..` (regola last-entry slack
  di `ext4_dir_insert`), mappatura legacy `i_block[0]`, `i_links_count`
  parent aggiornato, cleanup completo su ogni errore.
- [K] `kernel/fs/ext4.c` — `ext4_unlink` rimuove directory VUOTE:
  `ext4_dir_is_empty` (solo `.`/`..` vivi), rifiuto stile ENOTEMPTY,
  guardia anti `.`/`..`, decremento link parent. File regolari invariati.
- [K] `kernel/core/syscall_dispatch.c` — nuovo case `SYS_MKDIR`: copy path →
  resolve → `vfs_write_allowed` (stesso seam di UNLINK/FILE_WRITE, S-ALIGN
  F6) → `vfs_create(path, VFS_TYPE_DIR)`.
- [H] `include/api/syscall_nums.h` — `SYS_MKDIR 260`.
- [H] `include/api/os1.h` — `extern int _sys_mkdir(const char*)`.
- [L] `user/arch/aarch64/syscall.S`, `user/arch/amd64/syscall.S` — stub
  `_sys_mkdir`.
- [L] `user/sys/lib/lib.c` — `mkdir()` reale (era no-op che riportava 0).
- [A] `user/sys/bin/nxshell.c` — builtin `mkdir <path>` + riga di help.
- [A] `user/sys/bin/nxfilem/fileops.c` — `fm_create_folder` usa `mkdir()`
  (era "not supported") + refresh.

## POSIX/libc: completamento stub sul modello shell

- [K] (maintainer) `kernel/core/syscall_dispatch.c` — `SYS_OPEN` onora
  `O_CREAT`/`O_TRUNC`/`O_APPEND` (create/truncate via seam
  `vfs_write_allowed` + `vfs_create`); `SYS_FILE_WRITE` size==0 ritorna 0
  (idioma create/truncate-empty, niente kmalloc(0)).
- [H] (maintainer) `include/api/fcntl.h`, `include/api/posix_types.h` —
  flag `O_TRUNC`/`O_EXCL` aggiunti, valori allineati kernel/userland.
- [L] (maintainer) `user/sys/lib/lib.c` — `rename()` reale (size-probe +
  copia + unlink, modello shell `mv` testato), `remove()` → unlink con
  errno; `fopen("w")` truncate eager, `fopen("a")` create-if-missing +
  pos=EOF; errno preservato sui path di errore; `open()` con `O_APPEND`
  best-effort lseek EOF.
- [L] `user/sys/lib/lib.c` — `stat()` riporta `S_IFDIR` via probe
  `list_dir` (ext4_list ritorna -2 sui non-dir: verdetto affidabile);
  file regolari invariati (size + `S_IFREG`).
- [K] (maintainer) `kernel/fs/vfs.c` — `vfs_resolve_path` riassembla il
  path canonico direttamente in `out[]` in un passaggio bounded (rimosso
  O(n²) strncat + copia ridondante).

## Capability (A2)

- [K] `kernel/core/object.c` — `sys_object_read(FILE)` ri-sincronizza il
  nodo cachato dal path prima della lettura (simmetrico alla write che già
  rinfrescava dopo): un handle READ longevo vede i dati appesi da altri;
  fallback al nodo cachato se il path è sparito.

## Altro (maintainer, stesso blocco)

- [A] `user/sys/bin/nxexec.{c,h}` — helper di lancio stratificato raffinato
  (foreground/detached, finestre stabili, modello #193).
- [A] `user/sys/bin/nxassoc.h` (nuovo), `nxicon.h`, `nxres.h`,
  `user/sys/bin/nxfilem/*` — riscrittura UI/associazioni file manager.
- [L] `user/sys/lib/malloc.c`, `user/sys/lib/portability/lua/*`,
  `user/sys/bin/nxshell.c` — rifiniture.
- [B] `Makefile` — aggiornamenti build.

## Documentazione

- [D] `docs/ASTRA.md` — nuovo §7.12 (questo batch, con stato runtime).
- [D] `docs/PIANO-LIBC-ASTRA-2026-07-16.md` — piano completo: microfasi del
  blocco corrente, chiusura capability, macropiano ristrutturazione userland
  (split OS1lib_OS1/POSIX/LIBC, servizi, nxauth, formato `.X`), domande
  aperte per il maintainer.

## Verifiche eseguite

- Build pulite `make all` (aarch64) e `make all ARCH=amd64` ad ogni passo.
- Runtime aarch64 (`make run`): rename nxfilem senza fault (prima: Data
  Abort 0x11 riprodotto e catturato dal seriale); `mkdir ciao` + `ls`
  mostrano la directory; doom salva la config e scrive `temp.dsg` (il
  passaggio finale del save richiede `rename()`, ora reale — riverifica
  runtime in M7).

## Rimasti aperti (pianificati)

- Riverifica runtime: rmdir, save doom end-to-end, entrambe le arch (M7).
- `OS1_fs_write` ancora ambient → chiusura capability C1–C4 (piano Parte 2).
- Presentazione `/reg` in nxfilem (icone fuorvianti) — solo userland.
- Costo `ext4_find_ino` per write (walk completo) — ottimizzazione futura.
