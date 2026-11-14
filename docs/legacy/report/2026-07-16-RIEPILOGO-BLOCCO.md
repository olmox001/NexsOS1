# Riepilogo blocco 2026-07-16 — stabilizzazione libc/fs + mkdir + capability A2

Branch `comprehensive-review-sdl` · 6 commit superproject + 2 submodule ·
working tree pulito. Dettaglio per-file: `docs/report/2026-07-16-libc-fs-batch.md`.
Piano completo e prossimi blocchi: `docs/PIANO-LIBC-ASTRA-2026-07-16.md`.

## Problemi di partenza → root cause → fix

| Sintomo | Root cause reale | Fix | Verifica |
|---|---|---|---|
| nxfilem crash al rename | `vsnprintf` senza supporto C99 `*`: `%.*s` desincronizzava i va_arg, il `%s` successivo dereferenziava `dir_len` come puntatore (Data Abort 0x11, catturato dal seriale) | supporto `*` width/precision in `kernel/lib/vsnprintf.c` (condiviso kernel+userland) | ✅ runtime aarch64: rename pulito |
| doom "bloccato" al save | stdio libc non bufferizzato: una `SYS_FILE_WRITE` (con walk ext4 completo) per OGNI `fwrite` di 1 byte | write-buffer 4 KiB per stream posizionali in libc | ✅ runtime: config salvata, niente freeze |
| save doom non finalizzato (nessun errore) | `rename()`/`remove()` erano stub no-op che riportavano successo; mancava `mkdir` per `.savegame` | `SYS_MKDIR` + `mkdir()` reali; `rename()`/`remove()` reali (modello shell); `fopen("w"/"a")` truncate/append; `SYS_OPEN` onora `O_CREAT/O_TRUNC/O_APPEND`; `ext4_write` tronca su write from-start | ⏳ M7: riverifica save end-to-end |
| `rm` su directory rifiutato | `ext4_unlink` non supportava directory | rimozione directory VUOTE (stile ENOTEMPTY, guard `.`/`..`, link parent) | ⏳ M7 |
| divergenza capability (A2, ASTRA) | `sys_object_read` serviva il nodo cachato all'acquisizione (size stale) | re-sync dal path prima di ogni read (simmetrico alla write) | ✅ build, nessuna regressione |

Ipotesi scartate con evidenza: allocatore malloc (sbrk kernel è contiguo →
coalescing corretto); layer capability kernel per il crash (mv shell usa lo
stesso percorso e funzionava); VFS kernel per il "/reg sporco" (list_dir è
condiviso con la shell — resta da sistemare solo la presentazione nxfilem).

## Commit

1. `14daf28` vsnprintf: C99 `*` width/precision (+ conversioni float userland)
2. `a19f1f8` fs: directory end-to-end (SYS_MKDIR, rmdir vuote) + O_CREAT/O_TRUNC/O_APPEND
3. `f0bd300` object: A2 re-sync nodo su read capability
4. `ea1337f` libc: buffering stdio + stub reali (rename/remove/mkdir/stat/fopen)
5. `353a9ad` apps: nxfilem+nxassoc, nxexec, shell mkdir, kilo bump
6. `36329c6` docs: ASTRA §7.12, piano, changelog; mkdisk 432 MiB
- submodule kilo `780958a` (guardie OOM), lua `97aa15e` (luaconf port)

## Stato verifica (M7)

- aarch64: boot pulito, 0 fault; rename e `mkdir`+`ls` verificati nelle run
  precedenti. Ciclo completo (rm dir, doom save+load, kilo) da esercitare.
- amd64: build pulita; run di verifica in corso.

## Prossimi passi proposti (in attesa di conferma)

1. Chiusura capability C1–C4 (piano Parte 2): `O_CREAT` in `handle_create`,
   `OS1_fs_write` via capability, test `cap*` estesi.
2. Macroblocco ristrutturazione userland (piano Parte 3): split
   OS1lib_OS1/POSIX/LIBC, header in `user/sys/lib/include/api`, servizi in
   `user/sys/services`, nxexec demone + nxauth, formato eseguibili `.X`.
3. Domande aperte in piano Parte 4 (naming, shim include, nxauth, kill-model
   in kernel, granularità commit) — da rispondere prima di partire.
