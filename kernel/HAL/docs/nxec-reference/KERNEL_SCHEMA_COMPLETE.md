# KERNEL PROJECT — SCHEMA OPERATIVO COMPLETO E VERIFICATO
## Documento Fondativo per Costruzione Incrementale, Auto-Miglioramento e Verifica Continua

**Versione**: 1.0.0-FOUNDATION  
**Data**: 2026-11-05  
**Stato**: PRONTO ALL’USO DA LLM/AGENTE  
**Licenza codice esterno ammesso**: MIT / BSD / ISC / Apache-2.0 (solo codice già esistente e verificabile)  
**Linguaggio primario**: C moderna (C17/C23 subset, NASA Power of 10 + estensioni documentate)  
**Obiettivo primario**: Kernel pulito, tecnicamente rigoroso, ultra-sicuro, ultra-veloce, compatibile con il massimo hardware e software esistente, facile da comprendere e controllare da umani e da LLM, riproducibile su qualsiasi macchina con risorse minime.

---

## 0. PRINCIPI ASSOLUTI (NON NEGOZIABILI)

1. **Nessuna semplificazione**. Ogni problema deve essere affrontato, catalogato e risolto o esplicitamente deferito con piano.
2. **NASA Power of 10 Rules** applicate a tutto il codice del kernel e dei moduli critici (vedi Sezione 12).
3. **Zero-copy** come regola primaria di trasferimento dati.
4. **Capability-based security** end-to-end (ogni risorsa è un capability mascherato).
5. **Ogni bit è catalogato, mascherato e accessibile zero-copy**.
6. **Tutto in userland tranne HAL + Core minimo** (driver, VFS, compositor, network, filesystem = moduli ELF userland).
7. **Auto-miglioramento obbligatorio**: ogni modifica deve produrre test, metriche, catalogazione e confronto prestazionale/logico.
8. **Instrumentazione totale**: il sistema deve essere debuggabile e controllabile dall’interno da un LLM/agente.
9. **Riproducibilità totale**: chiunque, con risorse minime, deve poter scaricare, compilare e testare.
10. **Catalogazione LLM-ready**: ogni decisione, problema, test e miglioramento deve essere scritto in forma strutturata e direttamente sfruttabile da qualsiasi LLM futuro.

---

## 1. STRUTTURA DIRECTORY DEL PROGETTO (OBBLIGATORIA)

```
project-root/
├── README.md                          # Entry point umano + LLM
├── LICENSE
├── docs/                              # Tutta la documentazione generata
│   ├── SCHEMA.md                      # Questo documento (copia master)
│   ├── HCD_SPEC.md
│   ├── ABI_SPEC.md
│   ├── SYSCALL_SPEC.md
│   ├── CAPABILITY_MODEL.md
│   ├── TEST_CATALOG.md
│   ├── PROBLEM_CATALOG.md
│   ├── PERFORMANCE_LOG.md
│   └── DECISIONS.md                   # Decision log append-only
├── tools/
│   ├── setup-toolchain.sh             # Unico script entry-point
│   ├── setup-linux.sh
│   ├── setup-macos.sh
│   ├── qemu-runner.sh
│   ├── test-runner.sh
│   └── adb/                           # Adaptive Debug & Benchmark structures
├── mk/                                # Makefile e regole di build
│   ├── common.mk
│   ├── arch-amd64.mk
│   ├── arch-arm64.mk
│   └── modules.mk
├── grub/                              # Configurazioni e dipendenze GRUB/Limine
├── preboot/
│   ├── amd64/                         # Bootstrap 32-bit → Long Mode
│   ├── arm64/                         # Bootstrap AArch64 nativo
│   └── common/                        # HCD generation shared code
├── kernel/
│   ├── Hal/                           # Hardware Abstraction Layer + Hypervisor leggero
│   │   ├── isa/                       # Estensioni ISA attivabili
│   │   ├── platform/                  # Driver platform isolati + Device Tree registry
│   │   └── virt/                      # Virtualizzazione (se supportata)
│   ├── core/                          # Il nucleo minimo (SMP, schedule, paging, RAM, cache, capability)
│   │   ├── smp/
│   │   ├── schedule/
│   │   ├── memory/                    # Paging a blocchi + masks + capability
│   │   ├── capability/
│   │   ├── buffer/                    # Zero-copy buffer pool
│   │   └── vm/                        # La "gigantesca VM" basata su array di registri catalogati
│   ├── lib/kernel/                    # Libreria kernel-side (zero-copy helpers, assert, logging)
│   └── module/                        # Loader ELF + interfaccia moduli
├── modules/                           # Tutti i moduli userland (ELF)
│   ├── libriver/                      # Compatibility layer driver Linux/NetBSD
│   ├── lib/                           # libc usermode di sistema
│   ├── extensionlib/
│   ├── compositor/
│   ├── vfs/
│   ├── envreg/                        # System registry
│   └── ...
├── tests/
│   ├── unit/
│   ├── integration/
│   ├── hardware/
│   ├── performance/
│   └── adb/                           # Adaptive Debug Blocks
└── external/                          # Codice MIT/BSD importato (con checksum e licenza)
```

---

## 2. PRE-KERNEL E HARDWARE CAPABILITY DESCRIPTOR (HCD)

### 2.1 Principio
Il pre-kernel **non** è un mini-kernel. È un rilevatore e preparatore. Produce un **Hardware Capability Descriptor (HCD)** normalizzato, versionato, indipendente dall’ISA, che il kernel verifica prima di qualsiasi attivazione.

### 2.2 Bootstrap ISA-specifici (obbligatori)

| Architettura | Bootstrap Mode          | Rilevamento ISA/CPU          | Fonte piattaforma      | Note critiche |
|--------------|-------------------------|------------------------------|------------------------|---------------|
| AMD64        | x86-32 → Long Mode     | CPUID (leaf 0,1,7,0x80000001+) | ACPI + UEFI Memory Map | Non assumere UEFI 32-bit universale |
| ARM64        | AArch64 nativo         | ID registers (MIDR, ID_AA64*) | Device Tree + ACPI     | **Vietato** forzare AArch32 |

### 2.3 Struttura HCD (versione 1)

```c
/* docs/HCD_SPEC.md contiene la definizione completa e versionata */

#define HCD_MAGIC          0x48434431  /* "HCD1" */
#define HCD_VERSION_MAJOR  1
#define HCD_VERSION_MINOR  0

struct hcd_header {
    uint32_t magic;
    uint16_t version_major;
    uint16_t version_minor;
    uint64_t total_size;
    uint64_t checksum;                 /* CRC64 o BLAKE3 del payload */
    uint32_t architecture;             /* 1=AMD64, 2=ARM64, ... */
    uint32_t flags;
};

struct cpu_information {
    uint32_t vendor_id[3];
    uint32_t family, model, stepping;
    uint32_t features_supported[16];   /* bitmap estensioni */
    uint32_t features_enabled[16];
    uint32_t features_usable[16];      /* dopo verifica CR4/XCR0/etc */
    /* ... topologia, cache, MMU, virtualization ... */
};

struct hardware_capability_descriptor {
    struct hcd_header header;
    struct cpu_information cpu;
    struct cpu_topology topology;
    struct cache_information cache;
    struct memory_information memory;  /* ranges fisici, tipi, NUMA */
    struct interrupt_information interrupts;
    struct vector_information vectors;
    struct virtualization_information virtualization;
    struct device_tree_information devices; /* puntatore a DTB o ACPI tables */
    struct firmware_information firmware;
    /* Estensioni future con versioning */
};
```

**Regole HCD**:
- Dimensioni e allineamenti fissi, indipendenti dalla dimensione del puntatore dell’architettura produttrice.
- Distinzione esplicita: SUPPORTED / ENABLED / USABLE.
- Su CPU eterogenee: capacità per-core e intersezione globale.
- Il kernel **deve** verificare magic, version, checksum e requisiti minimi prima di usare qualsiasi bit.

### 2.4 Flusso di avvio unificato

```
FIRMWARE (UEFI/ACPI/DTB)
        │
        ├── AMD64 ──► pre-kernel (32-bit) ──► CPUID + ACPI/UEFI ──► HCD
        │
        └── ARM64 ──► pre-kernel (AArch64) ──► ID regs + DTB/ACPI ──► HCD
                │
                ▼
        HARDWARE REPORT (HCD)
                │
                ▼
        KERNEL LOADER (comune)
                │
                ├── verifica HCD
                ├── attiva solo estensioni USABLE
                ├── inizializza MMU/paging a blocchi
                └── passa al core
```

---

## 3. ARCHITETTURA DEL KERNEL

### 3.1 HAL (Hardware Abstraction Layer + Hypervisor leggero)

- Contiene **solo** i driver platform strettamente necessari (timer, interrupt controller, early console, MMU setup).
- Device Tree / ACPI catalogato in un **registro isolato**.
- Estensioni ISA (AVX, SVE, SME, crypto, etc.) attivabili a boot secondo HCD e **cambiabili a runtime in sicurezza** (con barrier e capability revocation).
- Isolamento: ogni piattaforma driver gira con capability ridotte.

### 3.2 Core (il nucleo minimo)

Il core è una **gigantesca macchina virtuale basata su array di registri catalogati**.

Concetti fondamentali:
- **Registro catalogato**: ogni risorsa (pagina, buffer, device, thread, capability) è un entry in un array indexato.
- **Puntatore mascherato zero-copy**: un handle che contiene index + rights + generation + mask. Dereferenziazione avviene solo attraverso il core.
- **Paging a blocchi + masks + capability**: non page table tradizionali pure, ma blocchi con bit di capability e mask.
- **Zero-copy buffer pool**: pool di buffer pre-allocati (NASA Rule 3) gestiti dal core.
- **SMP + Preemptive Schedule**: scheduler preemptive, per-core runqueue, capability di scheduling.
- **Accesso alle risorse**: solo tramite protocollo comune ultra-rapido (message + capability transfer zero-copy).

### 3.3 Module Loader e Userland

- Tutti i moduli (driver, VFS, compositor, network, etc.) sono **ELF userland**.
- Il core fornisce syscall minime e capability.
- **libriver**: layer di compatibilità che mappa l’API usermode del nostro kernel verso un’interfaccia simile a Linux/NetBSD, permettendo di portare driver Linux esistenti come programmi isolati.
- libc usermode + extensionlib.
- envreg: registro di sistema capability-protected.

### 3.4 Sicurezza e Isolamento

- Capability unforgeable (generation counter + rights mask).
- Revocation esplicita.
- Ogni modulo vede solo le capability che gli sono state concesse.
- No shared mutable state non capability-protected.
- HAL isolato dal resto.

---

## 4. TOOLCHAIN E RIPRODUCIBILITÀ

### 4.1 setup-toolchain.sh

Deve supportare:
- Linux: Ubuntu LTS, Debian stable, Fedora, Arch (package manager nativo)
- macOS: Intel e Apple Silicon (Homebrew)

Fornisce:
- Compilatori cross (clang/llvm preferito, gcc fallback)
- QEMU (con KVM se disponibile, TCG altrimenti)
- GDB multi-arch
- limine o GRUB
- clang-tidy, cppcheck, e strumenti static analysis
- Ambiente isolato (directory tools/env/)

**Regola**: una volta eseguito `./tools/setup-toolchain.sh`, il progetto deve compilare e testare senza dipendenze esterne aggiuntive.

### 4.2 Build

```bash
# Esempio target
make ARCH=amd64          # o arm64
make test
make qemu
make modules
```

Build riproducibile: hash dei binari, record delle versioni toolchain.

---

## 5. PROCESSO DI AUTO-MIGLIORAMENTO (OBBLIGATORIO PER L’AGENTE)

L’agente LLM che lavora su questo schema **deve** seguire questo loop per ogni modifica:

1. **Identifica problema o opportunità** → cataloga in `docs/PROBLEM_CATALOG.md`
2. **Studia alternative** (almeno 2-3 approcci, inclusi codice MIT/BSD esistente)
3. **Confronta logicamente e prestazionalmente** (microbenchmark + test)
4. **Implementa** seguendo NASA Power of 10 + regole di questo documento
5. **Aggiunge test incrementali e dinamici** (unit + integration + adb)
6. **Instrumenta** (log, counters, capability audit)
7. **Verifica** che l’architettura rimanga stabile
8. **Cataloga decisione** in `docs/DECISIONS.md` (append-only)
9. **Aggiorna performance log**
10. **Produci patch e aggiorna schema se necessario**

**Vietato**:
- Scegliere la soluzione “più semplice” senza confronto
- Lasciare codice non instrumentato
- Rimuovere test esistenti
- Introdurre dipendenze dinamiche dopo init (NASA Rule 3)

### 5.1 Adaptive Debug & Benchmark (ADB) Structures

Strutture create appositamente in `tools/adb/` e `tests/adb/`:

- Blocchi di test che possono essere abilitati/disabilitati a runtime
- Contatori di performance zero-overhead quando disabilitati
- Hook di debug controllabili dal core
- Snapshot di stato capability e memoria
- Test di stress dinamici (fault injection, concurrent access, capability revocation)

---

## 6. NASA POWER OF 10 RULES (APPLICATE RIGOROSAMENTE)

1. Control flow semplice (no goto, no setjmp/longjmp, no recursion)
2. Tutti i loop con upper bound fisso e verificabile staticamente
3. **Nessuna allocazione dinamica dopo l’inizializzazione**
4. Funzioni ≤ ~60 linee (una schermata)
5. Densità di assertion ≥ 2 per funzione (side-effect free)
6. Scope minimo per ogni dato
7. Controllo obbligatorio di ogni return value e di ogni parametro
8. Preprocessor limitato a #include e macro semplici
9. Puntatori: al massimo un livello di dereferenziamento; no function pointer non controllati
10. Compilazione con tutti i warning attivi; zero warning ammessi

**Estensioni documentate**:
- Capability generation e rights check obbligatori su ogni accesso
- Zero-copy path esplicitamente marcati
- Tutti i buffer da pool pre-allocato

---

## 7. COMPATIBILITÀ DRIVER E SOFTWARE ESISTENTE

- **libriver**: layer di traduzione API
  - Mappa le nostre syscall/capability verso un’interfaccia che permette di compilare driver Linux/NetBSD con modifiche minime
  - Ogni driver diventa un processo ELF isolato
  - Device Tree / ACPI → capability registry

- Supporto ELF completo (loader nel core)
- Possibilità futura di POSIX compatibility layer come modulo

---

## 8. GESTIONE DELLA MEMORIA E ZERO-COPY

- Paging basato su **blocchi** (non necessariamente 4KiB tradizionali)
- Ogni blocco ha mask di capability e diritti
- Buffer pool pre-allocati al boot (dimensione configurabile da HCD)
- Trasferimento dati: passaggio di capability di buffer, mai copia se evitabile
- Accesso zero-copy da userland tramite puntatori mascherati verificati dal core

---

## 9. INSTRUMENTAZIONE E CONTROLLO DA LLM

Il sistema deve esporre (tramite capability speciali di debug):

- Snapshot completo dello stato dei registri catalogati
- Contatori di performance e di errori
- Log strutturato (append-only, capability-protected)
- Possibilità di iniettare fault e di forzare percorsi
- Interfaccia per aggiungere nuovi test ADB a runtime (in fase di sviluppo)

Questo permette all’agente di “vedere” e controllare il sistema senza perdere contesto.

---

## 10. CATALOGAZIONE LLM-READY (FORMATO OBBLIGATORIO)

Ogni file in `docs/` deve seguire un formato strutturato:

```markdown
## ID: PROBLEM-2026-11-05-001
## Titolo: ...
## Stato: OPEN | RESOLVED | DEFERRED
## Impatto: SAFETY | PERFORMANCE | COMPATIBILITY | COMPLEXITY
## Descrizione:
## Analisi alternative:
## Decision:
## Test aggiunti:
## Metriche prima/dopo:
## Riferimenti codice:
```

Stesso approccio per DECISIONS, PERFORMANCE_LOG, TEST_CATALOG.

---

## 11. SUPPORTO ARCHITETTURE FUTURE

L’HCD e l’ABI del core sono progettati per essere estendibili.
Aggiungere una nuova ISA richiede solo:
1. Nuovo pre-kernel ISA-specifico che produce HCD
2. HAL platform drivers
3. Aggiornamento delle tabelle di feature in HCD

Il core rimane identico.

---

## 12. CHECKLIST DI VERIFICA INIZIALE (DA ESEGUIRE PRIMA DI QUALSIASI CODICE)

- [ ] Directory structure creata esattamente come specificato
- [ ] `tools/setup-toolchain.sh` funziona su Ubuntu e macOS
- [ ] HCD_SPEC.md completo con strutture C allineate
- [ ] Pre-kernel AMD64 e ARM64 minimali che producono HCD valido
- [ ] Core che verifica HCD e rifiuta se invalido
- [ ] Almeno un test ADB che controlla zero-copy e capability
- [ ] Compilazione con `-Wall -Wextra -Werror -Wpedantic` e zero warning
- [ ] Documentazione di ogni decisione

---

## 13. PROSSIMI PASSI OPERATIVI PER L’AGENTE

1. Creare la struttura directory esatta
2. Implementare `tools/setup-toolchain.sh` (Linux + macOS)
3. Scrivere `docs/HCD_SPEC.md` completo
4. Implementare pre-kernel AMD64 minimo (32-bit → Long Mode + CPUID → HCD)
5. Implementare pre-kernel ARM64 minimo (AArch64 + ID regs → HCD)
6. Core stub che riceve e verifica HCD
7. Primo test ADB + QEMU
8. Catalogare ogni problema incontrato

**Ricorda**: non semplificare. Confronta. Cataloga. Instrumenta. Testa. Migliora.

---

**Fine del documento fondativo.**  
Questo schema è progettato per resistere alla crescita enorme del progetto e alla perdita di memoria degli agenti LLM, fornendo un’ancora stabile, verificabile e auto-documentante.


---

# PARTE II — ESTENSIONI ARCHITETTURALI APPROVATE
## Versione 1.1.0-ARCHITECTURE-BASELINE
**Data di integrazione:** 2026-11-05
**Autorità di progetto:** AGENT-00
**Stato complessivo:** BASELINE ARCHITETTURALE IN DEFINIZIONE — non equivale a implementazione o verifica.

Questa parte integra le decisioni successive alla fondazione 1.0.0. Non cancella né riscrive i requisiti precedenti. In caso di apparente conflitto, si apre un elemento di analisi: nessun requisito viene eliminato tacitamente. Le soluzioni devono preservare simultaneamente i requisiti oppure documentare con precisione il limite fisico e il comportamento sicuro.

### 14. PRINCIPI DI RISOLUZIONE DEI CONFLITTI

#### 14.1 Compatibilità simultanea dei requisiti

Un conflitto tra requisiti non autorizza a rimuoverne uno. Ogni conflitto deve essere trattato con una scheda verificabile:

- identificativo del conflitto;
- requisiti coinvolti e loro formulazione normativa;
- vincolo tecnico o fisico effettivo;
- dominio interessato (tempo, memoria, privilegi, rappresentazione, concorrenza, hardware);
- meccanismo proposto per soddisfare simultaneamente i requisiti;
- invarianti che impediscono regressioni;
- modalità di errore e recupero;
- costo massimo di risorse, quando dimostrabile;
- test, stress test e prove richieste;
- stato: OPEN, DESIGNED, IMPLEMENTED, VERIFIED oppure BLOCKED.

Non si dichiara risolto un conflitto sulla base di un diagramma o di una spiegazione. DESIGNED indica soltanto una proposta architetturale coerente; VERIFIED richiede evidenze riproducibili.

#### 14.2 Limiti fisici e funzionalità

Il progetto deve ricercare sistematicamente percorsi alternativi per ottenere la massima funzionalità compatibile con le risorse reali. Non si presume che ogni limite sia superabile: limiti fisici assoluti non possono essere annullati da una scelta algoritmica. In tali casi il sistema deve identificare il limite, offrire un percorso sostitutivo se esiste e mantenere uno stato sicuro e comprensibile.

#### 14.3 Sicurezza non degradabile

Nessun profilo ridotto, fallback, emulazione, migrazione o modalità di emergenza può:
- aggirare il Capability Engine;
- trasformare un'operazione non autorizzata in autorizzata;
- ampliare implicitamente diritti o ambito di una capability;
- ignorare revoche, controlli di integrità o isolamento;
- abbassare i requisiti di autenticità e validazione dei dati.

La riduzione di qualità, risoluzione, throughput o prestazioni è possibile soltanto se non modifica la semantica di sicurezza e se le differenze rilevanti sono comunicate all'utente.

### 15. CONTINUITÀ FUNZIONALE ADATTIVA — AERM

#### 15.1 Requisito

**REQ-AERM-001 — Continuità funzionale adattiva**

Quando il percorso primario non è eseguibile per indisponibilità o insufficienza di risorse, il sistema deve cercare percorsi alternativi compatibili con l'obiettivo dell'operazione. Deve preferire, nell'ordine determinato dalla policy applicabile, un'implementazione sostitutiva equivalente, una modalità con qualità/prestazioni ridotte, un'esecuzione differita e infine un errore controllato. L'ordine effettivo è specifico per classe di operazione e non può violare sicurezza, integrità o autorizzazioni.

#### 15.2 Adaptive Execution & Resource Manager

AERM è un componente logico coordinatore, non un'autorità di sicurezza e non un bypass del kernel. Le responsabilità sono separate:

1. **Resource Monitor:** rileva capacità, pressione, disponibilità e riserve.
2. **Alternative Strategy Engine:** consulta il catalogo e individua alternative compatibili.
3. **Approval & Policy Manager:** applica autorizzazioni e raccoglie approvazioni.
4. **Execution & Recovery Manager:** attiva, osserva, termina o ripristina il percorso selezionato.

Il Capability Engine rimane indipendente e verifica le autorizzazioni nel punto di accesso effettivo alle risorse. AERM non può creare, derivare o ampliare capability per il solo fatto di selezionare un fallback.

#### 15.3 Politica di approvazione D — sistema misto

- Alternativa già approvata e configurata: attivazione automatica entro l'ambito autorizzato.
- Alternativa nuova: richiesta di approvazione.
- Modifica sensibile del risultato, qualità o semantica: richiesta di approvazione.
- Effetto non reversibile: conferma esplicita prima dell'esecuzione.
- Alternativa non sicura o non autorizzata: rifiuto senza deroga.
- Ripristino della modalità primaria: automatico solo se preventivamente autorizzato; altrimenti proposta all'utente.

L'autorizzazione è circoscritta a identificativo/versione dell'alternativa, risorsa, modalità primaria, differenze, costi, rischi, reversibilità e ambito (operazione, sessione o regola persistente). È revocabile. Un'approvazione per una classe di fallback non autorizza fallback diversi.

#### 15.4 Contratto di alternativa

Ogni strategia alternativa deve dichiarare almeno:
- strategy_id e versione;
- operazione/risorsa di riferimento;
- implementazione primaria e alternativa;
- prerequisiti hardware e software;
- compatibilità funzionale e limiti noti;
- differenze di qualità, precisione, latenza e throughput;
- consumo e riserve richieste;
- capability necessarie;
- reversibilità, cancellazione e recupero;
- policy di approvazione;
- test e misure che ne supportano le dichiarazioni.

La degradazione non deve bloccare il normale svolgimento delle attività quando esiste un'alternativa sicura. Le alternative possono ridurre qualità dell'immagine o aumentare il tempo di completamento, ma devono preservare le proprietà funzionali dichiarate. Le operazioni che richiedono esattezza (sicurezza, integrità, contabilità delle risorse, transazioni) non possono essere sostituite da approssimazioni non autorizzate.

### 16. GESTIONE GERARCHICA DELLE RISORSE

**Decisione RES-001: D — combinazione gerarchica A+B+C, con regole differenziate per tipo di risorsa.**

Livelli:
- **R0 — Risorse preallocate:** percorso preferenziale, dimensionato e verificato in fase di inizializzazione.
- **R1 — Pool preparati:** riserve già create e validate, attivabili tramite protocollo bounded.
- **R2 — Acquisizione controllata:** ammessa soltanto per categorie e fasi per cui il modello di allocazione è stato formalmente verificato.

La classificazione si applica separatamente a pagine, buffer, thread, handle, oggetti kernel, DMA, code, descrittori, stack e risorse dei dispositivi. Non esiste un'autorizzazione globale a espandere ogni tipo di risorsa.

**Vincolo NASA Rule 3:** la regola fondativa vieta allocazioni dinamiche dopo l'inizializzazione. R2 non è dichiarato compatibile per definizione. Prima dell'implementazione, ogni risorsa deve essere classificata come: allocata durante init; pool preallocato attivato dopo init; risorsa hardware acquisita senza allocazione heap; oppure allocazione dinamica vera e propria. Se la semantica di Rule 3 vieta quest'ultima, R2 deve essere limitato a fasi controllate prima dell'ingresso nello stato runtime oppure a meccanismi che non costituiscono allocazione dinamica secondo una definizione documentata e approvata. Nessuna reinterpretazione nominale è ammessa.

Ogni livello richiede quote, limiti superiori, controllo di overflow, backpressure, timeout ove applicabile, cancellazione sicura e comportamento deterministico in esaurimento. Le code e le riserve sono bounded. Non si attende indefinitamente una risorsa e non si mantiene un lock mentre si attende un evento esterno.

### 17. CATALOGO CENTRALIZZATO DI COMPATIBILITÀ

**Decisione COMP-001: D — catalogo gerarchico distribuito.**

La centralizzazione riguarda definizioni canoniche, identificativi, policy, versioni e dipendenze; non impone che ogni lettura runtime acceda a un singolo punto condiviso.

#### 17.1 Livelli

- **Master Compatibility Registry (MCR):** fonte canonica delle definizioni, degli schemi, delle versioni e delle relazioni.
- **Cataloghi di sottosistema:** viste specializzate (CPU/ISA, memoria, scheduler, dispositivi, moduli, sincronizzazione, algoritmi).
- **Cataloghi locali per CPU:** snapshot locali versionati delle capacità e delle implementazioni effettivamente utilizzabili da quel core.
- **Cataloghi di esecuzione:** selezione delle implementazioni ammesse per una specifica operazione e contesto.

Le viste specializzate non diventano fonti indipendenti di verità. Ogni record mantiene un identificativo canonico, versione, provenienza e generazione del catalogo di origine.

#### 17.2 Classi di compatibilità

Le classi iniziali sono un asse descrittivo e possono essere combinate:
- **K0:** indipendente dall'architettura.
- **K1:** specifica per architettura, con implementazioni dedicate (es. AMD64/ARM64).
- **K2:** richiede specifiche feature CPU.
- **K3:** usa acceleratore o hardware dedicato.
- **K4:** dispone di fallback software compatibile.
- **K5:** opzionale e condizionata dalle risorse.
- **K6:** privilegiata, richiede capability esplicite.

Esempio: K1+K4 identifica una funzione con backend specifici per ISA e un fallback software. Le classi non sono una certificazione automatica: ogni combinazione deve essere supportata da test e requisiti espliciti.

#### 17.3 Schema minimo del record

Ogni funzione/algoritmo deve registrare: ID stabile; versione; classe K; ISA e feature richieste; feature solo opzionali; prerequisiti MMU/firmware/dispositivo; allineamento e formato dati; memoria minima/massima; modalità sync/async; requisiti di contesto (thread, interrupt, early boot); capability richieste; limiti bounded; fallback e condizioni; semantica di errore; proprietà di concorrenza; misure e condizioni di benchmark; test; provenienza/licenza per codice esterno.

Il catalogo descrive compatibilità e selezione, ma non concede diritti. La verifica delle capability è sempre eseguita dal componente di sicurezza competente.

### 18. CORE PREVALENTEMENTE ASINCRONO

**Decisione CORE-ASYNC-001:** progettare il Core per massimizzare l'esecuzione asincrona e non bloccante, senza rendere asincrone artificialmente le operazioni che richiedono semantica atomica o una sezione critica breve.

#### 18.1 Modello di richiesta

Il percorso generale è:
1. validazione dei parametri e dell'identità dell'operazione;
2. verifica delle capability e delle policy;
3. admission control e riserva/associazione delle risorse;
4. selezione del backend dal catalogo;
5. esecuzione immediata bounded oppure accodamento bounded;
6. completamento tramite evento/future/porta di completamento;
7. rilascio sicuro delle risorse e pubblicazione dello stato finale.

Ogni richiesta asincrona possiede operation_id non ambiguo, generazione, stato, deadline/timeout se applicabile, token di cancellazione, risorse possedute, capability vincolate, backend e versione selezionati. Il completamento deve essere idempotente o protetto contro la doppia finalizzazione.

#### 18.2 Requisiti delle code

- Capacità e memoria massime definite staticamente o nel profilo di inizializzazione.
- Backpressure esplicita quando la coda è piena.
- Nessuna crescita non limitata.
- Priorità con prevenzione della starvation e regole di fairness verificabili.
- Timeout e cancellazione con semantica definita.
- Nessuna attesa bloccante su I/O o su eventi esterni nel percorso critico.
- Separazione tra code per interrupt, kernel worker e userland.
- Nessun callback non controllato o esecuzione di codice arbitrario in contesti di interrupt.

#### 18.3 Eccezioni legittime

Sono ammesse sezioni sincrone quando necessarie per atomiche, pubblicazione di stato, primitive hardware o critical section di durata limitata. Devono avere un limite superiore verificabile, non includere I/O bloccante né allocazioni vietate e documentare il contesto in cui possono essere invocate. Il termine "asincrono" non sostituisce la dimostrazione di correttezza concorrente.

### 19. CICLI CPU, TIMING E MASSIMALI

**Decisione PERF-001:** cicli e timing devono essere verificati e riportati come massimali soltanto quando esiste una base dimostrabile; non devono essere ipotizzati né presentati come garanzie sulla base di misure casuali.

#### 19.1 Classi di evidenza temporale

Ogni dato temporale deve essere marcato con una delle seguenti classi:
- **WCET_PROVEN:** worst-case execution time dimostrato sotto assunzioni esplicite e modello hardware definito.
- **BOUND_ANALYTIC:** limite analitico documentato, non necessariamente prova WCET completa.
- **BOUND_OPERATIONAL:** soglia operativa imposta dal sistema con comportamento definito al superamento.
- **MAX_OBSERVED:** massimo osservato durante un insieme di test; non è una garanzia WCET.
- **AVERAGE/PERCENTILE:** statistica descrittiva, mai etichettata come massimo.

Ogni risultato deve indicare CPU/stepping, ISA, frequenza o politica DVFS, numero di core, configurazione cache, memoria, firmware, compilatore/flags, commit/build ID, temperatura se rilevante, carico, metodo di misura e numero di campioni.

#### 19.2 Metriche obbligatorie per i percorsi critici

Registrare ove applicabile: cicli CPU; tempo wall-clock; latenza di ammissione; latenza di completamento; tempo di attesa; iterazioni massime; contesa; atomiche; cache miss rilevanti; memoria massima; allocazioni (che devono rispettare Rule 3); retry; preemption; interrupt masking; tempo di propagazione e reclamation.

Il contatore dei cicli è specifico della piattaforma e può essere influenzato da frequenza, migrazione, virtualizzazione e sincronizzazione tra core. Le misure AMD64 e ARM64 non devono essere confrontate come se fossero unità equivalenti senza normalizzazione e contesto.

#### 19.3 Regole di pubblicazione

Ogni massimale deve avere condizioni di validità, metodo di prova, copertura, assunzioni e comportamento in caso di superamento. Se non è dimostrabile, il valore viene pubblicato come MAX_OBSERVED o percentile, mai come WCET. Se non è possibile stabilire un limite, il campo deve dichiarare UNKNOWN/UNBOUNDED e generare un problema di progetto, non un numero inventato.

### 20. SINCRONIZZAZIONE, SEMAFORI E PRIMITIVE SMP

**Decisione SYNC-001:** confrontare e utilizzare semafori e altre primitive secondo la classe di risorsa e il contesto; nessuna primitiva è universale.

Le famiglie da valutare sono: semafori binari e counting; mutex; spinlock; atomiche; sequence counter/seqlock; RCU; epoch-based reclamation; hazard pointers; reference counting; message passing/event notification.

Per ogni primitiva il catalogo deve descrivere: contesto ammesso (thread/interrupt/NMI/early boot); possibilità di blocco; boundedness dell'attesa; fairness; rischio di starvation; inversione di priorità; deadlock; comportamento SMP; costo misurato; regole di nesting; gestione della cancellazione; interazione con revoca e reclamation.

I semafori coordinano disponibilità e risveglio, ma non dimostrano da soli un limite temporale né proteggono automaticamente la durata di vita degli oggetti. Uno spinlock non può proteggere operazioni lunghe o bloccanti. Le primitive lock-free richiedono dimostrazione delle proprietà e non sono considerate prive di costi o starvation per il solo nome.

### 21. COERENZA E VERSIONAMENTO DEI CATALOGHI — COMP-006

**Stato: STUDY_REQUIRED.** La scelta E è una decisione di non scegliere prematuramente un singolo protocollo. La politica finale dovrà derivare da uno studio comparativo e da test, non da una preferenza nominale.

I modelli da confrontare:
- aggiornamento immediato con barriera/coordinamento;
- propagazione asincrona;
- coesistenza di versioni parallele;
- modello gerarchico adattivo che combina i precedenti per classe di aggiornamento.

Tecniche candidate, da valutare per workload e lifetime:
- pubblicazione atomica di snapshot immutabili;
- RCU;
- epoch-based reclamation;
- hazard pointers;
- reference counting;
- MVCC;
- seqlock/sequence counter per metadati con vincoli appropriati.

Nessuna tecnica è selezionata come soluzione universale. Seqlock non protegge da solo lifetime e puntatori; RCU/epoch possono trattenere memoria se un lettore non completa; hazard pointers hanno costi di registrazione/scansione; reference counting può introdurre contesa; MVCC non risolve la revoca delle capability.

### 22. CICLO DI VITA DELLE OPERAZIONI E VERSIONI OBSOLETE

Le seguenti condizioni sono distinte e non intercambiabili:
- **OBSOLETE:** non è la versione corrente; può ancora essere valida per un'operazione già ammessa.
- **INCOMPATIBLE:** non soddisfa più un requisito hardware, ABI, dati, dipendenza o contesto.
- **REVOKED:** è vietata da una decisione di sicurezza o policy autoritativa.
- **UNVERIFIED:** non ha superato o non dispone delle verifiche necessarie; non può essere ammessa per nuovi usi che richiedono tale garanzia.

La semplice obsolescenza non implica interruzione. Una versione revocata non resta valida solo perché un'operazione era già iniziata.

#### 22.1 Classificazione delle operazioni (asse O, indipendente da K)

- **O0:** lettura di dati immutabili.
- **O1:** lettura di metadati versionati.
- **O2:** calcolo riavviabile.
- **O3:** operazione transazionale.
- **O4:** operazione che usa risorse hardware.
- **O5:** operazione privilegiata.
- **O6:** operazione critica con interruzione limitata o non immediatamente possibile.

Una funzione può appartenere a più classi. Esempio: K1+K4+O3 indica backend specifico ISA, fallback software e semantica transazionale.

#### 22.2 Politica D per versioni obsolete

La decisione è differenziata in base a categoria di aggiornamento, stato dell'operazione, compatibilità, dipendenze, reversibilità, risorse e rischio. Le azioni ammesse sono:
- completamento con snapshot precedente, soltanto se la versione è ancora valida e compatibile;
- migrazione verso backend/versione verificati;
- pausa e ripresa da checkpoint verificato;
- cancellazione cooperativa e cleanup;
- rollback transazionale;
- interruzione forzata quando richiesta dalla sicurezza e tecnicamente possibile.

La scelta non è basata soltanto sulla compatibilità funzionale: include sicurezza, capability, integrità, lifetime delle risorse e vincoli della CPU di destinazione.

#### 22.3 Stati minimi di un'operazione

NEW → VALIDATING → ADMITTED → QUEUED/RUNNING → COMPLETING → COMPLETED.

Transizioni laterali:
- UPDATE_DETECTED;
- COMPATIBILITY_CHECK;
- MIGRATION_PENDING;
- PAUSE_REQUESTED;
- CANCEL_REQUESTED;
- REVOKED;
- ROLLBACK_PENDING;
- FAILED;
- CANCELLED.

Ogni transizione deve essere esplicita, atomica rispetto allo stato condiviso e protetta contro doppie finalizzazioni. Una macchina a stati concreta dovrà specificare transizioni consentite, eventi, timeout, ownership delle risorse e azioni di recovery. Gli stati terminali non possono essere riaperti accidentalmente.

#### 22.4 Invarianti di lifetime e concorrenza

1. Nessun oggetto è liberato finché può essere referenziato da un utilizzatore valido.
2. Nessuna operazione esegue azioni privilegiate con capability revocate.
3. Migrazione soltanto verso backend compatibile con CPU, ABI, dati e risorse di destinazione.
4. Ogni operazione termina in uno stato definito o viene segnalata come bloccata con motivazione e procedura di recovery.
5. Le transazioni non pubblicano stato parziale: commit/rollback hanno punti di linearizzazione definiti.
6. Retry, attese e code hanno limiti; nessun loop di retry illimitato.
7. Generation counter e lifetime management impediscono riuso ABA e use-after-free.
8. La cancellazione non equivale a liberazione immediata: cleanup e quiescenza devono essere completati.
9. Le vecchie versioni vengono reclamate soltanto dopo prova che non esistano riferimenti validi.
10. Nessun percorso alternativo bypassa il Capability Engine.

### 23. REVOCA ADATTIVA DI SICUREZZA

**Decisione REV-001: D — sistema adattivo di sicurezza.**

La risposta combina blocco delle nuove operazioni, revoca progressiva, migrazione e interruzione secondo gravità, esposizione, possibilità di contenimento e recuperabilità. La continuità operativa non prevale mai sulla sicurezza.

#### 23.1 Fasi obbligatorie

1. **DECLARE:** decisione di revoca firmata/validata dall'autorità di policy, con target, motivazione, versione, ambito e gravità.
2. **FENCE:** impedire nuove ammissioni e nuovi riferimenti alla versione/capability revocata. Il controllo effettivo non dipende dal solo aggiornamento eventuale dei cataloghi locali.
3. **ASSESS:** identificare operazioni in corso, risorse, esposizione, privilegi e possibilità di migrazione.
4. **CONTAIN:** isolare il componente o ridurre i privilegi secondo la policy; nessun nuovo accesso vietato.
5. **MIGRATE / CANCEL / STOP:** migrare verso alternativa verificata, cancellare in modo cooperativo o interrompere quando il rischio lo impone.
6. **RECONCILE:** verificare che non restino operazioni o riferimenti che possano utilizzare il target revocato.
7. **RECLAIM:** recuperare codice, snapshot e risorse solo dopo quiescenza e prova del lifetime.
8. **AUDIT:** registrare decisione, tempi, operazioni coinvolte, esito e prove.

#### 23.2 Classi di severità (da calibrare nello studio)

- **S0 — informativa:** nessun rischio di sicurezza noto; normale deprecazione, nuove operazioni possono essere instradate alla nuova versione.
- **S1 — bassa:** rischio limitato e contenibile; blocco delle nuove ammissioni al target e migrazione controllata.
- **S2 — alta:** rischio significativo; fence immediato, revoca dei privilegi interessati, migrazione o cancellazione prioritaria.
- **S3 — critica/emergenziale:** rischio attivo o compromissione; stop/isolation immediato ove tecnicamente possibile, anche sacrificando continuità e lavoro non recuperabile.

La severità non è un punteggio autonomo sufficiente: l'autorità di sicurezza deve considerare exploitability, esposizione, privilegi, asset coinvolti, possibilità di containment e impatto. Le soglie e i tempi massimi devono essere specificati in una policy separata e testati.

#### 23.3 Regola inderogabile per capability revocate

Dal punto di linearizzazione della revoca, nessuna nuova azione che richiede la capability revocata può essere autorizzata. Le operazioni già ammesse devono rivalidare i diritti nei punti di accesso alle risorse e nei confini di commit. È consentita soltanto l'attività di cleanup strettamente necessaria e separatamente autorizzata; non è una continuazione dei privilegi revocati.

Una revoca di codice/versione e una revoca di capability sono eventi distinti. La revoca del codice può consentire migrazione o completamento di operazioni non esposte al difetto solo dopo analisi e policy; la revoca di capability elimina il diritto di compiere le azioni protette da quel diritto. Nessun grace period può trasformare un diritto revocato in un diritto ancora valido.

#### 23.4 Operazioni non interrompibili

"Non interrompibile" non significa esente dalla sicurezza. Per ogni O6 deve essere definito in anticipo:
- perché l'interruzione immediata è tecnicamente impossibile o più pericolosa;
- quali effetti sono già irreversibili;
- il punto più vicino di arresto sicuro;
- il limite massimo dimostrato per raggiungerlo;
- come isolare ulteriori effetti e privilegi;
- procedura di recovery e audit.

Se non è possibile garantire un limite, l'operazione deve essere segnalata come non bounded e non può essere dichiarata sicura per un contesto che richiede un limite temporale.

### 24. PIANO DI VERIFICA COMP-006 / REV-001

I test devono coprire AMD64 e ARM64, configurazioni single-core e SMP, CPU con feature eterogenee, memoria limitata e workload concorrenti.

Casi minimi:
- aggiornamento durante lettura O0/O1;
- aggiornamento durante calcolo O2 lungo;
- commit/rollback O3 durante cambio versione;
- device operation O4 in corso;
- revoca capability O5 durante l'operazione;
- worker sospeso durante RCU/epoch grace period;
- thread migrato tra CPU con feature differenti;
- esaurimento pool e riserve durante update;
- aggiornamenti consecutivi e versioni multiple;
- rollback mentre esistono lettori;
- worker terminato in ogni stato non terminale;
- revoca S3 mentre il target è in uso;
- revoca mentre la CPU destinataria è offline o non risponde;
- cancellazione simultanea a completamento;
- ABA, use-after-free, double-free e double-completion fault injection.

Metriche: tempo di fence; tempo di propagazione; tempo di migrazione/stop; durata di quiescenza; numero di operazioni coinvolte; memoria trattenuta; massima profondità delle code; retry; latenza; cicli CPU; tempo di cleanup e reclamation.

Per ogni metrica dichiarare MAX_OBSERVED, BOUND_ANALYTIC, BOUND_OPERATIONAL o WCET_PROVEN. Non convertire il massimo osservato in garanzia. Testare timeout e fallimenti di worker senza lasciare accessi non autorizzati.

### 25. DECISION LOG — DELTA 2026-11-05

| ID | Decisione | Esito | Stato |
|---|---|---|---|
| REQ-AERM-001 | Continuità funzionale adattiva | Accolta | DESIGNED |
| AERM-APPROVAL-001 | Approvazione mista D | Accolta | DESIGNED |
| RES-001 | Risorse gerarchiche A+B+C | Accolta | DESIGNED; Rule 3 da dimostrare |
| COMP-001 | Catalogo gerarchico distribuito D | Accolta | DESIGNED |
| CORE-ASYNC-001 | Core il più possibile asincrono | Accolta | DESIGN REQUIREMENT |
| PERF-001 | Cicli/timing verificati, massimali non ipotizzati | Accolta | VERIFICATION REQUIREMENT |
| SYNC-001 | Semafori e primitive classificate e misurate | Accolta | STUDY_REQUIRED |
| COMP-006 | Coerenza cataloghi: studio comparativo E | Accolta | OPEN / STUDY_REQUIRED |
| OP-VERSION-001 | Politica differenziata D per versioni obsolete | Accolta | DESIGNED |
| REV-001 | Revoca adattiva di sicurezza D | Accolta | DESIGNED |

### 26. QUESTIONI APERTE E GATE PRIMA DELL'IMPLEMENTAZIONE

Non considerare chiuse le seguenti questioni:
1. Semantica normativa di NASA Rule 3 per R2 e per l'attivazione di pool.
2. Protocollo definitivo di lifetime/reclamation per snapshot e cataloghi.
3. Algoritmo di coerenza scelto per ciascuna classe di aggiornamento.
4. Punto di linearizzazione e propagazione delle revoche tra CPU.
5. Gestione di CPU offline, bloccate o non responsive durante una revoca.
6. Modello di deadline e WCET per le operazioni critiche.
7. Semantica delle primitive asincrone, cancellazione e backpressure.
8. Soglie S0–S3, autorità di policy e audit della revoca.
9. Formato definitivo del record di compatibilità e dell'alternativa AERM.
10. Strategia di verifica formale e fault injection.

Gate: nessun codice di produzione per questi sottosistemi prima che invarianti, stati, contratti API, failure mode e test siano specificati. Sono ammessi prototipi isolati e benchmark esplorativi, marcati EXPERIMENTAL e non usati come garanzia architetturale.

### 27. STATO DEL DOCUMENTO E TRACCIABILITÀ

La parte fondativa 1.0.0 resta conservata. Questa estensione registra decisioni e requisiti emersi nella sessione AGENT-00 del 2026-11-05. Il file è un documento di progetto, non prova di implementazione, compilazione, test o verifica formale.

Ogni modifica futura deve aggiornare versione, data, changelog e decision log. Le decisioni devono essere append-only: una modifica successiva crea una nuova decisione che supersede la precedente, mantenendo la motivazione e la cronologia.

**Fine Parte II — Architecture Baseline 1.1.0.**


---

# PARTE III — ARCHITETTURA DI SISTEMA, BOOT BUNDLE E USERLAND
## Versione 1.2.0-SYSTEM-ARCHITECTURE-DESIGN
**Data:** 2026-11-05  
**Stato:** DESIGN; non equivale a implementazione o verifica.  
**Autorità:** proposta AGENT-00 accolta come direzione architetturale; dettagli normativi demandati alle specifiche subordinate.

### 28. Sistema composto da progetti indipendenti

nxec è suddiviso in preboot, kernel, moduli ELF, userland, bootloader upstream e tools. Ogni sottoprogetto mantiene build, ABI, manifest, test, versioning e dipendenze propri. La root orchestra e verifica gli artefatti senza fondere i componenti.

Le directory sono create progressivamente quando contengono un artefatto reale. Il progetto preboot/common contiene oggi il codice HCD condiviso; kernel/core contiene il primo gate di ammissione HCD. I progetti modules e userland saranno creati con i primi moduli e servizi reali.

### 29. Preboot HAL ISA e trasferimento di ownership

Il preboot dispone di HAL ISA specifica, distinta dalla HAL kernel, con contratti comuni. AMD64 BIOS/Multiboot2, AMD64 UEFI e ARM64 UEFI sono ingressi separati. Ogni adapter normalizza lo stato iniziale e produce HCD indipendente dall'ISA.

Il preboot inizializza la mappa fisica, riserve, allocator di bootstrap, mapping virtuali transitori, page tables iniziali e strutture di handoff. Riserva memoria per preboot, HCD, kernel image, decompression workspace, stack, page tables, moduli e userland image. Nessuna regione è libera senza classificazione e ownership.

Il salto al kernel è un trasferimento di ownership attraverso un Boot Handoff Descriptor verificabile. Il kernel rivalida HCD, BHD, segment map e CPU state, assume le page tables o le sostituisce, quindi conferma l'adozione prima che il preboot possa reclamare le proprie pagine.

### 30. Kernel image precompilata

Il kernel viene distribuito come bundle versionato con manifest, component table, kernel ELF, digest e firma. NXB è il nome provvisorio. Il preboot verifica autenticità secondo trust policy, limiti, decompressione, ELF, segmenti, relocation whitelist e W^X prima del salto all'entry ELF.

La userland è una seconda immagine indipendente, non incorporata implicitamente nel kernel bundle. CRC/checksum rilevano corruzione, non autenticità. Algoritmi firma/hash, anti-rollback, compressione e profilo ET_EXEC/ET_DYN restano soggetti a studio e test comparativi.

### 31. Kernel hypervisor leggero e microkernel

Il kernel unisce HAL, controllo hypervisor leggero, microkernel e Core. Le feature VMX/SVM/EL2 sono backend hardware condizionali e non si presumono disponibili. Il modello di isolamento di base usa privilege levels, MMU e capability; la virtualizzazione hardware viene dichiarata solo se realmente attiva.

Il Core mantiene registry e capability, scheduler, memoria, buffer, IPC, loader e supervisione. Driver, VFS, filesystem, network e servizi ordinari restano moduli ELF userland salvo funzioni minime strettamente necessarie al boot e alla sicurezza.

### 32. REG e risorse uniformi

REG è il protocollo comune asincrono per richieste, eventi, notifiche, capability transfer e buffer condivisi. È ispirato al modello request/reply e cancellation di Plan 9, ma definisce autorità e memory model propri. Il payload REG non è codice eseguibile; l'esecuzione nativa passa dal loader ELF autorizzato.

Ogni risorsa hardware/software ha object type, schema version, object ID, generation, owner, lifecycle, compatibility class, security label e capability. Il registry canonico è logicamente centralizzato, mentre shard e snapshot per CPU evitano contesa globale. Le code sono bounded, con crediti, backpressure, cancellation e completion esplicite.

Zero-copy è ammesso solo con ownership, lifetime, alignment, cache coherency, capability e DMA/IOMMU verificati. Il fallback copia dati solo tramite pool bounded autorizzato. Il registry non concede diritti: il Capability Engine resta autorità.

### 33. Domini e multiutenza

MACHINE è il dominio privilegiato hardware/kernel e non è accessibile da userland. SYSTEM è un principal di servizi userland privilegiati e syscall API, non ring zero. ROOT è sandbox amministrativa; USER sandbox per identità; SHARED sandbox per oggetti con grant esplicito; DRIVER è classe userland con capability device.

I domini sono astratti e mappati per ISA: AMD64 CPL/VMX e ARM64 EL/security state non sono sinonimi. Le syscall non bypassano capability. Ogni sandbox ha address space, namespace, capability table, quote e audit.

### 34. Userland image e VFS

La VFS completa è servizio userland. Il kernel mantiene mount authority, namespace e capability; filesystem driver e path lookup sono moduli isolati. NXUI è nome provvisorio per l'immagine userland read-only, distinta dal kernel. cpio newc, tar/pax, SquashFS e NXFS devono essere confrontati prima della scelta.

Ogni utente ha mount dati privato identificato dal registry, esposto ad esempio come data/sdN. Scrittura limitata alla capability del mount. ROOT, SYSTEM, USER e SHARED hanno namespace distinti; symlink, mount crossing, linking dinamico e compatibility layer sono capability-scoped.

### 35. Assembly modules e verifica

Ogni modulo assembly ISA-specifico ha manifest, ABI, target features, register clobber, stack bound, relocation whitelist, build e test indipendenti. La pipeline valida ELF, disassembly, istruzioni rispetto al manifest, W^X, ABI, QEMU feature matrix e hardware. Le feature K0-K6 determinano compatibilità e fallback senza cambiare i diritti.

### 36. GRUB upstream e licenze

Il source GNU GRUB 2.14 è mantenuto come progetto Git indipendente in boot/grub/upstream, con commit e licenza registrati. La licenza upstream è GPL-3.0-or-later, non inclusa nella lista permissiva originaria. GRUB non viene incorporato o linkato nel kernel; distribuzione e immagini combinate richiedono audit GPL e corresponding source. La licenza di nxec resta da decidere.

### 37. Reclaim del preboot e recovery

Default: il codice preboot viene reclamato dopo che il kernel ha verificato il BHD, assunto mapping e ownership e non restano riferimenti. Si conserva un audit record immutabile. Un demone di prevenzione errori opera in SYSTEM userland con capability ristrette. Un eventuale monitor MACHINE è modulo kernel distinto; conservare il codice preboot non concede privilegi impliciti.

### 38. Evidenza e stato implementativo al 2026-11-05

IMPLEMENTED/TESTED: HCD wire, CRC-64, parser, semantic checks, section catalog, builder, fixture Core, test host e cross compile AMD64/ARM64; kernel HCD admission gate con test host e cross compile.

SOURCE ACQUIRED: GNU GRUB 2.14 tag grub-2.14, commit d38d6a1a9b79427848976f53d474392cd29c2a71; build target non ancora verificata.

DESIGN ONLY: NXB bundle, decompression, ELF kernel loader, BHD, preboot ISA bootstrap, REG runtime, capability engine, hypervisor runtime, VFS/NXUI, userland, compatibility layers e QEMU boot.

Nessun timing/WCET è dichiarato per le nuove componenti. Tutte le metriche restano non misurate finché non vengono raccolte con metodo e classe di evidenza.

**Fine Parte III — System Architecture Design 1.2.0.**

### 39. Memory bootstrap planner — prima implementazione

Il preboot dispone ora di un extent planner caller-owned che consuma esclusivamente un HCD già validato e seleziona regioni MEMORY_USABLE. Le richieste dichiarano resource kind, owner, size, alignment, min/max physical address, boundary, attributes, NUMA preference e fallback esplicito. Le regioni contigue vengono coalesced soltanto se attributes, owner e NUMA coincidono. La policy iniziale è first-fit; best-fit, top-down e NUMA-aware restano alternative da confrontare.

Limiti v1 provvisori: 4096 memory records HCD, 64 allocation requests, 4160 extent slots e workspace massimo 144 KiB. Il planner non tronca: oltre il limite il producer può coalescere solo semanticamente equivalenti, altrimenti recovery/rifiuto. Il limite va qualificato con firmware reali; non è garantito dagli standard UEFI/ACPI/E820/DTB.

Il planner non inizializza ancora page tables, MMU o contenuto pagine. Produce soltanto allocazioni fisiche verificabili. Test host/cross AMD64/ARM64 PASS; nessuna misura WCET o timing è dichiarata.

### 40. Firmware ownership, initial task e cancellation asincrona

Il profilo HCD Core in modalità UEFI richiede ExitBootServices completato e vieta Boot Services attivi; la modalità BIOS vieta i flag EFI. Il protocollo di boot (UEFI application o Multiboot2) è distinto dall'ambiente firmware: Multiboot2 può essere avviato da UEFI e deve riportare la modalità reale. La memoria HCD_MEMORY_UNACCEPTED è distinta e mai allocabile. Il planner opera solo dopo trasferimento ownership e alloca solo HCD_MEMORY_USABLE.

La sequenza UEFI deve preallocare tutti i buffer tramite AllocatePages/AllocatePool, ottenere GetMemoryMap e MapKey, chiamare ExitBootServices e ripetere la query/uscita in caso di MapKey stale. Dopo il primo fallimento sono consentite soltanto Memory Allocation Services. Il budget retry è ancora da qualificare; nessun valore universale è presunto.

Il riferimento seL4 è il modello initial/root task + BootInfo + capability untyped, non una VFS nel kernel. La userland root image e il servizio VFS restano separati dal kernel. REG adotta request IDs con generation e cancellation ack/flush ispirato a 9P Tflush, distinguendo CANCELLED da TOO_LATE dopo commit; il payload REG non è codice eseguibile e non conferisce authority.

Test HCD UEFI active/done, memoria unaccepted, memory planner e sanitizer host PASS; cross AMD64/ARM64 PASS. UEFI ExitBootServices adapter/retry, page tables, REG runtime e root task sono ancora da implementare.

Aggiornamento implementativo: `preboot/hal/uefi_memory_map.c` converte in-place mappe descriptor v1 dopo ExitBootServices, con bounds, overflow check, sorting bounded, coalescing semantico e rifiuto esplicito delle mappe/attributi non rappresentabili. `preboot/hal/multiboot2_memory_map.c` valida e normalizza il tag BIOS/E820 Multiboot2 senza inferire attributi assenti; `preboot/hal/multiboot2_efi_map.c` estrae e valida il tag EFI post-EBS dal Multiboot2 information block. Tutti passano host/cross/sanitizer/static analysis; non sono ancora collegati a entry firmware. L'ownership EBS per AMD64 UEFI/Multiboot2 e ARM64 UEFI è risolta per design tramite ispezione del sorgente GRUB 2.14 pinned, ma resta da validare a runtime; vedi `docs/PROBLEM_CATALOG.md`.
