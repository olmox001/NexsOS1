# PIANO — Stabilizzazione libc/capability + Ristrutturazione userland ASTRA

Data: 2026-07-16 · Branch: `comprehensive-review-sdl`
Metodo: doc come riferimento concettuale (ASTRA.md), codice come stato reale.
Verifica: `make run` e `make run ARCH=amd64` a fine di ogni microfase, con
checkpoint del maintainer.

---

## Parte 1 — Blocco corrente (in corso) — stato microfasi

Origine: crash rename di nxfilem, freeze save di doom, gap `mkdir`.
Regola del blocco: correggere i LAYER (kernel, libc) in modo standard e
app-agnostico; le app (nxfilem, doom) si conformano da sole. Nessun codice
speciale per doom.

| ID | Microfase | Stato | Verifica |
|----|-----------|-------|----------|
| M1 | `vsnprintf`: supporto C99 `*` (width/precision dinamici). Root cause del crash rename (`%.*s` desincronizzava i va_arg → deref di `dir_len` come puntatore). | ✅ fatto | rename OK a runtime (aarch64), fault sparito |
| M2 | stdio libc: write-buffering per stream posizionali (`FILE.wbuf`, flush su fflush/fclose/fseek/fread) + rimozione euristica `(size_t)fp>10` → NULL check. | ✅ fatto | doom non si blocca più al save; config scritta |
| M3 | Capability A2: `sys_object_read` ri-sincronizza il nodo dal path (simmetria read/write, niente size stale su handle READ longevi). | ✅ fatto | build ok, nessuna regressione |
| M4 | `SYS_MKDIR` (260): `ext4_create` supporta `VFS_TYPE_DIR` (blocco `.`/`..` seminato, link count parent), gate `vfs_write_allowed` (stesso seam di UNLINK/WRITE), stub asm 2 arch, `mkdir()` libc, builtin `mkdir` in nxshell, "New folder" nxfilem. | ✅ fatto | `mkdir ciao` + `ls` verificato a runtime |
| M5 | `ext4_unlink` rimuove directory VUOTE (semantica rmdir/ENOTEMPTY, guard `.`/`..`, decremento link parent). | ✅ fatto | build ok · runtime da riverificare |
| M6 | Completamento stub libc sul modello shell (testato): `remove()`→unlink, `rename()`→copia+unlink (implementati dal maintainer), `fopen` "w"/"a" con truncate/append reali (maintainer), `O_CREAT/O_TRUNC/O_APPEND` onorati da `SYS_OPEN` (maintainer), `stat()` riporta `S_IFDIR` via probe `list_dir`. | ✅ fatto | build da rifare · runtime da verificare |
| M7 | Verifica runtime completa (2 arch): ciclo mkdir/ls/rm, save doom end-to-end (ora `rename()` è reale), rename nxfilem, /reg pulito. | ⏳ in corso | — |
| M8 | Chiusura blocco: aggiornamento ASTRA.md §7, file changelog, commit ordinati. | ⏳ in corso | — |

Problemi noti rimasti nel blocco (non bloccanti, da pianificare):
- `/reg` in nxfilem mostra icone/classificazioni fuorvianti (presentazione
  userland; il VFS kernel è corretto e condiviso con la shell).
- `ext4_find_ino` per ogni write (walk completo): amplificatore di costo,
  candidato a cache/ottimizzazione (vedi Parte 3).

## Parte 2 — Chiusura capability (dopo M8, stesso blocco)

Da ASTRA §7.11 (call-surface refactor) — obiettivo: nessun percorso di
scrittura ambient residuo dove esiste l'equivalente capability.

| ID | Microfase | Note |
|----|-----------|------|
| C1 | `handle_create(OS1_NS_FS)` con semantica `O_CREAT`: creare-il-file-poi-handle in un solo punto (il gate resta `vfs_write_allowed`). Sblocca il routing capability di `OS1_fs_write` (oggi ambient, NOTE M4.5-FS-WRITE in lib.c). | kernel/core/object.c |
| C2 | `OS1_fs_write` userland → percorso capability (handle WRITE + `OS1_object_write` + SEEK), mantenendo il size-probe ambient per compat. | user/sys/lib |
| C3 | Test `cap*` estesi: create-via-handle, write-via-handle, mkdir gate. | user/bin |
| C4 | Verifica 2 arch + checkpoint. | — |

## Parte 3 — MACROPIANO: ristrutturazione userland secondo ASTRA (nuovo blocco, da confermare)

Direttive del maintainer (2026-07-16) + ASTRA §6.1/6.4/6.8. Da raffinare con
domande/risposte prima dell'esecuzione (vedi Parte 4).

### 3.1 Divisione ABI ufficiale vs compatibilità

Principio ASTRA: POSIX e libc stanno in userland; tutto ciò che non è OS1 è
layer di compatibilità, usabile uniformemente.

- Split di `user/sys/lib/lib.c` in tre unità con API dedicate:
  - `user/sys/lib/OS1lib_OS1/OS1lib_OS1.c` + `OS1.h` — superficie nativa
    (`OS1_*`, `OS1low_*`): l'ABI ufficiale.
  - `user/sys/lib/OS1lib_POSIX/OS1lib_POSIX.c` + `POSIX.h` — layer POSIX
    (fd, open/read/write, stat, dirent, …) costruito SOPRA OS1.h.
  - `user/sys/lib/OS1lib_LIBC/OS1lib_LIBC.c` + `LIBC.h` — libc C (stdio,
    stdlib, string, …) costruita sopra OS1/POSIX. Diventa la libc ufficiale
    di NexsOS1 basata sulla nostra API.
- Standardizzazione header: spostare `include/api/*` in
  `user/sys/lib/include/api/` (aggiornare Makefile, inclusi i target che
  copiano gli header nel rootfs/VFS).
- Altre librerie principali sullo stesso modello (secondo ASTRA):
  `GRAPHICS`, `TERM`, `SERVICE`, `I/O`, e altre da censire.
- Portability layer esistenti (`user/sys/lib/portability/*`: lua, sdl, …):
  trattati come librerie principali, ma le librerie PORTATE (SDL2, Lua)
  vivono modularmente SOPRA di essi come layer secondari (alto livello,
  meno privilegio). SDL2 è l'esempio di riferimento.

### 3.2 Servizi (SRL → `user/sys/services`)

- Gli header `nx*` (nxexec.h, nxproc.h, nxres.h, nxassoc.h, …) sono le API
  dei servizi: oggi in `user/sys/bin`, destinazione `user/sys/services`.
- Le app linkabili eseguiranno operazioni TRAMITE servizi avviati in
  userland (modello nxexec, raffinato): libreria → IPC/capability → demone.

### 3.3 Modello di esecuzione privilegiata

- Esecuzione diretta di programmi riservata ai processi privilegiati
  (machine: oggi `nxinit`).
- `nxexec` → demone privilegiato di esecuzione: per processi che richiedono
  più autorità spawna un pannello di conferma password.
- `nxauth`: nuovo servizio di autenticazione — l'utente lo usa per elevare
  a root processi che mappano capability (machine NON vi ha accesso).
- Obiettivo: `nxinit` e `nxauth` unici processi privilegiati.
- La logica process-child/process-kill oggi raffinata in userland va
  portata/consolidata a livello kernel (o comunque il livello primario che
  permette exec diretta resta solo ai privilegiati), gestendo il vecchio
  metodo con capability; `nxinit` mantiene una logica più programmabile.
- Riferimento: docs/PROCESS-KILL-MODEL.md (modello finestre/figli attuale).

### 3.4 Formato eseguibili

- Estensione standard per gli eseguibili (proposta maintainer: `.X`).
- Correggere TUTTI i punti hardcoded che trattano come eseguibili i file
  senza estensione e i file in `/sys/bin` e `/bin` (level_for_path,
  nxexec/nxshell/nxlauncher resolution, mkdisk, …).
- Candidata a un passaggio dedicato con agente (censimento + patch).

### 3.5 Aggancio ai piani principali (M4, M5, B, C — ASTRA)

Temi: filesystem, object, syscall, registry, servizi, librerie userland.
- Divisione kernel/userland delle librerie (oggi vsnprintf/string/math
  kernel sono #include-ati in userland — USR-LIB-01, da sanare con lo split
  3.1).
- Eliminazione degli strati inutili (doppi wrapper, alias legacy).
- Collegamento dei moduli (ogni modulo consuma l'API del layer sotto, mai
  salti di livello).
- Kernel: prosecuzione B5 (split srl/hal nel source tree), C (object
  manager completo, `OBJ_TYPE_PORT`, `OS1low_vm_*`).

### Ordine proposto del macroblocco (bozza, da confermare)

1. 3.1 split librerie + header (fondazione, tutto il resto vi si appoggia)
2. 3.4 formato eseguibili `.X` (tocca pochi punti, sblocca 3.3)
3. 3.2 servizi in `user/sys/services`
4. 3.3 nxexec demone + nxauth + kill-model in kernel
5. 3.5 rifiniture strutturali continue

## Parte 4 — Domande aperte per il maintainer

1. **Naming**: confermi `OS1lib_OS1/POSIX/LIBC` e header `OS1.h/POSIX.h/LIBC.h`
   esattamente con queste maiuscole? Le app includeranno `<POSIX.h>` o
   restano gli include POSIX classici (`<stdio.h>` → forward a LIBC.h)?
2. **Compat include**: gli header classici (`stdio.h`, `stdlib.h`, …)
   restano come shim che includono i nuovi, così le app esistenti compilano
   senza modifiche?
3. **Librerie aggiuntive**: per GRAPHICS/TERM/SERVICE/IO — partiamo dal
   censimento di ciò che è già in lib.c (graphics_*, window_*, term…) o hai
   già una partizione precisa in mente?
4. **`.X`**: rinominiamo anche i binari esistenti nel rootfs (mkdisk) nello
   stesso passaggio, o prima solo il supporto e poi la migrazione?
5. **nxauth**: la password è per-utente (modello nxperm/login futuro) o una
   password di sistema unica per ora?
6. **Kill-model in kernel**: portare la logica child/kill in kernel significa
   nuove syscall/verbi OBJ_CTL o estendere `process_kill_subtree` con policy
   da capability? Preferenza?
7. **Timeline commit**: un commit per microfase come in questo blocco, o
   commit più granulari?
