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

## Parte 2 — Chiusura capability ✅ COMPLETATA (2026-07-17)

Da ASTRA §7.11 (call-surface refactor) — obiettivo: nessun percorso di
scrittura ambient residuo dove esiste l'equivalente capability.

| ID | Microfase | Stato |
|----|-----------|-------|
| C1 | `OS1_RIGHT_CREATE` (acquisition-only, strippato dai diritti installati) in `handle_create(OS1_NS_FS)`: path mancante con CREATE+WRITE creato dietro `vfs_write_allowed`; `regfs.create` per uniformità `/reg`. | ✅ commit `0bcc5d8` |
| C2 | `OS1_fs_write` → percorso capability (handle WRITE\|CREATE → SEEK → `object_write` → close), speculare a `OS1_fs_read`; percorso ambient NOTE(M4.5-FS-WRITE) ritirato. | ✅ commit `0bcc5d8` |
| C3 | captest 10–13: create-via-handle (+CREATE strippato), create esplicito, ACL `/sys/bin` all'acquisizione, seam mkdir/rmdir. | ✅ commit `0bcc5d8` |
| C4 | Verifica runtime 2 arch. | ✅ 13/13 PASS su amd64 E aarch64 (maintainer, 2026-07-17), 0 fault |

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

### Decisioni esecutive (2026-07-17, seconda tornata)

- **Mappa split confermata**: OS1lib_OS1 = `OS1_*`/`OS1low_*` + stub `_sys`;
  OS1lib_POSIX = fd/open/read/stat/dirent/unistd/termios/mman;
  OS1lib_LIBC = stdio(FILE)/stdlib/string/ctype/time/math;
  GRAPHICS = graphics/window/font/image; TERM = print/printf_win/ansi;
  SERVICE = header `nx*`; IO = input/eventi.
- **Eliminazione TOTALE degli alias bare-name** (spawn, flush, file_write,
  list_dir, …): solo la nuova nomenclatura sopravvive (piano ASTRA).
- **Kernel libs: fork in userland** — OS1lib_LIBC riceve copie freestanding
  proprie (vsnprintf/string/math); `kernel/lib` diventa kernel-only, non più
  accessibile all'userland (USR-LIB-01 chiuso).
- **Anche il codice portato migra** (doom/lua/sdl) via i compatibility
  layer; ristrutturazione completa dell'userland con strumenti di
  automazione semantica (brew — vedi sotto).
- **`.X` in un passaggio unico** (supporto + rinomina binari + bonifica di
  tutti i path hardcoded; censimento affidabile a un agente dedicato).

### Microfasi esecutive del macroblocco

- **R0 Censimento** (nessuna modifica di codice): R0.1 inventario simboli di
  lib.c → mappa di collocazione per libreria (inclusi gli alias da
  eliminare); R0.2 inventario call-site per app; R0.3 mappa header
  `include/api/*` → `user/sys/lib/include/api/*` + punti Makefile/rootfs.
- **R1 Scheletro + header**: R1.1 cartelle `OS1lib_*` + header nuovi
  (OS1.h/POSIX.h/LIBC.h/GRAPHICS.h/TERM.h/SERVICE.h/IO.h); R1.2 spostamento
  header + Makefile (-I e copia nel rootfs/VFS); R1.3 build verde.
- **R2 Split implementazioni**: R2.1 OS1lib_OS1 (+fork kernel libs);
  R2.2 POSIX; R2.3 LIBC; R2.4 GRAPHICS/TERM/SERVICE/IO; R2.5 rimozione
  lib.c, build verde.
- **R3 Nomenclatura**: R3.1 patch semantiche (coccinelle/comby) che
  eliminano gli alias bare-name in tutte le app native; R3.2 migrazione
  include delle app native ai soli header nuovi.
- **R4 Codice portato**: header classici SOLO dentro
  `user/sys/lib/portability/*`; migrazione automatizzata di doom/lua/sdl.
- **R5 Formato `.X`** (passaggio unico): censimento path exec hardcoded
  (agente dedicato) + mkdisk + loader/level_for_path/nxexec/nxshell/
  nxlauncher.
- **R6 Servizi**: spostamento `nx*` in `user/sys/services`; nxexec demone
  privilegiato + nxauth (password root preset); studio kill-model/exec in
  kernel (doc dedicato prima del codice).

Ogni microfase: build 2 arch + `make run` checkpoint; **solo stage fino a
test superati** (direttiva 2026-07-17).

### Architettura header definitiva (decisione 2026-07-17, terza tornata)

Separazione COMPLETA dei due layer ("il kernel è neutro, l'userland è
neutro"); molte implementazioni header oggi sdoppiate/confuse vanno
ricostruite pulite in questo passaggio:

```
kernel/  (superficie kernel SOLO nel kernel; lib fondamentali in kernel/lib)
   │
   ▼
include/abi/    contratto ABI kernel↔userland, neutro e centralizzato
                (syscall_nums, object/caps/rights, posix_types ABI,
                 sysstats, style_names, font structs, input events)
   │
   ▼
include/api/    SOLO base userland: OS1low_ + OS1_ — API neutra,
                il più sottile possibile
   │
   ▼
user/sys/lib/   OS1lib_OS1 · OS1lib_POSIX · OS1lib_LIBC
                + librerie di compatibilità (portability)
   │
   ▼
librerie aggiuntive: GRAPHICS · TERM · SERVICE · IO (modello librerie
principali) e le librerie PORTATE (SDL2, Lua) sopra i portability layer
```

### Strumenti di automazione (brew)

- `coccinelle` — patch semantiche C (.cocci): rinomine API su call-site,
  eliminazione alias, migrazione firme. Motore principale di R3/R4.
- `comby` — riscritture strutturali semplici (include, pattern testuali
  con struttura): complemento veloce a coccinelle.
- `universal-ctags` — inventario simboli per il censimento R0.

## Parte 4 — Decisioni del maintainer (2026-07-17)

1. **Include model: SOLO header nuovi.** Le app migrano a
   `<OS1.h>/<POSIX.h>/<LIBC.h>` direttamente (niente shim retrocompatibili;
   migrazione big-bang contestuale allo split 3.1).
2. **Ordine: si parte dalla chiusura capability C1–C4** (Parte 2), poi il
   macroblocco 3.x.
3. **nxauth (prima versione): solo utente root con password di preset.**
   Utenti nominali NON in questo blocco. Le modifiche fs restano sulla
   divisione attuale per ora.
4. **Visione VFS per-utente (blocco futuro, modello Linux/Android):**
   - Una partizione per ambiente: root = sd0 (`/dev/sd0`, `/mnt/sd0`) →
     linkata in `/root`, `/bin`, `/sys/library`, `/sys/bin`;
     user1 = sd1 (`/dev/sd1`, `/mnt/sd1`) → linkata in `/usr/user1/home`
     (contenuto attuale di /home; alcune cose come le icone vanno spostate
     nello spazio root), `user1/bin`, `user1/lib`, `user1/apps`, ….
   - Il kernel tratta gli ambienti come chroot: le capability si mappano
     direttamente sulle partizioni.
   - Serve una partizione `/tmp`; layout standard Unix ma sul nostro
     modello (Plan 9-like dove conviene).
   - `OS1lib_LIBC`/`OS1lib_POSIX` devono contenere la MAPPA del VFS →
     inizializzazione del VFS standardizzata a runtime + compatibility
     layer che mappa il VFS Unix classico. Richiede uno studio pragmatico
     del modello prima dell'esecuzione.
5. **Kill-model/exec (da studiare nel blocco insieme ad ASTRA):** la logica
   di base di `nxwins` + process-child di `nxexec` (nuova finestra / nuova
   finestra terminale / terminale corrente se avviato da terminale) va
   STANDARDIZZATA nel kernel mantenendo l'approccio nxexec. Studiare il
   modello attuale e decidere cosa resta in userland e cosa si astrae in
   kernel; verbi `OBJ_CTL` su PROCESS ed estensione di
   `process_kill_subtree` sono entrambi da valutare insieme alla rinomina
   eseguibili (`.X`). Precisazioni rimandate alle prossime fasi.
