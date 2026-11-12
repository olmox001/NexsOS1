# NASA/JPL — The Power of 10

Riferimento: Gerard J. Holzmann, NASA/JPL Laboratory for Reliable Software, “The Power of 10: Rules for Developing Safety-Critical Code”, IEEE Computer, giugno 2006.

Fonti:
- PDF dell'articolo: https://www.state-machine.com/doc/NASA-JPL06c.pdf
- Scheda NASA Software Engineering Handbook, riferimento SWEREF-417: https://swehb.nasa.gov/pages/viewpage.action?navigatingVersions=true&pageId=16452550

## Le dieci regole

1. **Control flow semplice:** niente `goto`, `setjmp`, `longjmp`, ricorsione diretta o indiretta.
2. **Loop con limite superiore fisso:** il limite deve essere dimostrabile staticamente; un ciclo senza limite verificabile non è conforme.
3. **Allocazione dinamica:** non allocare dinamicamente dopo l'inizializzazione.
4. **Funzioni brevi:** una funzione deve poter essere esaminata su una pagina standard; usare circa 60 righe come soglia operativa iniziale, includendo le dichiarazioni secondo la regola originale.
5. **Asserzioni:** densità media di almeno due asserzioni per funzione; devono verificare invarianti reali, essere prive di effetti collaterali e non sostituire la gestione degli errori.
6. **Scope minimo:** dichiarare ogni dato nel più piccolo ambito possibile.
7. **Contratti e risultati:** ogni chiamante controlla il risultato delle funzioni non-void; ogni funzione valida i parametri ricevuti.
8. **Preprocessore limitato:** inclusioni e macro semplici; evitare token-pasting, macro variadiche/ricorsive e compilazione condizionale non necessaria.
9. **Puntatori limitati:** massimo un livello di dereferenziazione secondo la formulazione originale; niente dereferenziazioni nascoste in macro/typedef e niente function pointer.
10. **Warning e analisi:** warning al livello più pedante, zero warning; analisi statica giornaliera con uno o più strumenti e zero findings non giustificati.

## Applicazione a NexsOS1

Queste regole sono un requisito di progettazione e verifica, non una dichiarazione automatica che tutto il codice legacy sia già conforme.

- Le nuove interfacce SDK, i manifest di build, il codice di packaging e i nuovi moduli devono essere progettati per la conformità.
- Il codice esistente viene censito e portato a conformità per dominio, senza nascondere o sopprimere warning.
- Le caratteristiche essenziali di un sistema operativo (allocator, scheduler, driver, code asincrone) possono entrare in conflitto con l'interpretazione letterale di alcune regole. Non sono esenzioni automatiche: ogni deviazione deve avere ID, motivazione tecnica, superficie limitata, mitigazione, test e approvazione esplicita.
- Per la Rule 3, l'architettura deve distinguere allocazione durante bootstrap da allocazione runtime. Se il runtime dinamico è necessario, il dominio deve dimostrare pool preallocati/bounded, limiti, ownership, failure path e assenza di crescita non controllata; la deviazione resta registrata, non viene chiamata conformità letterale.
- Per la Rule 9, le API generiche di sistema e i callback non vanno riscritti con alias fittizi: occorre progettare interfacce tipizzate e dispatch esplicito dove necessario. Le eccezioni indispensabili (ABI C, driver/provider) devono essere isolate e verificate.
- Nessun uso di `-Wno-*`, cast a void, macro di soppressione o esclusione dal checker per far apparire verde un gate.

## Finding legacy già visibili (non è ancora un audit completo)

1. **Rule 1 — setjmp/longjmp:** entrambi gli assembly `user/arch/*/syscall.S` implementano i simboli `setjmp` e `longjmp`; `include/api/setjmp.h` li dichiara. Il commento dell'header conferma che Lua li usa per `lua_pcall`/protezione errori. La regola originale vieta esplicitamente setjmp/longjmp. Non rimuoverli senza una soluzione di compatibilità reale: NX-SDK deve isolare questa superficie e registrare una decisione/deroga esplicita oppure sostituire i call-site incompatibili.
2. **Rule 8 — preprocessore:** `include/abi/posix_types.h` definisce macro non semplici, tra cui `container_of` con statement expression e `__typeof__`, oltre a macro di allineamento e memory barrier. La superficie ABI deve essere separata dagli helper kernel e il preprocessore va ridotto nei nuovi header pubblici.
3. **Rule 10 — warning:** `NX_DIAG_WARNINGS` è disabilitato per default; quando abilitato, `-Wconversion` e `-Wsign-conversion` sono impostati con `-Wno-error`. Alcuni target upstream/overlay aggiungono `-Wno-error`. Il gate attuale non dimostra quindi warning-zero con il profilo più pedante per tutti i moduli.
4. **Rule 3 — allocazione:** kernel e userland hanno allocator (`kmalloc`/`malloc`); il loro uso dopo init non è ancora censito. Non dichiarare conformità finché lifetime, pool, limiti e fasi di inizializzazione non sono verificati.
5. **Rule 2, 4, 5, 6, 7 e 9:** non ancora sottoposte a scansione completa. Le asserzioni ABI compile-time non sostituiscono il requisito di densità delle asserzioni runtime della Rule 5.

Questi finding descrivono il codice attuale osservato e non giustificano eccezioni automatiche. Ogni deroga deve essere valutata singolarmente.

## Evidenza richiesta per ogni modulo

- Configurazione versionata del checker e toolchain.
- Report per regola con file, simbolo, riga, severità, motivazione e stato.
- Eccezioni tracciate e approvate, mai implicite.
- Build con warning attivi su tutte le architetture target.
- Test unitari/host, integrazione e boot shell dove applicabile.
- Zero errori e zero warning non giustificati prima del gate di release.

La conformità va dichiarata per commit, modulo, linguaggio e profilo di build. Non usare la frase “NASA compliant” senza un report riproducibile che specifichi esattamente il perimetro verificato.
