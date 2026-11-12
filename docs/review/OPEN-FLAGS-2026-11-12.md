# Incremento successivo al checkpoint: apertura NONBLOCK dei file

Base: checkpoint completo `80359034fe7fcd487a198d7b477d0f47b42cdc93`.
Il relativo inventario con hash descrive quella revisione e resta immutato.

## Causa e confine

GNU truncate usa `open(O_WRONLY | O_CREAT | O_NONBLOCK)` prima di chiamare
ftruncate. L'ABI nativa rifiuta O_NONBLOCK, quindi il precedente test falliva
con EINVAL prima del cambio di lunghezza già corretto. Non viene modificato
il sottoprogetto Coreutils e non cambia l'aspettativa del suo test.

La personalità libc traduce il modificatore e verifica la capability realmente
acquisita con cap_query. Il tipo FILE è accettato: i provider correnti non
attendono un peer durante l'acquisizione; il modificatore non impone I/O disco
asincrono. Un oggetto PROCESS viene rifiutato con ENOTSUP e la capability viene
chiusa prima del ritorno. Nessuna ipotesi basata sul prefisso del percorso o
su uno stat separato, nessuna nuova tabella di descrittori, syscall o opcode.

La modalità di accesso POSIX 3 viene rifiutata prima di aprire o troncare il
file. Gli altri bit sconosciuti continuano ad arrivare alla validazione nativa
e a produrre EINVAL. Errori di acquisizione, query e chiusura restano visibili.
L'apertura senza NONBLOCK conserva il comportamento precedente.

Riferimento primario: [Linux man-pages, open(2)](https://man7.org/linux/man-pages/man2/open.2.html),
che distingue il comportamento sui file ordinari da pipe e dispositivi.
Questo incremento non qualifica il supporto generale dei file status flags:
fcntl è ancora uno stub e non conserva/interroga O_NONBLOCK; F_GETFL/F_SETFL,
CLOEXEC, O_APPEND atomico e la collisione O_NOCTTY/O_APPEND restano aperti.
Anche il parametro mode di open mantiene la limitazione precedente.

## Verifica

`make test-open-nonblock` estrae le vere funzioni libc e le esegue con
ASan/UBSan: traduzione dei flag, tipo FILE/PROCESS, query fallita, errore di
chiusura, negazione di accesso, flag sconosciuti, modalità 3 senza side effect
e conservazione del percorso append precedente. Sulla libc del checkpoint
la regressione compila e fallisce al primo caso; sulla correzione passa.

fdtest aggiunge apertura/scrittura/riduzione/lettura con NONBLOCK, verifica che
una modalità invalida non tronchi il file e prova il rifiuto di /proc/<pid>
come capability PROCESS. I controlli precedenti rimangono presenti.

La prima immagine modificata ha fatto passare truncate su entrambe le
architetture, ma touch ha raggiunto una ricorsione prima nascosta dal rifiuto
di open: il contatore aggregato restava 41/50, con un caso corretto e uno
regredito. Il fault è stato risolto fino a fdutimens in utimens.c; il simbolo
futimens forte del fallback richiamava fdutimens, che con HAVE_FUTIMENS=1
richiamava futimens. Non è stato accettato quel contatore come miglioramento.

L'overlay ora seleziona il provider SDK già dichiarato dalla configurazione,
senza il fallback non rinominato. La ricetta dell'archivio dipende anche
dall'elenco dei sorgenti e ricrea l'archivio tramite file temporaneo: ar rcs
sull'archivio precedente manterrebbe futimens.o anche dopo la rimozione dalla
lista. Il test host usa il recipe reale con un membro obsoleto: fallisce
sulla ricetta precedente e passa su quella nuova.

La semantica dei timestamp rimane non qualificata: il provider SDK futimens
ritorna ancora successo senza persisterli. Questa correzione elimina il ciclo
di collegamento e ripristina il comportamento precedente, non certifica touch.

Le immagini finali avviate via nxadb raggiungono la shell su AMD64 BIOS e
AArch64 UEFI. Kernel 32/32 e 33/33; suite 17/18 su entrambi, con Coreutils
42/50 (otto fallimenti), captest 83/83 e libc 29/29. touch e truncate passano
come comandi; la limitazione dei timestamp descritta sopra resta valida.
Nessuna aspettativa del test Coreutils è stata modificata.

La ripetizione di fdtest passa su entrambe le immagini. GNU truncate porta la
fixture da 16 a 4 e poi a 32 byte, verificati con wc. Il contenuto finale è
verificato con due trasformazioni indipendenti NUL→Y e NUL→Z: entrambe
producono 32 caratteri attesi; un controllo con dati non nulli verifica il
percorso tr/pipe. Fixture eliminate e shell ancora reattive su entrambe.
Il primo tentativo base64 non era disponibile nell'immagine, e una pipeline
con due pipe non è supportata dalla shell attuale: questi tentativi sono
conservati nel rapporto, senza presentarli come prove riuscite.

`make check ARCH=amd64` e `ARCH=aarch64` passano, inclusi contratti e NXPAX.
Delta Power12: cinque file di competenza, zero riscontri; il gate globale
rimane non qualificato. Passano le due regressioni host con le rispettive
prove negative sul codice precedente. L'ELF touch finale usa il simbolo SDK
debole futimens, e futimens.o non è più membro di libgnulib.a.
Prove selezionate: `evidence/2026-11-12/open-flags-verification.json`.

## File e revisione residua

| File | Incremento | Obbligazione residua |
| --- | --- | --- |
| system/lib/lib.c | adattatore libc open e controllo del tipo acquisito | status flags, fcntl reale, append e gestione mode |
| user/bin/fdtest.c | casi reali con file e capability PROCESS | mantenere ripetibilità e cleanup su entrambe le architetture |
| tools/test_open_nonblock.py | regressione sulle funzioni reali con fault injection | non sostituire la verifica guest con i mock |
| Makefile | target host dedicato | preservare il gate globale e la verifica runtime |
| user/portability/gnulib/overlay.mk | selezione provider futimens | verificare presenza e semantica reale di tutti i provider configurati |
| user/portability/gnulib/README.md | percorsi correnti e lacune esplicite | rimuovere le dichiarazioni legacy di conformità non dimostrate |
| tools/test_gnulib_archive.py | prova della rimozione di membri obsoleti | non sostituire il test dei simboli e l'esecuzione delle immagini |

La revisione FTS conferma che il glue locale restituisce ENOSYS e che fts_.h
è un ABI incompleto: l'integrazione del modulo upstream richiede header,
openat/fdopendir/fstatat e metadati coerenti. Collegare semplicemente fts.c o
accettare indiscriminatamente i flag non costituisce una correzione verificata.
Rimane inoltre da completare il tracciamento delle dipendenze degli header
e della configurazione in tutti gli oggetti Gnulib, prima di modificarne
massivamente le dichiarazioni HAVE_* e i moduli generati.
