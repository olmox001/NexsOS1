# Grafica userland — stato verificato e ripresa, 2026-11-08

**Stato: transizione parziale, non certificata completa.** Questo documento
chiude il contesto corrente su richiesta dell'utente, dopo le ultime modifiche,
il ripristino della barretta sul pulsante minimizza e le verifiche delle immagini
aggiornate. Non chiude i gate di parità, isolamento o prestazioni della migrazione.

Riferimenti correnti: [piano e confini](../architecture/GRAPHICS-USERLAND-MIGRATION.md),
[catalogo originale](../architecture/GFX-KERNEL-FUNCTION-CATALOG.md),
[registro pendenti §15](../PENDING-WORK.md#15-migrazione-grafica-in-userland--surface-manager-kernel--parziale--nexs-ng).
Gli inventari e rapporti del 2026-11-03 restano evidenze storiche, non descrizioni
del codice attuale. Le indicazioni operative nei documenti non sostituiscono le
richieste dell'utente.

## Implementazione presente

- `user/sys/bin/nxcompositor.c`: composizione effettiva in ring 3 da snapshot,
  ordinamento finestre, alpha/scaling, sfondo, titoli, pulsanti, cursore originale
  12×16 e presentazione tramite `OS1_display_present`.
- `system/lib/gfx/chrome.c` e `system/include/api/os1_gfx.h`: geometria condivisa,
  hit test, pixel dei due pulsanti nelle tre forme/sui due lati, tinta del titolo
  e predicato degli angoli arrotondati. Questo non include ancora tutta la
  decorazione del renderer originale. Il pulsante giallo ha una barretta centrale
  di 7 pixel; il cursore non è stato ridisegnato.
- `user/sys/bin/nxmoused.c`: processo separato per aggregazione puntatore,
  hit test, focus, drag/resize e click. Usa la stessa geometria del compositor;
  chiudi e minimizza invocano i controlli finestra esistenti. `nxkeyboardd` resta
  il processo tastiera separato. `BTN_TOUCH` estende il percorso del puntatore;
  questo non certifica un protocollo multitouch completo.
- `WININFO_PROTECTED` è presente nelle tre copie del contratto oggetti e negli
  snapshot: i controlli chiudi/minimizza non vengono disegnati né gestiti sulle
  finestre protette. Il layout delle strutture non è stato cambiato per questo flag.
- `user/sbin/nxinit.c` avvia execution service, bootstrap ambiente, compositor,
  tastiera e mouse, poi dock/barra/launcher/shell. I servizi sono supervisionati.
- Da `kernel/graphics/compositor.c` sono stati rimossi renderer/tick inutilizzati,
  buffer desktop di composizione e vecchi handler mouse/click/drag. Inizializzazione
  e cambio dimensioni mantengono metadati/policy; non allocano più quel backbuffer.
  Rimossi i relativi prototipi/extern inutilizzati. Il messaggio di avvio è ora
  `Compositor: Window manager initialized (1280x800)`.
- `SYS_COMPOSITOR_RENDER` è mantenuta come no-op di compatibilità. La sua rimozione
  dall'ABI richiede prima la conversione dei caller. Non è il backend del servizio.

Il catalogo conserva tutte le **182 funzioni originali**, raggruppate per i 13
file di origine. Nel medesimo perimetro sono ora presenti 159 funzioni: le 23
rimosse mantengono una voce con destinazione e lacune. La cancellazione di una
funzione non è conteggiata automaticamente come porting completato.

## Verifiche finali sulle immagini aggiornate

Prima dei test sono stati eseguiti, con le VM ferme:

```sh
make ARCH=amd64 boot-iso
make ARCH=aarch64 boot-iso
make test-gfx-chrome
python3 tools/nxadb.py --arch amd64 test --display none --soak-seconds 30
python3 tools/nxadb.py --arch aarch64 test --display none --soak-seconds 30
```

`boot-iso` include `all`: build e immagini GRUB completate su entrambe le arch.
I precedenti test effettuati dopo il solo `all` potevano usare un kernel ISO
vecchio e **non costituiscono evidenza della rimozione del renderer**. Nei due
run finali il nuovo messaggio del window manager è presente nella UART. L'esame
dei due `kernel.elf` non trova `compositor_render`, `compositor_render_internal`,
`compositor_tick`, `compositor_update_mouse` o `compositor_handle_click`.

| Verifica | AMD64 | AArch64 |
|---|---|---|
| KTEST | 11 passati, 0 falliti | 12 passati, 0 falliti |
| `nxwins` e comando focus | PASS, finestra 104 | PASS, finestra 103 |
| Health dopo 30 s e screenshot finale | PASS | PASS |
| Fault rilevati dal gate | 0 | 0 |
| Mouse: minimizza e ripristina dal dock | Verificato visivamente | Verificato visivamente |
| Tastiera: `nxwins` dopo ripristino | Shell in focus e prompt restituito | Shell in focus e prompt restituito |
| Host chrome ASan/UBSan | PASS, target comune | PASS, target comune |

Test mouse riproducibile per questa disposizione iniziale 1280×800:
`nxadb --arch ARCH mouse 633 -90 --click` minimizza la shell;
`mouse -929 558 --click` clicca la sua icona nel dock. Verificare la posizione
prima di riusare queste coordinate. Uno screenshot immediato può precedere il
prossimo frame; il ripristino AArch64 è stato verificato nella cattura successiva
e con un comando alla shell, non dedotto dal solo successo del comando QMP.

Il test host in `system/lib/tests/gfx_chrome_test.c` copre geometria delle tre
forme e dei due lati, entrambi i pulsanti, gap/bordo superiore dell'hit test,
colori centrali, alcuni punti arrotondati, overflow geometrico e tinta base.
Non è una prova esaustiva di tutte le decorazioni o di tutti i valori malformati.

## Timer: misura corrente, nessuna promessa di accelerazione

Eseguito `/bin/nxtimerdiag` via nxadb su entrambi i guest, desktop attivo:
160 campioni per architettura, 80 per API, attesa richiesta 33 ms.

| Architettura / API | Media ms | Massimo ms | Campioni oltre 83 ms |
|---|---:|---:|---:|
| AMD64 / OS1_sleep | 37.339 | 54.713 | 0 |
| AMD64 / nanosleep | 37.336 | 52.642 | 0 |
| AArch64 / OS1_sleep | 57.446 | 211.766 | 12 |
| AArch64 / nanosleep | 62.842 | 107.438 | 11 |

Entrambi i diagnostici: `failures=0`, regressioni clock=0, mismatch=0,
nessuna attesa oltre 250 ms. I guest erano contemporaneamente attivi sullo
stesso host; questi dati non sono un confronto controllato prima/dopo.
La latenza AArch64 resta significativa. Renderer/tick rimossi erano già fuori
dal percorso attivo: eliminarli non dimostra un miglioramento dei timer.
La gestione timer su tutte le CPU, lock/IRQ-off time e prestazioni richiedono
ancora misure dedicate e stress SMP.

**Vincolo esplicito dell'utente, aggiornamento di chiusura:** le latenze non
devono essere hardcodate. Il codice attuale viola ancora questo requisito:
`nxcompositor.c` attende sempre `NXCOMPOSITOR_FRAME_MS=33` dopo il rendering;
`nxmoused.c` e `nxkeyboardd.c` usano `OS1_sleep(5)` per il polling. I 33 ms si
sommano al tempo di rendering: non rappresentano un vero pacing a 30 Hz.
Sostituire queste attese con notifiche bloccanti di input/damage/completamento
e pacing derivato dal display, senza busy loop, wakeup persi o intervalli
arbitrari semplicemente spostati in una configurazione. Le deadline necessarie
devono derivare dal contratto dell'operazione o dalla modalità display.

Nel diagnostico i 33 ms sono lo stimolo richiesto, mentre i valori della tabella
sono misurazioni. La soglia di errore di 250 ms è anch'essa una costante del
test: `failures=0` non certifica una latenza accettabile. La prossima revisione
deve rendere espliciti durata richiesta e budget del test, misurare ritardo di
risveglio e distribuzione della latenza, e non adattare le soglie per nascondere
i colli di bottiglia. Questo requisito resta aperto; non è stato implementato
durante la chiusura del contesto.

## Evidenze conservate

Directory locale `docs/report/logs/GFX-USERLAND-20261108/` (ignorata da Git):
UART complete, stdout/stderr QEMU, manifest dei due run, screenshot desktop,
minimizzazione e ripristino, oltre al manifest SHA-256. Non dipendere soltanto
da `build/runtime`, che verrà sostituito dal prossimo avvio.

Run sorgenti: `build/runs/amd64/run-20261108-151258-904108000.json` e
`build/runs/aarch64/run-20261108-151302-821690000.json`.
Screenshot finali del gate: `amd64-20261108-151336-503253000.png` e
`aarch64-20261108-151347-068128000.png`, nelle rispettive directory `build/runs`.
Ripristino verificato: `amd64-20261108-151450-931955000.png` e
`aarch64-20261108-151517-876091000.png`.

Backup sorgenti: `/tmp/nexsos-gfx-input-backup-20261108/`, tutte le 15 voci
di `SHA256SUMS` verificate. Comprende grafica e input; **non** comprende i driver
GPU. Per questi resta la storia Git. Il backup temporaneo va conservato fino
alla chiusura della parità; non è un archivio permanente.

## Lavoro ancora necessario, senza doppie implementazioni

1. Completare la parità del renderer originale: occlusione/damage, clipping,
   angoli/ombre/bordi, temi/font e tutte le modalità di zoom/resize. Recuperare
   le implementazioni dal backup/storia e usare il catalogo, senza ricreare un
   secondo compositor parallelo. Il disegno attuale del bordo è semplificato.
2. Completare input: resize da tutti i bordi/angoli, protezione finestre,
   motion verso il target catturato, coordinate scalate, overflow/drop/recovery,
   unplug e stato per device. La motion attuale segue l'hit test anche durante
   capture applicativa. Collaudare separatamente PS/2, VirtIO, USB e touch;
   il test QMP corrente non certifica l'intera matrice.
3. Implementare veri oggetti buffer/surface, mapping condivisi, generazioni,
   diritti, pin/refcount, revoca e completamenti. Oggi gli snapshot copiano
   pixel dal kernel e il present li ricopia: non è zero-copy né isolamento
   completo. Eliminare allocazioni del framebuffer e delle finestre a ogni
   frame solo dopo aver definito proprietà e durata delle risorse.
4. Trasferire raster terminale/font, GL/2D/3D/region/chrome residui e tabelle
   stile/tema ancora kernel; conservare API/errori e comportamento esistenti.
   `compositor_window_write` e operazioni compatibili draw/blit sono ancora
   attive. La copia kernel di chrome resta anche per i KTEST: non contare il
   porting dei pulsanti come eliminazione di tutte le duplicazioni.
5. Trasferire implementazioni e protocolli GPU/display in servizi userland;
   kernel conserva validazione risorse/capability, MMIO/IRQ/DMA e handoff sicuro.
   Il present kernel e i driver VirtIO GPU sono ancora percorsi attivi.
6. Convertire i caller delle syscall superflue e poi ritirarle senza rompere
   la compatibilità. Inventariare anche syscall/input fuori dai 13 file del
   catalogo. Completare autenticazione IPC, errori e gestione del riavvio servizi.
7. Validare morte/riavvio servizi, resize/destroy concorrenti, revoche, memoria,
   SMP e stress prolungato su entrambe le arch. I vecchi SSP intermittenti
   terminal/input non sono chiusi da questi due run senza fault. Un precedente
   stress interrotto e un precedente arresto QEMU durante soak restano prove
   inconcludenti, non successi né diagnosi della causa.

Il bookkeeping kernel damage/alpha è rimasto. Un precedente tentativo di
rimozione tramite espressioni regolari è stato bloccato dalla revisione
automatica perché troppo ampio; non è stato applicato. Riprendere con patch
puntuali e analisi dei caller. Alcuni commenti storici nel codice descrivono
ancora il vecchio renderer e non costituiscono una certificazione Power of Ten.

## Stato del workspace alla consegna

Modifiche sia staged sia unstaged già presenti e condivise con altro lavoro;
nessun reset o commit automatico. `user/bin/coreutils` risulta modificato come
submodule e non è stato toccato da questa chiusura. Conservare anche i nuovi
test host non ancora tracciati. Le VM di verifica vengono fermate dopo aver
salvato le evidenze, per evitare lock sulle immagini alla prossima build.
