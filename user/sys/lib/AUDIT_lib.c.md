# Audit di `lib.c` — stato reale vs. note obsolete

Ho incrociato ogni "known issue" dichiarato nell'header originale di `lib.c` con
(a) il codice sotto quella stessa nota, e (b) il resto del bundle
(`NexsOS1_full4ai.txt`: `kernel/lib/registry.c`, `kernel/core/syscall_dispatch.c`,
`include/abi/*.h`, `Makefile`). Risultato: **3 note su 9 sono ancora vere,
6 erano documentazione non aggiornata rispetto a codice già corretto.**

## Aperte per davvero (confermate nel bundle)

| ID | Verificato dove | Perché non si chiude dentro `lib.c` |
|---|---|---|
| **USR-LIB-01** | `lib.c:729-730` fa ancora `#include "../../kernel/lib/string.c"` e `.../vsnprintf.c` | È un problema architetturale (due alberi sorgente che condividono lo stesso file), non sistemabile con un `#include` diverso. Serve un terzo percorso condiviso (`lib/common/…`) linkato da entrambi i lati — tocca `kernel/lib/` e il Makefile, fuori dal perimetro di questo file. |
| **USR-BLOAT-01** | `Makefile` (`USER_LIB_O`) linka `lib.o` **intero** in ogni ELF (`user/bin/counter.c` misura ~503KB, ~52KB di stb_image/stb_easy_form inutilizzato) | GCC mette già ogni funzione nella sua sezione con `-ffunction-sections`; il codice C non serve a nulla finché il link non chiede `--gc-sections`. È un flag di build, non una riga di `lib.c`. |
| **USR-BLOAT-02** | Stesso Makefile: nessun `-Wl,--gc-sections`, nessuno step di `strip` | Idem: fix è nel Makefile (vedi patch proposta sotto). |

## Chiuse, ma la nota nell'header mentiva (o era rimasta indietro)

| ID | Nota originale diceva | Codice reale (verificato) |
|---|---|---|
| USR-LIB-04 | "system/getenv sono no-op; atof tronca i decimali via `(double)atoi()`" | `system()` (riga ~2176) lancia davvero `nxshell -c` e aspetta l'exit status; `getenv/setenv/unsetenv` (riga ~2371-2468) sono una vera personalità POSIX sopra un registry `sys.proc.<pid>.env.*`; `atof()` è `strtod()`, parsing IEEE-754 vero. |
| USR-LIB-05 | "vfprintf ignora lo stream, scrive sempre su fd 1" | `vfprintf()` (riga ~2714) passa da `fwrite(buf, 1, len, stream)` — rispetta davvero `stream`. Resta un limite reale ma diverso: stdin/stdout/stderr condividono UN SOLO oggetto CONSOLE nel kernel, quindi stdout e stderr sono visivamente indistinguibili — non è un bug di libc, è una proprietà del kernel. |
| USR-SEC-01 | "registry_read/write senza autenticazione" | `SYS_REGISTRY` in scrittura è gated da `CAP_REG_WRITE` + owner_pid first-writer-wins in `kernel/lib/registry.c`. Il controllo è (correttamente) nel kernel, non in userland — un ricontrollo lato libc sarebbe scenografia, non difesa in profondità, perché lo userland non è il confine di fiducia. |
| USR-SEC-02 | "send()/kill_process() accettano PID arbitrari senza controllo" | `SYS_KILL` è gated da `process_kill_allowed()` (CAP_IPC_ANY o parente/antenato) in `kernel/core/syscall_dispatch.c`; `SYS_SEND` è gated da `CAP_IPC_ANY` per i non-parenti. Stesso discorso: enforcement corretto è a livello kernel. |
| USR-LIB-02 / USR-LIB-03 | — | Già marcate "fixed" nell'originale; confermato nel codice. |

**Conseguenza pratica:** la parte di lavoro con più valore reale non è "riscrivere
codice che sembra rotto" — è già a posto — ma **allineare la documentazione alla
realtà** (fatto in questo primo passaggio) e chiudere le 3 voci genuinamente
aperte, che sono tutte e tre fuori da `lib.c` stesso (Makefile + separazione
kernel/userland).

## Patch Makefile proposta per USR-BLOAT-01/02 (da rivedere, non applicata)

```diff
 USER_CFLAGS = $(COMMON_FLAGS) $(ARCH_CFLAGS) $(INCLUDE)
+USER_CFLAGS += -ffunction-sections -fdata-sections
 USER_CFLAGS += -Wno-error

+# BUILD=release droppa i simboli di debug e attiva --gc-sections al link;
+# BUILD=debug (default) resta invariato per non rompere gdb/qemu -s.
+BUILD ?= debug
+ifeq ($(BUILD),release)
+LDFLAGS_USER_EXTRA = -Wl,--gc-sections -s
+else
+LDFLAGS_USER_EXTRA =
+endif
```

e nella regola di link di ogni `%.elf` utente aggiungere `$(LDFLAGS_USER_EXTRA)`
prima di `-o $@`. Non l'ho applicata perché tocca il Makefile condiviso e non
l'hai chiesto esplicitamente — la lascio qui come diff pronta da rivedere.

## Cosa ho già modificato in `lib.c` (Modulo 1 e 2)

1. Header riscritto: le note sopra, con riferimenti a riga verificati.
2. Aggiunta la policy di conformità NASA/JPL "Power of Ten" (R1-R10), applicata
   per ora ai moduli 1 (`errno`/`OS1_report_error`) e 2 (syscall wrapper /
   controllo processi).
3. Macro `nx_assert()` (R5): asserzioni runtime attive quando `NX_STRICT` è
   definito (già sempre vero, `COMMON_FLAGS` lo definisce) — a differenza di
   un `assert()` che aborta e basta, questa passa prima dalla stessa via di
   notifica (`OS1_report_error`) di ogni altro errore hard, così un'invariante
   violata è visibile allo stesso modo di un fault I/O.
4. R7 (nessun valore di ritorno ignorato senza motivo dichiarato): applicato a
   `OS1low_process_wait_status` e `os1_process_ctl` — `sprintf()` ora controlla
   la lunghezza scritta invece di fidarsi del buffer a occhio, `OS1low_handle_close()`
   ha un `(void)` esplicito con commento sul perché ignorarlo è corretto lì.

Nessuna funzionalità è cambiata: stesso comportamento, stesse firme, solo
verifiche aggiuntive (attive solo in build `NX_STRICT`, cioè sempre secondo
`COMMON_FLAGS` attuale) e documentazione corretta.

## Errata corrige (dopo il tuo `make run` reale)

Il primo passaggio non compilava — grazie per l'output reale, senza quello questi
tre bug sarebbero rimasti. Correzioni applicate, tutte verificate contro il
Makefile completo (non più assunte):

1. **`"/*" within comment` (x2)** — nel testo dell'header avevo scritto un
   `/* ... */` di esempio dentro un commento già aperto (nidificazione, non
   supportata in C) e la parola `user/*.elf`, dove `/*` è letteralmente la
   stessa sequenza di apertura commento. Riformulato entrambi senza `/*`
   letterale. Ho poi scansionato l'intero file con un piccolo state-machine
   (rispetta stringhe/char literal, non un grep) per essere sicuro che non
   ce ne fossero altri: zero trovati.
2. **`unused variable 'idlen'`** — causato da un'assunzione sbagliata: avevo
   scritto che `NX_STRICT` fosse "sempre definito da COMMON_FLAGS". Rileggendo
   il Makefile per intero, **non è così**: `NX_STRICT` è opt-in
   (`make NX_STRICT=1`), esattamente lo stesso interruttore che
   `kernel/nx_contract.h` usava (e poi ha reso sempre-attivo) per
   `NX_MUST_USE`. Con `NX_STRICT` spento (il caso normale), `nx_assert()`
   collassava a `((void)0)`, lasciando `idlen` assegnato ma mai letto sotto
   `-Werror=unused-variable`. Fix: il ramo "off" ora è `((void)(cond))` —
   valuta e scarta l'espressione (nessun costo osservabile, nessun side
   effect visto che una condizione di assert non deve averne), il che conta
   come "uso" per il compilatore e chiude il warning in entrambe le
   modalità.
3. **Scoperta collaterale utile**: il progetto ha già una propria convenzione
   reale per "nessun valore di ritorno ignorato senza motivo" —
   `NX_MUST_USE`/`NX_DISCARD(expr, "perché")` in `kernel/nx_contract.h`, con
   tanto di nota esplicita che **un cast `(void)` nudo NON sopprime
   `warn_unused_result` sotto GCC**. Non è ancora esposta in userland
   (`include/api/os1.h` dichiara `OS1low_handle_close` senza `NX_MUST_USE`,
   quindi il mio `(void)OS1low_handle_close(...)` non genera un warning
   oggi), ma è il meccanismo giusto da riusare — non da reinventare — se in
   un prossimo modulo `NX_MUST_USE` arriva anche lato userland. Lo lascio
   annotato qui per non riscoprirlo da zero al modulo 3.


1. **Ambiente & registry** (`OS1_env_*`, `getenv`/`setenv`, `OS1_registry_*`) — già solido, serve solo R5/R7/banner.
2. **Stdio bufferizzato** (`fopen`/`fread`/`fwrite`/`fseek`/`fflush`) — qui ci sono i loop con bound da rendere espliciti (R2) e più asserzioni (R5) sui buffer.
3. **printf/scanf/vsnprintf/sscanf** — modulo grande, il più delicato per R4 (funzioni sotto le ~60 righe).
4. **Grafica / `graphics_load_image` (stb_image)** — qui applico anche l'estrazione in TU separata propedeutica al fix Makefile.
5. **Shim POSIX finali** (pwd/grp, sysconf, utime*, ecc.) — molti sono già stub onesti e dichiarati tali; per quelli valuto singolarmente se "spostabili in userland" (es. un futuro `nxid_srv` per uid/gid, sul modello di `nxexec`) o da lasciare no-op documentato.
