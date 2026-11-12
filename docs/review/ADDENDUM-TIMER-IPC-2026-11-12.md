# Addendum: timer, IPC, libc e diagnostica

## Metodo e prove riproducibili

Il manutentore ha richiesto una nuova pulizia prima della prosecuzione:
`make clean` è stato rieseguito. Le build ordinarie sono ripartite in `build`,
con avvio e interazioni tramite nxadb su AMD64 e AArch64. I risultati indicati
come storici non qualificano automaticamente una revisione successiva.

Priorità corrente: timer/latenza, affidabilità PORT, libc/Coreutils,
diagnostica e provenienza dell'entropia. Le osservazioni dei log sono ipotesi
da verificare. Non sono state cambiate le soglie per ottenere un PASS.

## IPC: race del figlio rapido riprodotta

La prova iniziale supera 20/20 ripetizioni su AMD64, ma fallisce alla
quattordicesima su ARM: `packet=0 forged-services=3 wake-race=0`, suite 2/3.
La traccia mirata successiva riproduce il problema alla decima ripetizione:
nxexecd registra `phase=owner ... rc=-3`; il client riceve `-ESRCH`.
Il programma `/bin/true` può già essere terminato quando il broker tenta
`SETOWNER` dopo lo spawn. Il richiedente è ancora vivo e attende la risposta.
Prove: `build/runs/addendum/aarch64/port-13.log` e
`build/runs/addendum/aarch64/traced/port-09.log`.

La correzione usa il protocollo ad oggetti esistente: comando
`OBJ_CTL_SPAWN_OWNED=18` su una capability PROCESS con diritto DESTROY,
oltre ai requisiti privilegiati e CAP_SPAWN. Il descrittore v1 è di 48 byte,
copiato e validato nel kernel; l'identità del proprietario deriva dall'oggetto,
mai da un PID nel payload. L'ownership viene assegnata mentre il figlio è
ancora PROC_CREATED, prima di ELF/finalize. Non si introduce uno stato di
processo sospeso o una nuova syscall. PID e stato d'uscita restano utilizzabili
anche se il figlio termina prima del ritorno al broker.

L'estensione execsvc contiene risoluzione del programma e accesso al nuovo
meccanismo; nxexecd mantiene la politica del servizio. Dopo un avvio positivo,
un errore di ripristino del contesto del broker non trasforma il risultato in
un fallimento che potrebbe indurre un secondo avvio.

Il caricamento ELF non tiene più disabilitati gli interrupt per tutta la
lettura. Percorso e argomenti sono locali alla chiamata. Una guardia di
creazione rinvia KILL/STOP del creatore fino alla liberazione delle risorse e
dei pin al confine della syscall; CONT cancella uno STOP pendente, KILL ha
precedenza. Lo STOP viene applicato sotto lo stesso lock di CONT.

I test host estraggono le funzioni reali tramite AST e le eseguono con
ASan/UBSan: proprietà prima della pubblicazione, figlio già terminato al
ritorno, errori del loader, input malformati, controlli di autorità e ordine
KILL/STOP/CONT. Il test guest conserva i tre gruppi e aggiunge attesa esatta
del figlio rapido e rifiuto del nuovo comando senza autorità.

Obbligazioni non chiuse: pin del processo nella redirezione legacy da una
tabella FD esterna (PROC-REF-01), delega completa di ctty/ambiente e risultato
incerto se il broker muore dopo la creazione ma prima della risposta RPC.

## Timer: separare misura e comportamento

`nxtimerdiag --quiet-probe` conserva le stesse soglie e non stampa durante i
campioni. Nella modalità verbosa PRE precede le letture iniziali dell'orologio:
il tempo bloccato dal logger non viene attribuito alla sleep richiesta.
Il test con logger simulato da 300 ms fallisce sul sorgente precedente e
passa sul corretto; entrambe le modalità misurano la sleep simulata da 33 ms.
I massimi delle statistiche per CPU sono marcati `max_scope=since-boot`:
la differenza tra due massimi cumulativi non misura un massimo nell'intervallo.

Misure precedenti alla correzione owner/creazione, 160 campioni da 33 ms:

| Condizione | OS1_sleep media / massimo | nanosleep media / massimo |
| --- | --- | --- |
| AMD64 sola, quiet | 34,295 / 39,534 ms | 34,187 / 38,176 ms |
| ARM con AMD64 attiva, quiet | 70,003 / 100,400 ms | 44,648 / 98,458 ms |
| ARM sola, quiet | 40,981 / 86,201 ms | 61,635 / 113,177 ms |

Questi campioni non dimostrano una causa unica host/kernel. La frequenza
nominale di compatibilità è 1000 Hz; la modalità a deadline è tickless e il
quantum configurato è 30 ms. Non esiste una garanzia di risveglio entro 1 ms.

Dopo owner-before-publish, 100/100 esecuzioni PORT passano su ciascuna
architettura, senza attenuare le aspettative. I massimi della fase di 1024
scambi sono però 37,720 s AMD64 e 3,809 s ARM. Questi sono tempi dell'intera
fase, non latenze di un singolo messaggio. Rapporti:
`build/runs/addendum/{amd64,aarch64}/after-owned/port-soak.json`.

È stata individuata un'incoerenza autonoma: la selezione dello stesso task
aggiornava il quantum logico senza riprogrammare il clockevent locale.
Ora ogni aggiornamento in `__sched_arm_quantum`, incluso idle, raggiunge
il provider. La regressione host fallisce sul sorgente precedente e passa
sul corretto. La nuova serie `after-rearm` mostra ancora fasi AMD64 di
circa 38 s: il riarmo da solo non risolve il rallentamento. Serve osservare
CPU, code e pin dello stack, senza un watchdog che nasconda il problema.

La successiva strumentazione per scambio, senza logging dentro il ciclo,
supera altre 30/30 esecuzioni per architettura. Su AMD64 la fase più lenta
dura 35,552 s, ma il massimo singolo intervallo della stessa fase è 73,921 ms;
su ARM il massimo singolo osservato è 113,393 ms. I tempi di decine di secondi
non sono quindi una singola sospensione di tale durata. La media per scambio
della fase AMD64 lenta è circa 34,7 ms, compatibile con attese ripetute del
quantum configurato, senza ancora dimostrarne l'intera causalità. Rapporti:
`build/runs/addendum/{amd64,aarch64}/per-round/port-soak.json`.
La cattura di CPU e code tramite QMP/nxadb è conservata in
`build/runs/addendum/amd64/idle-capture-first.json`; ha fermato il guest per
circa 25 ms, perciò la relativa iterazione non è una misura incontaminata.

## Coreutils/libc: cause dimostrate e verifiche aperte

- Il percorso fts_open usato da chmod/chown/chgrp è uno stub ENOSYS nel glue
  locale. xfts_open di Gnulib passa quel NULL a xalloc_die: il messaggio
  `memory exhausted` non dimostra un esaurimento della memoria. La sorgente
  FTS upstream esiste già; occorre integrarne modulo/header e dipendenze FD,
  senza costruire un secondo FTS privato o modificare il sottoprogetto.
- nice arriva allo stub execvp ENOTSUP; le API di priorità che simulano
  successo richiedono hardening separato.
- nohup segnala l'assenza di /dev/null. users/who richiedono verifica utmp.
  Il sorgente upstream di truncate apre con O_NONBLOCK, attualmente rifiutato
  dal kernel: questo spiega il messaggio `Invalid argument` prima ancora di
  raggiungere ftruncate. Non si elimina il flag senza verificare la semantica
  del tipo di oggetto aperto.
- Il caso readlink su un file regolare non deve aspettare un successo:
  prima di correggere l'aspettativa servono casi positivi e negativi reali.
- La ripetizione della suite espone fdtest: la scrittura tramite descrittore
  a offset zero tronca la fixture da 57 a 8 byte. La semantica di sostituzione
  legacy del VFS va distinta dalla scrittura posizionale del descrittore.
  Ricreare la fixture non risolverebbe il difetto.

## Incremento file e pubblicazione delle pagine

Il nuovo provider VFS ha un'operazione esplicita truncate: il verbo FILE
esistente numero 10 accetta una lunghezza non negativa, mantiene la posizione
e richiede WRITE. ext4 separa scrittura posizionale e cambio della lunghezza.
La crescita usa il writer già presente con blocchi di zeri limitati a 4096
byte, senza un buffer grande quanto il file; azzera anche dati nascosti da
una precedente riduzione. Il limite del provider rimane a 32 bit. I blocchi
oltre EOF rimangono allocati, come nel percorso precedente: il recupero
transazionale di extent/indiretti resta un'obbligazione separata.

O_TRUNC conserva l'inode anziché cancellarlo e ricrearlo. truncate/ftruncate
della libc usano questa stessa operazione, senza leggere il file con un
descrittore WRITE-only e senza spostare l'offset. Il helper legacy OS1_fs_write
mantiene la sostituzione da posizione zero chiedendo esplicitamente truncate;
anche regfs implementa l'azzeramento con i propri controlli di autorità.
Una scrittura normale al descrittore conserva la coda, incluso offset zero.
Il test host fallisce sulla precedente funzione di dimensionamento e passa
sulla corretta; verifica inoltre crescita, zeri, limiti e errori del provider.
fdtest aggiunge WRITE-only, posizione conservata, coda azzerata dopo riduzione
e successiva crescita, e rifiuto esatto su descrittore READ-only.

La verifica PMM ha inoltre riprodotto una race indipendente: pmm_free_page
liberava il bit e il lock prima del poison 0xCC. Un'altra CPU poteva allocare
e inizializzare la pagina, poi subire quel poison. Il nuovo ordine completa
la scrittura limitata a una pagina prima di renderla allocabile. Il test
inietta una nuova allocazione immediatamente al rilascio del lock: fallisce
sul precedente codice e passa sul corretto con ASan/UBSan. Non viene
presentata questa race come causa dimostrata dei messaggi FTS o delle latenze.

## Diagnostica e provenienza SSP

Un candidato ELF mancante è ora DEBUG: la ricerca /bin seguito da /sys/bin
non produce un falso errore. Il chiamante conserva l'errore finale reale.
nxtestrunner emette una sola copia stdout dei record canonici; restano da
valutare gli altri test legacy con output sia diretto GUI sia stdout.

Su 100 catture ARM correnti/archiviate esaminate non compare più di un banner
UEFI per cattura. I riavvii espliciti nxadb archiviano esecuzioni distinte.
Non è stata dimostrata una causa per il secondo avvio indicato nel log del
manutentore; non viene dichiarato un reset spontaneo senza quella prova.

La policy kernel prova 16 volte l'RNG architetturale e, se indisponibile,
mescola 8 campioni del contatore iniziale con un indirizzo. Il kernel non
consuma l'interfaccia TRNG del firmware. Ora pubblica solo la provenienza,
senza stampare il valore del canary: AMD64 `arch-rng`, Cortex-A57 QEMU
`counter-mix strength=unqualified`. Questo chiarisce il warning, non qualifica
la robustezza del fallback. La policy non viene sostituita con un RNG privato.

Riferimenti primari per distinguere firmware e istruzioni architetturali:
[EDK2 ArmTrngLib, versione stable202408](https://github.com/tianocore/edk2/blob/edk2-stable202408/ArmPkg/Library/ArmTrngLib/ArmTrngLib.c)
e [Arm True Random Number Generator Firmware Interface](https://documentation-service.arm.com/static/61dff1202183326f21772c2d).

## Gate e perimetro

La nuova build con riarmo raggiunge shell e suite 17/18 su entrambi i guest;
Coreutils resta fallito. Lo stress successivo e le misure finali vanno
registrati separatamente. Il gate Power12 globale resta rosso per debito
legacy: il delta verde non certifica l'intero kernel. Questi incrementi non
completano la migrazione graphics/font/UART, documentata nel catalogo vivo.
Queste sezioni descrivono le prove precedenti al checkpoint completo richiesto
successivamente dal manutentore. Stato di salvataggio, risultati finali e
obbligazioni residue sono in [CHECKPOINT-2026-11-12.md](CHECKPOINT-2026-11-12.md).
