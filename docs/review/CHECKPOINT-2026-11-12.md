# Checkpoint completo — 12 novembre 2026

La richiesta successiva del manutentore è salvare e pubblicare insieme tutte
le modifiche staged, unstaged e i nuovi sorgenti, preservando le dipendenze
tra kernel, SDK, servizi, binari e build. Questa richiesta supera il precedente
metodo di stage parziale. Il commit che contiene questo documento è un
checkpoint di sviluppo avviabile verificato su QEMU, non una release qualificata.
I problemi residui non sono chiusi dal salvataggio.

## Risultati finali osservati

`make clean` è stato rieseguito su richiesta. Le immagini ricostruite e
avviate con nxadb raggiungono la shell grafica su AMD64 BIOS e AArch64 UEFI.
La build AMD64 ha inizialmente segnalato una dichiarazione mancante nel nuovo
fdtest; aggiunto unistd.h, la ricompilazione e l'avvio sono riusciti.

| Verifica reale | AMD64 | AArch64 |
| --- | --- | --- |
| Test kernel all'avvio | 32/32 | 33/33 |
| Suite completa iniziale | 17/18 | 17/18 |
| Suite completa ripetuta sulla stessa immagine | 17/18 | 17/18 |
| Coreutils, unico gruppo ancora fallito | 41/50 | 41/50 |
| Capability / libc | 83/83; 29/29 | 83/83; 29/29 |
| Notifiche / PORT / gfx buffer / SIMD / alpha | 14/14; 3/3; 2/2; 1/1; 1/1 | 14/14; 3/3; 2/2; 1/1; 1/1 |
| fdtest dopo la suite ripetuta | 0 errori; fixture 57 byte | 0 errori; fixture 57 byte |
| Stress timer-kill, 10 secondi | 134 casi, 0 errori | 63 casi, 0 errori |
| Stress coda timer | 24 figli, PASS | 24 figli, PASS |
| Probe timer, 160 campioni da 33 ms | 0 errori; massimi 40,816 / 42,566 ms | 0 errori; massimi 125,590 / 119,144 ms |
| Drag finestra e successivo comando shell | RESIZE_ALIVE_AMD64 | RESIZE_ALIVE_AARCH64 |

Le schermate in `evidence/2026-11-12/*-resize.png` sono quelle acquisite
durante la prova nxadb. Mostrano ancora una finestra vuota dopo il drag:
la risposta al comando successivo è verificata dal log, ma il resize e il
ridisegno non sono dichiarati risolti o atomici. Non sono mockup.

Le serie PORT 100 + 100 + 30 per architettura appartengono a iterazioni
successive, documentate nell'addendum: non sono presentate come 230 prove
dell'ultima immagine. La seconda serie non elimina i rallentamenti; la terza
misura ogni scambio e distingue le lunghe fasi di 1024 messaggi dal massimo
singolo intervallo (circa 74 ms AMD64 e 113 ms ARM).

`make check ARCH=amd64` e `ARCH=aarch64` terminano con successo: contratti,
delta Power12 e verifiche NXPAX. Il gate globale Power12 rimane fallito:
ultimo rapporto 5055 riscontri AST e 369 sui limiti, zero sul build audit.
Il delta è verde; non certifica funzioni o file legacy completi. Nessun
`break` nuovo è autorizzato, nemmeno negli switch.

Passano le nove regressioni host aggiunte per getcwd, pubblicazione PMM,
lunghezza file, campionamento timer, creazione owned/controllo, quantum ed
entropia, con ASan/UBSan. Passano le 17 mutazioni dell'auditor e il layout
dei due rootfs: 14 servizi nominati, 8 entrypoint server, 7 moduli SDK.
Le prove HAL host, cross e sanitizer sono registrate separatamente; nessuna
prova su hardware fisico è stata eseguita.

I risultati selezionati e gli hash dei log originali sono conservati in
`evidence/2026-11-12/verification.json`; i log integrali restano in build/runs
e build/runtime. Un ritorno nxadb 1 della suite ripetuta conserva il fallimento
noto di Coreutils; le conclusioni 17/18 vengono lette dal log applicativo.

## Correzioni comprese nel checkpoint

- getcwd nativo: stato corretto nel resolver e copia kernel limitata alla
  stringa con NUL, con EINVAL/ERANGE e sentinelle di regressione.
- PORT: metadati di capability autenticati dal kernel, pulizia comune SDK,
  rifiuto di slot falsi nei servizi e assegnazione dell'owner prima che un
  figlio rapido possa terminare. Creazione preemptibile con controllo
  KILL/STOP rinviato alla fine della pulizia; CONT ordinato sotto lo stesso lock.
- Scheduler: protezione del task ancora corrente prima del salvataggio del
  contesto; propagazione del quantum logico al clockevent anche sul medesimo task.
- PMM: poison completato prima che una pagina diventi allocabile da altre CPU.
- File: scrittura posizionale conserva la coda; truncate esplicito conserva
  posizione/inode, supporta WRITE-only e azzera la crescita. Nessun buffer libc
  proporzionale alla dimensione del file.
- Immagini: allocazione mkdisk che evita i metadati dei gruppi; runtime libgcc
  del compilatore al posto di helper long-double ricorsivi; consumo unico degli
  stati di uscita; tastiera condivisa nell'estensione SDK.
- Notifiche, TTY, temi, servizi, contratti HAL e ISA: incluse tutte le modifiche
  locali collegate. La loro presenza non equivale alla migrazione completa.

## Ripresa obbligatoria e ordine

1. **Timer/IPC:** stabilità della latenza resta aperta; i PASS funzionali non
   stabiliscono un limite temporale. Verificare per CPU code, affinità, quantum,
   IPI e logging con misure per scambio; la telemetria IRQ-off ha campioni
   invalidi e non costituisce una misura completa. Qualificare KILL/STOP/CONT
   durante caricamento reale, pin dei processi per redirezioni FD esterne
   (PROC-REF-01), chiusura degli allegati e attese RPC limitate. Rimane incerto
   l'esito se nxexecd muore dopo lo spawn e prima della risposta.
2. **Coreutils/libc:** integrare l'FTS Gnulib già presente, header e dipendenze
   corretti. Gli stub fts_open ENOSYS sono etichettati OOM da xfts_open: non è
   memoria esaurita dimostrata. Verificare execvp/priorità per nice, /dev/null
   per nohup, utmp per users/who, casi veri symlink/non-symlink per readlink.
   truncate apre con O_NONBLOCK, ancora rifiutato: definire la semantica per
   tipo di oggetto prima di accettare flag. Controllare anche O_NOCTTY/O_APPEND
   sovrapposti in fcntl.h, CLOEXEC, fcntl e chmod/chown che simulano successo.
3. **VFS/PMM:** ext4 conserva i blocchi oltre EOF; recupero transazionale di
   extent/indiretti, concorrenza e fallimenti parziali richiedono revisione.
   Validare overflow di offset prima dei restringimenti nel percorso FILE e
   metadati reali di stat/fstat, senza indebolire i test o ricreare fixture.
4. **Notifiche e servizi:** chiudere lifecycle, backpressure, restart e delega
   capability con prove per servizio. Estensioni/protocolli nella libreria
   system; demoni in user/sys/services; accesso utente in user/sys/bin.
5. **Graphics:** seguire tutte le 115 voci del catalogo vivo. Spostare scena,
   focus, stacking, geometria, decorazioni, testo e composizione in userland.
   Kernel/resources conserva meccanismi surface/buffer e capability, non la
   politica rinominata. Rivedere image.c, buffer_client.c, gfx_buffer.c,
   surface_client.c, notify_client.c e time_client.c uno per uno; proprietà,
   generazioni, mappe, commit e resize vanno dimostrati prima della rimozione
   dell'adattatore WINDOW. Finestra vuota durante resize ancora riproducibile.
6. **Font/UART:** nxfontdd asincrono ed estensione SDK da implementare riusando
   font_lib; nxfont resta interfaccia utente, font kernel solo emergenza dopo
   migrazione dei consumatori. UART da HAL ai driver kernel secondo ASTRA,
   senza portare ora il driver in usermode; servizi distinti di debug dei
   processi e accesso ADB con capability. nxadb interagisce con il desktop
   attraverso QMP: il suo log seriale è diagnostica, non una shell UART utente.
7. **ISA/entropia/qualificazione:** nessuna nuova ISA senza salvataggio contesto
   e matrice hardware. ARM QEMU usa counter-mix non qualificato per SSP;
   il warning firmware TRNG non dimostra quale sorgente consumi il kernel.
   Rivedere duplicati legacy e tracciare l'eventuale doppio boot nella stessa
   cattura. Completare Power12 globale, test statici e hardware prima di una release.

## Protocollo di revisione alla ripresa

L'elenco completo dei file del checkpoint, con hash e obbligazione di revisione,
è in [CHECKPOINT-FILES-2026-11-12.json](CHECKPOINT-FILES-2026-11-12.json).
Comprende anche codice preesistente al presente intervento: nessun file viene
dichiarato interamente revisionato perché compila o passa il delta. L'inventario
generale di userland/system e il catalogo vivo graphics restano i riferimenti.

Per ogni gruppo: leggere chiamanti e protocollo vivo; confrontare la doc legacy
con la richiesta corrente; verificare ownership e fallimenti; correggere senza
duplicare provider; applicare Power12; eseguire prove mirate e poi entrambe le
architetture via nxadb. Registrare esattamente sorgenti, esiti negativi e limiti.
Non ripristinare vecchi file isolati da questo checkpoint: ABI, SDK, servizi e
immagine sono stati salvati insieme su richiesta del manutentore.
