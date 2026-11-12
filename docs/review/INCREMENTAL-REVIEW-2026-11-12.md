# Revisione incrementale — 12 novembre 2026

**Stato di ripresa:** il manutentore ha successivamente richiesto un commit
completo e push di tutto il lavoro. Il riepilogo corrente e le obbligazioni
sono in [CHECKPOINT-2026-11-12.md](CHECKPOINT-2026-11-12.md). I riferimenti allo
stage parziale e ai test in corso qui sotto descrivono iterazioni storiche.

Le nuove prove di timer, race PORT, creazione atomica, logging ed entropia sono
in [ADDENDUM-TIMER-IPC-2026-11-12.md](ADDENDUM-TIMER-IPC-2026-11-12.md).
Le sezioni seguenti conservano anche risultati storici delle iterazioni.

## Metodo e perimetro

La richiesta corrente del manutentore guida il lavoro. I documenti legacy
sono riferimenti storici da confrontare con sorgenti, build e comportamento.
L'inventario iniziale è in USERLAND-SYSTEM-CATALOG-2026-11-12.md. Le differenze
inizialmente staged e unstaged sono conservate separatamente in /tmp; nessun
lavoro preesistente è stato eliminato. Lo stage viene ricostruito per incrementi
verificati. Non è stata pubblicata una PR.

Power12 completo e delta sono due risultati diversi. Il delta non certifica il
resto di una funzione legacy. Dal presente incremento R1 rifiuta anche BreakStmt
nei cicli e negli switch; due test di mutazione verificano il controllo.

## Risultati della revisione

| Priorità | Riscontro | Correzione / stato |
| --- | --- | --- |
| P1 | SYS_GETCWD copiava dal campo cwd il numero di byte chiesto dal chiamante: lettura oltre il campo con destinazioni grandi e successo senza NUL con destinazioni piccole | Copia soltanto lunghezza+1; rifiuta NULL, zero capacità e capacità insufficiente. Test reale con sentinelle e buffer piccoli |
| P1 | Il resolver SDK interpretava getcwd come un puntatore POSIX, mentre l'API nativa restituisce 0 al successo | Controllo dello stato nativo e test del percorso relativo |
| P1 | nxexecd/nxtimerdd/nxpaxd decodificavano e chiudevano slot dal payload senza sapere se il kernel avesse trasferito handle | Envelope PORT con conteggio autenticato dal kernel; i server verificano il conteggio e liberano allegati inattesi |
| P1 | EXECSVC_FD_GRANTED indicava uno slot arbitrario del server anche senza trasferimento contestuale | Protocollo exec v3: ref è l'ordinale di un allegato supplementare del messaggio, mai uno slot scelto dal client |
| P1 | Il resize logico dell'app produceva un nuovo evento nella stessa mailbox; alternandosi a richieste WM successive poteva riattivare dimensioni obsolete | Nessuna eco del resize logico; dimensioni di presentazione WM conservate esplicitamente anche quando coincidono temporaneamente con quelle logiche |
| P2 | Il timeout di un popup poteva riscrivere una ricevuta sopra una nuova voce della stessa posizione nel ring notifiche | Chiude la ricevuta prima di sostituire popup o riusare lo slot; test di wrap su broker reale |
| P2 | Broker notifiche: un messaggio ogni 300 ms, dipendenza dal caricamento del font e dal successo del log stdout | Batch limitato di 32 messaggi ogni 20 ms; ricezione/registro indipendenti dal font e dal log; severità/metadati validati |
| P2 | Due Shift/Ctrl premuti insieme: rilasciarne uno disattivava anche l'altro | Stato dei due tasti separato nella stessa estensione SDK; test delle coppie e del repeat CapsLock |
| P2 | Test strutturale dei servizi fermo a sette server, cinque moduli e manifest con soli cinque campi | Allineato a nxttyd, gfx_buffer, port_client e autorità opzionale del manifest |
| P2 | Snapshot ABI con checksum precedenti alle modifiche locali caps/object | Verificata identità kernel/SDK; checksum aggiornati per i contratti modificati |

## Contratto PORT verificabile

Il messaggio IPC originale non cambia. Un receive in os1_port_packet restituisce
anche versione, dimensione e numero di handle realmente installati dalla syscall
port_send_caps. Il conteggio viene memorizzato insieme alla voce della coda; una
normale write imposta sempre zero. Il payload non può autenticare se stesso.

port_client.c è l'estensione comune; i demoni mantengono gestione richieste e
lifecycle. porttest invia sia numeri falsi senza allegati sia un buffer realmente
trasferito e attenuato; dopo richieste malformate verifica time/exec reali.
Il nuovo metadata risolve la provenienza degli handle, non i blocchi indefiniti
possibili nei precedenti protocolli RPC su pipe: questa obbligazione resta aperta.

## Primo avvio misurato

make clean seguito da make run ARCH=amd64 arriva alla shell. Il successivo
avvio MDEBUG=1 passa 32/32 test kernel; suite userland 12/14. La prova getcwd
aggiunta appositamente fallisce sul vecchio comportamento e ha motivato la
correzione kernel. Fallisce anche il gruppo Coreutils: readlink su un file non
symlink ha un'aspettativa sbagliata; users/who, nice/nohup, truncate e chmod/chown/
chgrp evidenziano problemi nelle interfacce libc/portability. Nessun risultato
fallito è stato trasformato in un PASS o escluso dalla suite.

Verifiche successive e stage: in corso. I risultati di un avvio precedente non
certificano modifiche applicate dopo la sua compilazione.

## Obbligazioni ancora aperte

- Qualificazione Power12 globale: migliaia di riscontri legacy; non certificata.
- Verifica nxadb aggiornata per entrambe le architetture, test completi e screenshot del resize.
- Runtime kernel ancora con allocazioni generali e logica legacy di finestre/term/font: la rimozione dell'eco non completa la migrazione.
- RPC time/exec/pax ancora su pipe potenzialmente bloccanti: migrare al modello capability con attesa limitata, senza duplicare implementazioni.
- UART: provider ancora in HAL; spostamento nei driver e separazione debug/ADB da completare. Nessuno spostamento in usermode autorizzato per questo driver nella fase corrente.
- Font: nxfont è ancora un rasterizzatore/uploader con math privato errato e commenti non aggiornati. Il kernel già copia il blob; non serve un processo che giri indefinitamente per mantenerlo vivo. nxfontdd, estensione asincrona, cache condivisa, impostazioni globali/per-app ed eliminazione del percorso normale kernel restano da implementare.
- image.c, buffer_client.c, gfx_buffer.c, surface_client.c, time_client.c fanno parte del catalogo; la loro presenza non costituisce verifica dei relativi comportamenti.


## Aggiornamento: confine graphics e audit ISA

La precisazione del manutentore del 12 novembre supera la scelta del piano
8 novembre di tenere la politica delle finestre nel kernel. Anche focus,
stacking, geometria desktop, associazione finestra/app e lifecycle GUI sono
politica userland. resources rimane il confine già esistente per meccanismi
surface/buffer e capability, non una destinazione per rinominare compositor.c.
Il piano graphics è aggiornato; il vecchio catalogo funzioni è marcato storico.

Riscontri ISA: i frame attuali preservano FXSAVE64 su AMD64 e Q0-Q31/FPSR/FPCR
su AArch64. Non certificano AVX/XSAVE/SVE/SME. L'inizializzazione ora spegne
esplicitamente OSXSAVE e ZEN/SMEN non gestiti, ed evita di ereditare CR0.TS/EM.
Questa è una limitazione coerente con il frame, non implementazione di nuove
estensioni. Il boot AArch64 già inizializzava CPACR da zero, ma anche la
successiva inizializzazione per CPU esplicita il contratto. Rimossi inoltre i
marker I/J/K/L/M scritti direttamente su COM1 da HAL/core/core_protocol.c:
il protocollo comune non deve conoscere un indirizzo UART AMD64.

Riferimenti primari consultati per il controllo dei bit (nessun codice importato):
- [Intel SDM Vol. 3A, CR4/OSXSAVE](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-3a-part-1-manual.pdf).
- [Arm SME supplement DDI0616, CPACR_EL1 ZEN/SMEN/FPEN](https://documentation-service.arm.com/static/62015c6c965f7d118e3f5f4c).

I test host/cross/sanitizer HAL passano, ma il test ioport_amd64 originale
controlla due volte soltanto available()==1: non è prova di correttezza I/O né
supporto hardware reale. Port I/O ARM è dichiarato assente; gli adattatori legacy
con letture zero/scritture vuote non equivalgono a una transazione riuscita.
Restano da verificare gli utenti del trasporto, i registri MMIO, i fallback e
l'assenza di assunzioni QEMU. simdtest è ora incluso nella suite reale per
verificare preservazione del contesto tra processi su entrambe le ISA.


## Ultima verifica prima delle ulteriori diagnosi

- AMD64: kernel 32/32; suite 16/18; restano il gruppo Coreutils e il caso
  captest che avvia Lua. AArch64: kernel 33/33; suite 17/18, fallisce Coreutils.
- libc 28/28, notifytest 14/14, porttest 3/3, gfxbuffertest 2/2 e simdtest
  1/1 passano su entrambe le architetture. Nessun nuovo panic in questi due giri.
- Il primo test resize chiedeva il resource di scena da un processo USER:
  il rifiuto era corretto. È stato corretto il test per leggere la capability
  WINDOW posseduta dal chiamante, senza allargare i diritti sul resource.
- La diagnosi AMD64 ora identifica vector=6 (#UD) nel processo Lua. Il PC
  corrisponde a l_alloc, ma i byte del file host a quel punto non giustificano
  #UD. Causa non ancora dimostrata; nessuna correzione inventata sul parser Lua.
- Nei boot diagnostici si aggiunge una lettura limitata dei byte dell'istruzione
  corrente prima del teardown, per distinguere ISA non supportata da mapping o
  codice corrotto. La dipendenza MDEBUG ricompila anche fault.o; l'auditor Clang
  analizza esplicitamente questo profilo, invece di escluderne il codice.


Il client notifiche ora usa l'enqueue PORT non bloccante già esistente
(port_send_caps con zero allegati), senza passare dal wrapper POSIX che
riattiverebbe OS1_report_error. Una coda satura restituisce EAGAIN; un errore
libc non deve poter addormentare indefinitamente il chiamante dietro al broker.


La prova mouse nxadb su entrambe le architetture riduce la finestra TTY da
640x428 circa a 480x328; la shell risponde con RESIZE_ALIVE. Le catture immediate
mostravano un ridisegno parziale/vuoto prima del ritorno di tutto il testo.
Il servizio TTY pubblicava prima un buffer cancellato, poi ogni riga separata.
Ora renderizza nel backing capability già esistente e pubblica una singola
banda contigua di righe sporche. Il resize kernel legacy può comunque mostrare
una transizione tra buffer: l'atomicità completa richiede il cutover surface/
commit a oggetti, non un'altra policy aggiunta al kernel.

## Incrementi successivi: immagini disco e stato dei processi

La diagnostica AMD64 ha mostrato sedici byte `ff` in `l_alloc` di Lua, a fronte
di istruzioni valide nel file host. La traduzione VMM portava a RAM ordinaria
(0x07d18360), non al PCI hole. La lettura indipendente dell'immagine ext4 ha
poi trovato gli stessi byte corrotti nel file su disco: i dati attraversavano
il blocco 32768, riservato ai metadati del gruppo successivo. Il generatore
avanzava il cursore senza saltare i blocchi già riservati; la scrittura finale
delle bitmap sovrascriveva parti dei file. Non era un'istruzione ISA mancante.

Il generatore ora usa un allocatore comune per dati, directory e nodi degli
extent/indiretti. Gli extent descrivono i blocchi realmente assegnati e si
interrompono ai metadati di gruppo; il dimensionamento include queste divisioni.
`make test-mkdisk-groups` crea e rilegge immagini reali oltre 128 MiB nei due
layout, confrontando hash, ordine logico e assenza di sovrapposizione ai
metadati. Entrambe le prove falliscono sul sorgente precedente e passano sul
sorgente corretto. Non sono stati modificati parser o aspettative di Lua.

ARM ha inoltre esposto una doppia raccolta dello stato di uscita: process_wait
riportava ripetutamente un figlio morto ancora nel pool, e il reaper poteva
ripubblicarne lo stato. Un flag protetto da sched_lock consuma il risultato una
sola volta nei due percorsi. captest verifica anche che il risultato resti
consumato dopo il successivo recupero delle risorse.

Dopo questi due incrementi, prima della modifica al runtime matematico:
AMD64 kernel 32/32 e ARM kernel 33/33; captest 83/83 e suite 17/18 su entrambi.
Coreutils resta fallito (AMD64 41/50, ARM 40/50). Le interazioni mouse nxadb
e il comando RESIZE_ALIVE passano su entrambe le architetture; catture:
`build/runs/amd64/iso-bios/amd64-20261112-022031-653824000.png` e
`build/runs/aarch64/iso-uefi/aarch64-20261112-022036-893580000.png`.

Durante la revisione l'indice è stato trovato con 109 file. Lo snapshot completo
è conservato in `/tmp/nexs-review-index-observed-0216.patch`; l'indice è stato
ricostruito con soli incrementi selezionati, lasciando invariati i contenuti.

## Runtime del compilatore: verifica in corso

Il fault di seq su ARM punta a `__fixunstfdi`; il disassemblato mostra un BL
verso la stessa funzione. I wrapper C di math.c usavano le operazioni per cui
GCC emette quelle stesse chiamate. Anche i confronti restituivano booleani
invece di rispettare il contratto dei helper. Rimossi i sostituti locali;
i link userland, inclusi gli overlay Coreutils, usano il libgcc del cross
compilatore già fissato dal progetto. Nessuna nuova aritmetica software privata.
Il nuovo caso libctest verifica conversioni, operazioni, confronti ordinati e
NaN e conversione unsigned a 64 bit. Il rebuild e la verifica nxadb successivi
devono completarsi prima di considerare qualificato questo incremento.

Riferimenti del compilatore: [runtime GCC](https://gcc.gnu.org/onlinedocs/gccint/Libgcc.html)
e [contratto soft-float](https://gcc.gnu.org/onlinedocs/gcc-7.2.0/gccint/Soft-float-library-routines.html).

## Prossimi passaggi e criteri di chiusura

1. **Base e libc:** completare i fallimenti Coreutils e le API che simulano
   successo senza eseguire l'operazione. Conservare errori e test reali.
2. **Notifiche:** chiudere l'incremento broker/client, provenienza capability,
   saturazione, lifecycle e ricevute. Protocolli nell'SDK; demone nei servizi;
   comando di controllo nei binari di sistema.
3. **Buffer/surface:** verificare una per una image, buffer_client, gfx_buffer,
   surface_client e time_client. Riutilizzare resources; definire e verificare
   ownership, generazioni, mappe e commit prima di rimuovere l'adattatore WINDOW.
4. **Grafica userland:** migrare scena, focus, stacking, resize, decorazioni,
   testo e composizione nel servizio e nella libreria esistenti. Chiudere ogni
   gruppo di chiamanti prima della rimozione kernel; nessun semplice cambio
   di directory equivale a migrazione completata.
5. **Font e UART:** implementare nxfontdd asincrono ed estensione, mantenendo
   nxfont come interfaccia; font kernel solo emergenza. Spostare UART da HAL
   ai driver kernel, con servizi debug/accesso separati e autorità esplicita.
6. **ISA e qualificazione:** completare la matrice hardware e i fallback;
   rieseguire build pulita, test via nxadb, NXPAX e interazioni grafiche sulle
   due architetture. Il gate Power12 globale resta obbligatorio e attualmente
   fallito; il delta verde non lo sostituisce.

## Stato misurato dopo il runtime e la ripetizione della suite

I due avvii nxadb successivi al runtime standard passano kernel AMD64 32/32 e
ARM 33/33, captest 83/83, libc 29/29 (compreso long double), notifiche 14/14,
PORT 3/3, gfx buffer 2/2 e SIMD 1/1. seq passa anche su ARM; Coreutils resta
41/50 su entrambi. La suite al primo avvio è 17/18 su entrambi.
Una lettura indipendente delle due immagini conferma che il file Lua coincide
byte per byte con il corrispondente file host.

La successiva esecuzione esplicita di `/sys/bin/nxtestrunner` attraverso nxadb
porta entrambe le suite a 16/18: oltre a Coreutils fallisce fdtest.
La fixture dedicata era di 57 byte e dopo il primo test è di 8 byte.
`object.c` scrive attraverso `vfs_write_file`, che arriva a ext4_write;
ext4_write_new_size considera ogni offset zero una sostituzione completa.
Per un descrittore POSIX una normale scrittura in posizione zero deve invece
conservare la coda; il solo O_TRUNC/truncate deve cambiare la lunghezza.
Il test tenta di ripristinare gli otto byte originali, ma il contenuto oltre
quegli otto byte è già perso. La correzione deve separare queste semantiche
nel provider e nell'accesso tramite oggetti, senza mascherarla ricreando la
fixture o cambiando l'aspettativa. Questo è il prossimo incremento libc/VFS.

`make check ARCH=amd64` e `make check ARCH=aarch64` terminano con successo,
inclusi contratti ABI, delta Power12 e stress NXPAX; mantengono esplicitamente
il risultato globale Power12 NON VERIFICATO. Le prove host tastiera/TTY/chrome/
alpha, layout servizi, mutazioni Power12 e controlli nxadb passano.
Non sono state certificate nuove ISA né eseguite prove su hardware fisico.

Lo stage contiene solo gruppi selezionati: catalogo/tastiera e audit dei break;
generazione immagine con regressioni; consumo dello stato di uscita; runtime
del compilatore con regressione. Il resto della migrazione rimane aperto.
Nessun commit o PR è stato pubblicato.

Le catture finali dopo il ritorno della shell sono
`build/runs/amd64/iso-bios/amd64-20261112-023048-234636000.png` e
`build/runs/aarch64/iso-uefi/aarch64-20261112-023047-053789000.png`.
Mostrano testo conservato e RESIZE_ALIVE nella finestra ridotta. La cattura
immediata ARM durante la transizione mostra ancora una finestra vuota prima
del ridisegno: il resize non è dichiarato completamente risolto. L'assenza
di eco e la sopravvivenza della shell non certificano presentazione atomica.
La chiamata nxadb shell della suite ARM ha esaurito il limite di cattura
di 30 secondi; il log seriale integrale ha poi registrato la conclusione
16/18. I risultati applicativi qui riportati provengono da quella conclusione,
non dal codice di ritorno del comando host.
