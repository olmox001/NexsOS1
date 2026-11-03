# Diagnostica timer, SMP e compositor — 2026-11-03

## Ambito

Diagnosi del blocco/crash globale osservato dopo alcuni secondi su AMD64 e AArch64, con attenzione al timer userland usato da nxbar. Tutto il lavoro resta nel repository.

## Che cosa usa davvero nxbar

Il timer di nxbar non è un timer hardware autonomo:

- `get_time()` restituisce millisecondi monotoni dall'avvio.
- Il percorso kernel `SYS_GET_TIME` deriva il valore da `mono_ns() / 1.000.000`; non usa il vecchio stub `timer_get_us()`.
- nxbar usa `get_time()` per la manutenzione periodica delle identità (intervallo di 5.000 ms).
- Il frame loop usa `OS1_sleep(33)`; la libc OS1 implementa l'attesa attraverso `nanosleep` e il timer software per-processo.
- nxinit chiama `flush()` e poi `OS1_sleep(33)`: è il frame pump del desktop a circa 30 Hz.

Vanno quindi distinti monotonic clock (`get_time`), risveglio bloccante (`OS1_sleep/nanosleep`) e presentazione grafica (`SYS_COMPOSITOR_RENDER`).

## Difetto AArch64 individuato e corretto

Prima della correzione, il test userland si fermava durante una sequenza di sleep. QEMU era ancora in esecuzione, senza panic seriale.

L'ispezione QMP di registri e memoria ha mostrato:
- PC dentro `arch_impl_spin_lock()`, chiamato da `schedule()`;
- lock della runqueue già tornata a zero;
- core ancora in attesa con `WFE`;
- `arch_impl_spin_unlock()` rilasciava la lock ma non emetteva `SEV`.

Il waiter poteva restare addormentato anche dopo il rilascio della lock, con IRQ locali mascherati. È un difetto concreto del protocollo spinlock AArch64, compatibile con lo stallo SMP osservato.
**Correzione nel working tree:** `arch_impl_spin_unlock()` esegue ora `SEV` dopo il release-store. La modifica è stata compilata e il successivo boot AArch64 SMP ha completato `nxtimerdiag` (160 campioni, `failures=0`); la shell ha risposto a un comando successivo.

## Diagnostica runtime osservata

Test eseguiti con quattro CPU QEMU:

- **AArch64, dopo SEV:** `nxtimerdiag` ha completato tutti i 160 campioni; nessun panic seriale osservato e prompt NXShell nuovamente disponibile.
- **AMD64:** due esecuzioni consecutive di `nxtimerdiag` hanno completato 160 campioni ciascuna; `failures=0`, prompt disponibile e `nxadb status` ha riportato `faults=0`.
- Le attese richieste da 33 ms hanno mostrato jitter significativo (circa 34–79 ms su AMD64 e valori più variabili su AArch64). Il test precedente non separava le statistiche di `OS1_sleep` e `nanosleep`, né verificava direttamente `get_time()`.

I log del boot precedente contenevano intervalli compositor/IRQ-off nell'ordine di decine di millisecondi (circa 25–66 ms AMD64 e 65–117 ms AArch64). La sorgente corrente di `compositor.c` è stata aggiornata per misurare separatamente `lock_wait` e `render_hold`; serve un nuovo boot per attribuire correttamente la latenza.

## Secondo difetto SMP individuato: input e rendering multi-caller

Il syscall `SYS_COMPOSITOR_RENDER` era autorizzato a tutti i processi con `CAP_WINDOW`. Ogni chiamata eseguiva `usb_hid_poll()`, `input_drain()` e `compositor_render()`.

L'ispezione ha mostrato che `usb_hid_poll()` non ha una protezione di reentrancy; `input_ring_lock` protegge l'estrazione dalla coda, ma non serializza l'intera fase di dispatch. Poiché nxinit, nxui e nxbar possono chiamare `flush()`, più processi potevano interrogare contemporaneamente lo stesso HCD e consumare/dispatchare eventi in parallelo.
**Correzione nel working tree:** PID 1 (`nxinit`) è ora l'unico proprietario del frame pump e dell'input poll/dispatch. Le chiamate `flush()` degli altri processi CAP_WINDOW ritornano rapidamente come richieste/hint; il loop supervisor di init presenta lo stato accumulato al frame successivo (33 ms). La modifica è coerente con il loop effettivo di `nxinit.c`, che chiama `flush()` e poi `OS1_sleep(33)`.

Questa correzione è stata compilata per entrambe le architetture, ma non è ancora stata avviata in una nuova immagine QEMU: il test runtime deve essere ripetuto prima di considerarla certificata.

## Diagnostica userland migliorata

`user/bin/nxtimerdiag.c` è stata estesa per:
- raccogliere statistiche separate per `OS1_sleep` e `nanosleep` (minimo, media, massimo e soglie di ritardo);
- confrontare `get_time()` in millisecondi con `os1_mono_ns()`;
- rilevare regressioni del clock, mismatch e latenze superiori a 250 ms;
- limitare l'output periodico per non introdurre ulteriore contesa sul compositor.

Il nuovo sorgente è stato compilato con `make ARCH=amd64 user` e `make ARCH=aarch64 user`. La nuova versione non è ancora stata eseguita nel guest; i risultati runtime sopra si riferiscono alla versione diagnostica precedente.

## VirtIO Block AMD64

È stata corretta anche la gestione della queue legacy PCI:
- la dimensione legacy esposta dal dispositivo è read-only; il driver usa la dimensione effettiva;
- gli offset di descriptor, available e used ring rispettano l'allineamento legacy;
- la memoria DMA viene dimensionata in base alla queue reale;
- il driver non dichiara feature opzionali legacy che la sua implementazione single-queue non consuma;
- il timeout del polling usa il contatore hardware e una deadline di 2 secondi, non un numero fisso di iterazioni.

Dopo queste modifiche le immagini precedenti hanno avviato il root filesystem e la userland su entrambe le architetture.
## Verifiche di compilazione

Dopo le modifiche più recenti (owner PID 1 e diagnostica userland), i target kernel, user e check sono passati su AMD64 e AArch64.

Toolchain verificata:
- AMD64: x86_64-elf-gcc 16.1.0, Binutils 2.46.1, QEMU 11.1.1, GRUB BIOS.
- AArch64: aarch64-none-elf-gcc 7.2.0, Binutils 2.29.1, QEMU 11.1.1, firmware UEFI e GRUB arm64-efi.

Le build complete `all` e `boot-iso` per entrambe le architetture erano passate dopo le correzioni VirtIO/SEV, ma prima dell'ultima modifica che assegna il compositor a PID 1 e dell'estensione di `nxtimerdiag`.

Un tentativo iniziale di build parallela AArch64 ha segnalato che `file_text.png` era scomparso durante la pianificazione/scrittura dell'immagine; le build sequenziali successive sono riuscite. Il percorso parallelo del target `disk` richiede quindi un controllo ulteriore.

## Stato e prossimi passi

1. Rigenerare le immagini complete dopo aver chiuso la VM AMD64 attualmente attiva.
2. Eseguire un nuovo boot SMP a 4 CPU su entrambe le architetture con il codice PID-1-owner.
3. Eseguire la nuova `nxtimerdiag` e confrontare statistiche `get_time`, `OS1_sleep` e `nanosleep`.
4. Lasciare nxbar, nxui, nxlauncher e NXShell attivi per almeno 60 secondi; controllare prompt, serial log, `nxadb status` e fault count.
5. Leggere i nuovi `GFX-TIMING lock_wait/render_hold` per decidere se il compositor richiede una sezione critica più corta o un percorso trylock/snapshot.
6. Follow-up API: `time()` in `user/sys/lib/lib.c` usa attualmente `OS1_time_now()` senza conversione da millisecondi a secondi; va allineato al contratto POSIX separatamente da `get_time()`.

Nessun commit o staging è stato eseguito; le modifiche restano nel working tree.


## Risultati del boot aggiornato — 2026-11-03, seconda passata

Le immagini complete sono state rigenerate sequenzialmente dopo le modifiche PID-1-owner e il nuovo diagnostico:

- `make ARCH=amd64 all`: PASS, exit 0.
- `make ARCH=amd64 boot-iso`: PASS, ISO BIOS pronta.
- `make ARCH=aarch64 all`: PASS, exit 0.
- `make ARCH=aarch64 boot-iso`: PASS, ISO UEFI arm64 pronta.

Entrambe le VM sono state avviate con `NEXS_QEMU_SMP=4` e con il nuovo `nxtimerdiag` incluso nel rootfs. Entrambe hanno raggiunto NXShell, eseguito il test kernel (8/8 PASS), completato i 160 campioni del diagnostico, ritornato al prompt e riportato `faults=0`. I guest sono stati poi arrestati con `nxadb stop`; entrambi risultano `running=False`.

### Dati timer

| Misura | AMD64 | AArch64 |
|---|---:|---:|
| Campioni OS1_sleep | 80 | 80 |
| OS1_sleep media | 40.89 ms | 49.80 ms |
| OS1_sleep massimo | 83.66 ms | 143.03 ms |
| Campioni nanosleep | 80 | 80 |
| nanosleep media | 41.57 ms | 46.51 ms |
| nanosleep massimo | 113.86 ms | 112.56 ms |
| `get_time()` regressioni | 0 | 0 |
| `get_time()` mismatch oltre tolleranza | 0 | 0 |
| Gap finale `get_time` vs monotonic | 1 ms | 0 ms |
| `nxtimerdiag failures` | 0 | 0 |

Il clock monotono e `get_time()` sono coerenti; il problema residuo misurato è la latenza/jitter dei risvegli, non una regressione del clock. Le medie sono superiori ai 33 ms richiesti e i massimi arrivano a 143 ms su AArch64.

### Stato SMP

- AMD64: i tre secondary CPU 1, 2 e 3 hanno tutti scritto `Secondary CPU ... online and ready`; shell e diagnostica OK.
- AArch64: il BSP ha stampato timeout dell'ack per CPU 1 e CPU 2; CPU 3 è stato confermato online. Il guest ha comunque raggiunto NXShell e completato timerdiag. Nei log sono presenti attività di timer su CPU1/CPU2, ma non il messaggio di ack/online per loro: non considero certificato il bring-up a 4 CPU. Va indagato il percorso `smp_bringup_secondary()` / `kernel_secondary_main()` e la pubblicazione/visibilità di `cpu_data[cpu].idle_task`; non aumentare semplicemente il timeout senza aver trovato la causa.

### Diagnostica compositor: causa della latenza individuata

I nuovi log attribuiscono la latenza a rendering/lock, non solo a contesa:

- AMD64: `render_hold` circa 34.7–72.3 ms; `lock_wait` trascurabile nei campioni osservati.
- AArch64: `render_hold` circa 40.7–130.0 ms; `lock_wait` fino a circa 33.7 ms.

La lettura del codice spiega il rischio: `compositor_render()` acquisisce `compositor_lock` con `spin_lock_irqsave()`, chiama sincronicamente `compositor_render_internal()` (passi di occlusione, raster CPU pixel-per-pixel, chrome, upload/present) e rilascia la lock soltanto alla fine. Quindi gli IRQ locali restano mascherati per l'intera durata `render_hold`; questo può ritardare timer, input e scheduling. Il trylock in `compositor_tick()` evita di attendere una lock occupata, ma non risolve il caso del rendering sincrono con IRQ disabilitati.

Non ho applicato una modifica superficiale sostituendo `spin_lock_irqsave` con `spin_lock`: gli handler IRQ del mouse e altre mutazioni del compositor usano la stessa lock e potrebbero deadlockare se un IRQ la interrompesse sullo stesso core. La correzione corretta richiede separare la breve sezione di protezione dello stato da un frame snapshot/ownership stabile, oppure deferire gli aggiornamenti IRQ; va progettata e testata come refactor dedicato.

### Stato effettivo

La correzione SEV e il single-owner PID1 sono ora compilati e boot-testati. Il diagnostico aggiornato è stato eseguito su entrambe le architetture. Restano aperti: handshake AArch64 CPU1/CPU2; riduzione della sezione critica del compositor e del jitter; prova di soak 60 s e stress combinato. `time()` POSIX in secondi resta follow-up separato. Nessun file è stato staged o committato.


## Correzione handshake SMP e soak — 2026-11-03, terza passata

### Causa del timeout AArch64 e fix

La seconda prova ha isolato una race nel percorso condiviso `kernel_secondary_main()` (`kernel/main.c`): il core secondario eseguiva `local_irq_enable()` prima di `smp_ack_boot(cpu)`. Il timer per-CPU era già armato da `timer_init_percpu()`. Un timer IRQ poteva quindi preemptare il boot path immediatamente dopo l'abilitazione degli IRQ e prima della pubblicazione dell'ack; il BSP continuava ad attendere un ack che il percorso interrotto non aveva ancora inviato.

**Fix applicato:** dopo l'acquire-load che conferma la pubblicazione dell'idle task e dopo l'inizializzazione hardware per-CPU, il secondario ora chiama `smp_ack_boot(cpu)` e solo dopo `local_irq_enable()`. Il release-store pubblica le inizializzazioni; il BSP può procedere senza esporre il boot path alla preemption prima dell'ack.

### Verifica dopo il fix

- AArch64, boot SMP=4: BSP ha confermato `CPU 1 online`, `CPU 2 online`, `CPU 3 online`; nessun messaggio `failed to acknowledge boot`. NXShell pronta, KTEST 8/8, `nxtimerdiag` 160 campioni e `failures=0`, `faults=0`.
- AMD64, ricostruito dopo la stessa modifica condivisa: `Secondary CPU 1/2/3 online and ready`; KTEST 8/8, `nxtimerdiag` 160 campioni e `failures=0`, `faults=0`.
- AMD64 timer dopo il fix: OS1_sleep media 42.15 ms, max 84.32 ms; nanosleep media 39.41 ms, max 61.94 ms; `get_time()` gap finale 1 ms, zero regressioni/mismatch.
- AArch64 timer dopo il fix: OS1_sleep media 46.95 ms, max 132.91 ms; nanosleep media 49.25 ms, max 121.63 ms; `get_time()` gap finale 0 ms, zero regressioni/mismatch.

### Soak di shell

Entrambi i guest sono rimasti attivi per circa 60 secondi dopo i test timer. AMD64 ha completato sei richieste NXShell distanziate di 10 secondi e ha riportato `shell=yes`, `faults=0`. AArch64 ha completato sei richieste NXShell `echo alive` distanziate di 10 secondi e ha riportato `shell=yes`, `faults=0`. Un primo tentativo AArch64 concorrente con AMD64 ha avuto una riga di comando digitata in modo errato dal livello QMP; non è stato conteggiato come test superato ed è stato ripetuto in modo seriale con comandi semplici.

Entrambi i guest sono stati arrestati con `nxadb stop`; `nxadb status` conferma `running=False` per entrambe le architetture.

### Restano aperti

La race di ack SMP è stata corretta e verificata su entrambe le architetture. Restano il refactor del compositor per evitare IRQ mascherati durante `compositor_render_internal()` (snapshot/ownership dei buffer e protezione dei mutatori), la riduzione del jitter del timer e lo stress combinato di applicazioni. Il soak di shell non equivale ancora allo stress combinato richiesto. Il bug POSIX `time()` millisecondi/secondi resta separato.
