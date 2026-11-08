# HCD — Hardware Capability Descriptor
## Specifica tecnica, formato binario e contratto di bootstrap

**ID:** SPEC-HCD-001  
**Versione:** 1.0.0-DRAFT  
**Data:** 2026-11-05  
**Stato:** DESIGN — non ancora implementato né verificato su hardware  
**Autorità:** Documento fondativo nxec, sezioni 2, 4, 6, 10, 11, 12 e Parte II  
**Ambito:** AMD64 e ARM64; estendibile ad altre ISA senza modificare il formato comune.

---

## 1. Scopo e requisiti normativi

Le parole MUST, MUST NOT, SHOULD e MAY sono normative.

- **HCD-001:** formato indipendente dall'ISA, dalla dimensione dei puntatori e dall'ABI C del produttore.
- **HCD-002:** tutte le dimensioni, gli offset e gli indirizzi fisici sono interi a larghezza fissa; nessun puntatore nativo viene serializzato.
- **HCD-003:** le feature CPU sono distinte in SUPPORTED, ENABLED e USABLE, sia per CPU sia a livello di piattaforma.
- **HCD-004:** su CPU eterogenee sono conservati i dati per ogni CPU; l'intersezione globale non sostituisce i dati individuali.
- **HCD-005:** il kernel verifica magic, versione, dimensioni, allineamento, checksum, directory, sezioni, riferimenti esterni e requisiti prima di usare i contenuti.
- **HCD-006:** sezioni e record hanno limiti massimi espliciti; ogni ciclo del parser è bounded.
- **HCD-007:** una sezione sconosciuta obbligatoria causa rifiuto. Una sezione sconosciuta opzionale può essere saltata solo dopo averne verificato i limiti.
- **HCD-008:** il descrittore non concede capability e non costituisce prova di autenticità del firmware o del produttore.
- **HCD-009:** il pre-kernel non alloca memoria dinamicamente dopo l'inizializzazione. Buffer HCD e tabelle temporanee provengono da aree riservate/preallocate.
- **HCD-010:** ogni dato non rilevato o non verificabile è UNKNOWN/UNAVAILABLE; non si inventano valori, feature o limiti.

Il formato è un oggetto binario immutabile dopo la finalizzazione. Il produttore costruisce il buffer in memoria preallocata, valida la propria costruzione, calcola il checksum e lo passa al Core. Il Core tratta l'HCD come input non attendibile anche quando proviene da un bootloader considerato affidabile.

## 2. Separazione tra trasporto, formato e autorità

Sono tre livelli distinti:

1. **Trasporto di boot:** protocollo con cui GRUB/UEFI consegna il controllo e le informazioni iniziali.
2. **HCD:** formato comune normalizzato, indipendente dal trasporto.
3. **Autorità:** il Core decide quali risorse e feature siano effettivamente utilizzabili e quali capability concedere.

Un tag Multiboot2, una tabella ACPI, un DTB o una struttura UEFI non sono l'HCD. Sono fonti da validare e tradurre in sezioni HCD.

Gli indirizzi fisici esterni sono riferimenti descrittivi, non puntatori dereferenziabili direttamente. Il Core verifica che ogni intervallo sia rappresentabile, non overflowi, sia mappabile e non contraddica la mappa memoria validata.

## 3. Scelta del trasporto GRUB: vincolo verificato

La specifica Multiboot2 2.0 definisce gli identificatori architetturali i386 (0) e MIPS32 (4); non definisce un identificatore AArch64. Non è lecito dichiarare un kernel AArch64 come Multiboot2 inventando un valore o presumendo compatibilità.

Conseguenza progettuale, senza cambiare la decisione GRUB come riferimento principale:

- **AMD64 BIOS/legacy:** GRUB Multiboot2 può consegnare il controllo in protected mode a 32 bit; il percorso pre-kernel richiesto esegue la transizione controllata al long mode.
- **AMD64 UEFI x64:** il percorso Multiboot2 EFI amd64 consegna il controllo già in long mode secondo lo stato definito dalla specifica; il codice di ingresso riconosce questo stato e non ripete la transizione.
- **ARM64 UEFI:** GRUB arm64-efi avvia il pre-kernel come applicazione EFI PE/COFF tramite il percorso EFI/chainloader. Il pre-kernel parte in AArch64 e usa i servizi UEFI prima di ExitBootServices.
- **ARM64 non-UEFI:** resta un percorso distinto da specificare per firmware/piattaforma; non viene simulato con Multiboot2.

Il pre-kernel mantiene un ingresso comune logico, ma gli stub ISA/firmware-specifici traducono il contesto iniziale in un contratto interno verificato. L'eventuale scelta di un secondo bootloader richiede una decisione registrata, non è implicita.

**Ownership firmware definita; integrazione runtime pendente:** AMD64 BIOS/Multiboot2 consuma il tag memory map Multiboot2 e non usa ExitBootServices. AMD64 UEFI/Multiboot2 usa per default il percorso GRUB che termina ExitBootServices e consegna il tag EFI memory map; il kernel nxec MUST NOT richiedere il tag header EFI Boot Services che mantiene attivi i servizi. ARM64 UEFI è chainloaded da GRUB come applicazione EFI PE/COFF: il preboot nxec riceve Boot Services attivi, acquisisce tutti i buffer necessari e possiede la chiamata ExitBootServices. Il codice di normalizzazione non può essere collegato finché i relativi adapter non rispettano questi contratti. La scelta è verificata nel sorgente GRUB 2.14 pinned; resta da validare con boot firmware/QEMU. Vedere DEC-UEFI-ENTRY-OWNERSHIP-001 e PROBLEM-UEFI-ENTRY-OWNERSHIP-001.

L'iteratore bounded `preboot_multiboot2_info_iterator` valida dimensione e tag dell'information block completo prima di esporli. Per AMD64 BIOS, `preboot_multiboot2_get_memory_map_tag` richiede un solo tag type 6 e `preboot_multiboot2_normalize_information_memory_map` compone estrazione e conversione in una singola chiamata: valida version 0, stride >=24, reserved zero, input ordinato/non-overlapping e tipi noti; produce record HCD caller-owned e coalescesce solo adjacent entries equivalenti. Gli span sorgente, output e output-count devono essere disgiunti; nel getter anche i due output devono essere disgiunti tra loro. L'aliasing o il wrap degli span host-side è rifiutato prima di modificare qualsiasi buffer, incluso il buffer d'ingresso. Attributi e NUMA/owner non trasportati dal tag vengono catalogati. Per AMD64 UEFI, `preboot_multiboot2_get_efi_memory_map` riusa l'iteratore, richiede il tag EFI memory map post-EBS, rifiuta EFI_BS/duplicati e restituisce i descriptor al normalizzatore UEFI esistente senza copiarli o alterarne la provenienza. Questi parser di trasporto non costituiscono firmware entry adapter né prova runtime.

Fonti primarie consultate:
- GNU GRUB Multiboot2 Specification 2.0: https://www.gnu.org/software/grub/manual/multiboot2/multiboot.html
- GNU GRUB Manual 2.14: https://www.gnu.org/software/grub/manual/grub/grub.html
- UEFI Specification 2.11: https://uefi.org/specifications
- UEFI 2.11 PDF: https://uefi.org/sites/default/files/resources/UEFI_Spec_Final_2.11.pdf
- Devicetree Specification: https://devicetree.org/specifications/
- Intel SDM: https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html
- Arm64 CPU feature register documentation (cross-check, non normativa ISA): https://docs.kernel.org/arch/arm64/cpu-feature-registers.html
- ACPI Specification 6.6: https://uefi.org/specifications

## 4. Regole di codifica binaria

### 4.1 Endianness e tipi

- Tutti i campi scalari HCD v1 sono little-endian.
- Tipi wire: U8, U16, U32, U64; non esistono size_t, long, uintptr_t, bool o puntatori nel wire format.
- I booleani sono U8 con valori ammessi 0 o 1; ogni altro valore è invalido.
- Gli enum sono U32 o U16 con valori riservati esplicitamente; un valore sconosciuto in un campo obbligatorio causa rifiuto.
- I campi reserved e il padding devono essere zero.
- Ogni offset è relativo all'inizio dell'HCD, salvo i campi denominati physical_address.
- Tutti gli offset e le dimensioni sono byte. Nessun calcolo offset+size è eseguito prima di una verifica overflow-safe.

Il codice C usa uint*_t da stdint.h e controlli _Static_assert. Non usare __attribute__((packed)) per rendere artificialmente valido il layout: il layout deve essere naturale, esplicito e verificato con sizeof, _Alignof e offsetof. La lettura da input non allineato avviene tramite accessori endian-safe o copie controllate, non tramite cast arbitrari.

### 4.2 Limiti v1

I limiti seguenti sono limiti del parser, non affermazioni sui limiti dell'hardware:

| Costante | Valore v1 | Comportamento al superamento |
|---|---:|---|
| HCD_HEADER_SIZE | 64 byte | rifiuto |
| HCD_SECTION_ENTRY_SIZE | 40 byte | rifiuto |
| HCD_MAX_SECTIONS | 64 | rifiuto |
| HCD_MAX_MEMORY_REGIONS | 4096 (limite v1 provvisorio) | rifiuto esplicito; coalescing solo se attributi/owner/NUMA/lifetime coincidono |
| HCD_MAX_CPUS | 65.536 | rifiuto esplicito; mai troncamento silenzioso |
| HCD_MAX_TOTAL_SIZE | 64 MiB | rifiuto esplicito |
| HCD_FEATURE_BITS | 512 per CPU | feature fuori catalogo segnalata; nessuna falsa completezza |

I valori sono configurabili solo tramite nuova versione della specifica e test dei limiti. Una build con limiti inferiori deve dichiarare il profilo e non può presentarsi come pienamente compatibile con v1. Il pre-kernel non degrada silenziosamente un descrittore oltre i limiti: produce un errore diagnostico bounded e ferma il boot oppure consegna un HCD esplicitamente incompleto, accettabile solo per un profilo di recupero definito.

## 5. Header v1

Dimensione esatta: 64 byte; allineamento: 8 byte.

| Offset | Tipo | Campo | Regola |
|---:|---|---|---|
| 0 | U32 | magic | 0x31444348 (byte ASCII HCD1) |
| 4 | U16 | version_major | 1 |
| 6 | U16 | version_minor | 0 |
| 8 | U32 | header_size | 64 |
| 12 | U32 | architecture | 1=AMD64, 2=ARM64 |
| 16 | U64 | total_size | 64..HCD_MAX_TOTAL_SIZE |
| 24 | U64 | checksum | CRC-64/ECMA-182 dell'intero HCD |
| 32 | U32 | flags | bitmask definita sotto |
| 36 | U32 | section_count | 1..HCD_MAX_SECTIONS |
| 40 | U64 | section_table_offset | offset allineato a 8 |
| 48 | U32 | producer_id | identificatore stabile del pre-kernel |
| 52 | U32 | producer_version | versione codificata dal produttore |
| 56 | U64 | reserved0 | deve essere zero |

Header flags v1:
- bit 0 HCD_F_INCOMPLETE: almeno una categoria richiesta non è stata rilevata integralmente.
- bit 1 HCD_F_HETEROGENEOUS_CPUS: i dati CPU non sono uniformi.
- bit 2 HCD_F_EXTERNAL_REFERENCES: almeno una sezione contiene riferimenti fisici esterni.
- bit 3 HCD_F_FIRMWARE_BOOT_SERVICES_ACTIVE: i boot services UEFI sono ancora attivi al passaggio.
- bit 4 HCD_F_EFI_EXIT_BOOT_SERVICES_DONE: ExitBootServices è riuscita.
- bit 5 HCD_F_SECURE_BOOT_REPORTED: lo stato Secure Boot è stato letto e riportato; non equivale ad autenticità HCD.
- bit 6..31: reserved, zero in v1.

Le combinazioni dei bit 3 e 4 devono essere coerenti col percorso di boot. Nel profilo CORE, modalità UEFI richiede bit 4 impostato e bit 3 azzerato, indipendentemente dal trasporto (UEFI application o Multiboot2 avviato da UEFI); un HCD Core con Boot Services ancora attivi è rifiutato. Modalità BIOS non può dichiarare gli stati Boot Services UEFI. Il profilo RECOVERY può descrivere uno stato preboot incompleto/attivo ma non è handoff kernel. Il Core non assume che UEFI sia ancora invocabile solo perché il firmware è UEFI.

## 6. Checksum e integrità

### 6.1 Algoritmo v1

CRC-64/ECMA-182:
- polynomial: 0x42F0E1EBA9EA3693;
- init: 0x0000000000000000;
- refin: false;
- refout: false;
- xorout: 0x0000000000000000.

Per calcolare il checksum, il produttore tratta i byte dell'intervallo [24, 32) (campo checksum) come zero e calcola sul range completo [0, total_size). Il risultato è scritto in little-endian nel campo checksum. Tutti i byte di padding compresi nel range devono essere inizializzati a zero prima del calcolo.

CRC rileva corruzione accidentale; non autentica il produttore e non resiste a modifiche intenzionali. Secure Boot, firma del loader, misure TPM e catena di fiducia sono livelli distinti da specificare nella security architecture. Il Core non trasforma un CRC valido in una prova di provenienza attendibile.

La scelta CRC-64 rispetto a BLAKE3 per il checksum strutturale v1 privilegia un algoritmo piccolo, deterministico e implementabile nel pre-kernel senza dipendenze crittografiche complesse. BLAKE3 e firme restano candidati separati per integrità crittografica/autenticità, da confrontare in una decisione di sicurezza; non sostituiscono il controllo dei limiti.

## 7. Directory delle sezioni

Ogni entry è lunga 40 byte, allineata a 8:

| Offset relativo | Tipo | Campo |
|---:|---|---|
| 0 | U32 | type |
| 4 | U16 | version |
| 6 | U16 | flags |
| 8 | U64 | offset |
| 16 | U64 | size |
| 24 | U32 | entry_size |
| 28 | U32 | entry_count |
| 32 | U32 | alignment |
| 36 | U32 | reserved0 |

Section flags:
- bit 0 HCD_SF_REQUIRED: il Core deve conoscere e validare la sezione.
- bit 1 HCD_SF_ARRAY: payload composto da record fissi.
- bit 2 HCD_SF_EXTERNAL: payload contiene indirizzi fisici esterni.
- bit 3 HCD_SF_PER_CPU: record indicizzati da logical_cpu_id.
- bit 4..15 reserved.

Tipi sezione v1:
1. CPU_SUMMARY — riepilogo globale CPU/ISA.
2. CPU_RECORDS — una entry per CPU logica.
3. CPU_FEATURE_CATALOG — definizioni/ID delle feature usate.
4. TOPOLOGY — package/die/cluster/core/thread e NUMA.
5. CACHE — gerarchia cache e condivisione.
6. MEMORY_MAP — intervalli fisici e attributi.
7. INTERRUPT_CONTROLLERS — controller e domini interrupt.
8. VECTOR_LAYOUT — vettori eccezioni/interrupt e riserve.
9. MMU_CAPABILITIES — record per CPU, granularità page table, VA/PA, livelli e attributi.
10. VIRTUALIZATION — record per CPU, capacità rilevate e stato abilitato.
11. FIRMWARE_REFERENCES — riferimenti a ACPI, DTB, SMBIOS e tabelle firmware.
12. BOOT_CONTEXT — trasporto, entry state, loader, memoria/ownership di bootstrap.
13. TIMER_CAPABILITIES — timer disponibili, frequenza e modalità di lettura.
14. PLATFORM_DEVICES — dispositivi platform essenziali descritti senza driver attivi.

Una sezione singleton non può comparire più di una volta. Una sezione array può comparire una sola volta per tipo nella v1; eventuali partizioni multiple richiedono versione successiva. Tipi non elencati sono sconosciuti: se REQUIRED il Core rifiuta; se opzionali il Core verifica bounds, alignment e checksum globale e li ignora.

## 8. Regole di layout e validazione strutturale

1. total_size deve essere almeno 64 e non superiore al limite del profilo.
2. section_table_offset deve essere multiplo di 8 e non inferiore a header_size.
3. Il prodotto section_count * 40 deve essere calcolato con moltiplicazione overflow-safe.
4. L'intera directory deve ricadere dentro total_size.
5. Ogni sezione deve avere offset allineato al valore dichiarato (potenza di due, minimo 8 per v1), size coerente con entry_size * entry_count per ARRAY e range completamente contenuto nel buffer.
6. Nessuna sezione può sovrapporsi all'header, alla directory o a un'altra sezione.
7. Nessun intervallo può terminare oltre total_size; verificare prima size <= total_size - offset.
8. I record hanno dimensione esatta per la versione dichiarata; dimensioni sconosciute non vengono interpretate.
9. reserved0, padding e gap canonici sono zero.
10. Gli ID di sezione sono univoci; gli ID dei record sono univoci nel proprio dominio.
11. Le sezioni richieste dal profilo di boot devono essere presenti; non basta un checksum valido.
12. Il parser impone un limite al numero di record e alle operazioni per record. Nessun ciclo usa un conteggio non validato.
13. La validazione strutturale precede la validazione semantica; la validazione semantica precede ogni attivazione hardware.
14. La verifica CRC precede l'uso di qualunque payload; i bounds di header/directory necessari per localizzare il checksum vengono verificati prima.
15. Qualunque errore rende il descrittore non utilizzabile. Il Core registra un codice errore stabile e passa al percorso di boot recovery/reject definito; non tenta parsing parziale con privilegi normali.

## 9. Semantica CPU e feature

### 9.1 Identità CPU

Ogni CPU record contiene logical_cpu_id univoco; package_id, die_id, cluster_id, core_id, thread_id; numa_node_id o HCD_ID_UNKNOWN; vendor bytes (16 byte, NUL padding); campi normalizzati family/model/stepping quando applicabili; identificatore raw ISA (CPUID signature/MIDR_EL1 e relative estensioni); flag di presenza, BSP/boot CPU, online al rilevamento e stato affidabilità.

Gli ID topologici non sono presumibilmente contigui. HCD_ID_UNKNOWN è 0xFFFFFFFF. Il Core non deduce parentela da un ordinamento arbitrario dei record.

### 9.2 Tre insiemi distinti

Per ogni CPU e per la piattaforma:
- **SUPPORTED:** il processore dichiara la feature.
- **ENABLED:** tutti i gate architetturali richiesti risultano abilitati nello stato osservato.
- **USABLE:** il pre-kernel ha verificato anche i prerequisiti software/OS necessari per un uso sicuro nel contesto descritto.

Invariante v1: USABLE ⊆ ENABLED ⊆ SUPPORTED. Una violazione rende il record semanticamente invalido.

La presenza CPUID/ID register non equivale automaticamente a USABLE. Esempio AMD64: AVX richiede controllo coerente di XSAVE/OSXSAVE e XCR0; feature dipendenti da stato esteso devono includere tutti i prerequisiti. Esempio ARM64: i campi ID_AA64* devono essere interpretati secondo l'Arm ARM e non come bit booleani generici; valori implementer-defined/reserved non diventano feature portabili.

Il catalogo usa ID stabili nxec (0..511), con nome canonico, ISA, prerequisiti, fonte registro/bit, dipendenze e classe K. Non si confrontano direttamente i bit CPUID con i bit ID_AA64: il pre-kernel normalizza entrambi in ID canonici e conserva le fonti raw necessarie all'audit.

### 9.3 Intersezione globale e scheduling

Il riepilogo globale espone intersezione delle feature USABLE comuni a tutte le CPU ammesse allo scheduling generale; unione delle feature SUPPORTED rilevate; flag di eterogeneità e gruppi CPU equivalenti; eventuali CPU escluse dal profilo generale e motivazione.

Una feature presente solo su un sottoinsieme CPU non può essere usata da un task migrabile liberamente. Potrà essere usata solo da un backend K2 con affinità/compatibilità verificata e capability adeguata. Il Core rivalida il target CPU dopo ogni migrazione e non si affida al solo snapshot iniziale.

## 10. Sezioni e record: contenuto minimo

I record wire dettagliati e le relative sizeof/offsetof devono essere mantenuti nel header condiviso preboot/common/hcd_wire.h; questo documento è la fonte normativa. Il codice non può introdurre una seconda definizione divergente.

### 10.1 CPU_SUMMARY

Contiene conteggio CPU, logical CPU del bootstrap, ISA primaria, modalità di avvio, bitmap globali e flag di eterogeneità. I bitmap sono derivati dai record CPU: SUPPORTED è l'unione delle feature dichiarate; ENABLED è l'intersezione sulle CPU online; USABLE è l'intersezione sulle CPU ammesse allo scheduling generale. Non possono contraddirsi né sostituire i dati per-CPU.

### 10.2 CPU_RECORDS

Array con un record per CPU logica. Il record v1 ha logical/package/die/cluster/core/thread/NUMA IDs; flags e ISA; vendor[16], family/model/stepping e raw ISA identity; 8 parole U64 per SUPPORTED, 8 per ENABLED e 8 per USABLE (512 feature IDs); riferimento/indice a dati raw supplementari se presenti.

Le feature oltre 511 non possono essere compresse, aliasate o ignorate: il produttore deve segnalarle come non rappresentate e impostare INCOMPLETE, oppure una versione successiva deve estendere il catalogo.

### 10.3 TOPOLOGY

Record: node_id univoco, parent_node_id, logical_cpu_id associato oppure HCD_ID_UNKNOWN, livello (package/die/cluster/core/thread/NUMA), flags e source ID. Il parent deve esistere oppure essere UNKNOWN. La relazione parent-child deve rispettare livelli strettamente crescenti verso i nodi CPU; cicli e parent multipli incompatibili sono errori.

### 10.4 CACHE

Record: CPU logical ID o gruppo, livello, tipo (data/instruction/unified), dimensione byte, line size, ways, sets, sharing domain e attributi. I valori derivati devono conservare la fonte e non sostituire valori hardware incoerenti senza segnalazione.

### 10.5 MEMORY_MAP

Record: base fisica U64, length U64, type U32, attributes U32, NUMA ID U32, owner U32. Tipi distinti includono usable RAM, reserved, firmware/runtime, ACPI reclaimable, ACPI NVS, MMIO, persistent memory, unusable, bootloader/pre-kernel e unaccepted memory. L'elenco deve essere normalizzato, ordinato e non sovrapposto. Le regioni contigue possono essere unite solo se tipo, attributi, owner e NUMA coincidono.

Nessun indirizzo fisico è automaticamente RAM libera. Il Core esclude kernel, HCD, stack, moduli, page tables, firmware e buffer temporanei ancora in uso.

### 10.6 INTERRUPT_CONTROLLERS e VECTOR_LAYOUT

Descrivono controller (es. xAPIC/x2APIC, GICv2/v3/v4), interrupt domain, intervalli ID, routing, trigger/polarity quando noti e indirizzi MMIO. I vettori CPU/exception sono un namespace diverso dagli interrupt ID del controller: non devono essere confusi. I record riportano fonte, stato e limiti; il Core costruisce le proprie tabelle solo dopo la validazione.

### 10.7 MMU_CAPABILITIES

Un record per CPU logica, con logical_cpu_id, architettura, larghezza indirizzo fisico supportata e configurata; larghezza VA e modalità canonical address AMD64; livelli page-table disponibili e attivi; granularità page/block supportate; attributi memoria e modalità execute-never/no-execute; feature di invalidazione/TLB; stato translation regime al passaggio.

Le granularità AMD64 e ARM64 vengono normalizzate in byte, ma i dettagli ISA restano disponibili in campi estesi specifici. Non si presume che una dimensione pagina supportata sia già attiva.

### 10.8 VIRTUALIZATION

Un record per CPU logica, identificato da logical_cpu_id. Registra capacità hardware (AMD SVM/Intel VMX, ARM EL2 e feature correlate), stato firmware/hypervisor, livelli privilegiati disponibili, controlli e limitazioni osservate. SUPPORTED non significa che la virtualizzazione sia attivabile: policy firmware, hypervisor esterno, lock e capability possono impedirlo.

### 10.9 FIRMWARE_REFERENCES

Record: kind, physical_address, length, revision, flags, checksum/identificatore quando definito dalla fonte, memory type/owner e lifetime. Include RSDP/XSDT/RSDT, tabelle ACPI, DTB, SMBIOS e tabelle vendor ammesse.

Il Core verifica allineamento e range fisico; contenimento in intervalli mappabili e non contraddittori; checksum e lunghezza secondo la specifica della singola tabella; signature/revision; lifetime e proprietà della memoria.

Una tabella firmware non viene copiata o liberata automaticamente. La strategia zero-copy mantiene il riferimento solo se il firmware non la modifica e la memoria resta riservata; altrimenti il Core la copia in un pool preallocato e registra l'operazione come copia necessaria, non come zero-copy.

### 10.10 BOOT_CONTEXT e TIMER_CAPABILITIES

BOOT_CONTEXT identifica protocollo, modalità CPU all'ingresso, stato UEFI Boot Services, origine memory map, entry address e intervalli occupati dal loader/pre-kernel. Non contiene function pointer.

TIMER_CAPABILITIES distingue timer per-core, platform timer e clocksource; riporta frequenza, width, proprietà di monotonicità, sincronizzazione, accessibilità e incertezza. Un valore di frequenza non è un WCET né garantisce sincronizzazione tra CPU.

## 11. Acquisizione per architettura

### 11.1 AMD64

Fonti: CPUID (interrogare prima i massimi leaf/subleaf); registri di controllo, XCR0 e MSR solo quando CPU, livello di privilegio e contesto consentono la lettura; ACPI e UEFI memory map o informazioni Multiboot2 senza mescolare le semantiche delle fonti.

Per ogni accesso MSR/registro potenzialmente non supportato o riservato serve un controllo di prerequisito e una gestione di fault controllata. Il pre-kernel non assume che un leaf CPUID sia identico tra vendor. Le feature vendor-specific mantengono namespace e provenienza.

La transizione 32-bit → long mode richiede verifica dei prerequisiti CPU, PAE, EFER.LME, CR0.PG, CR4.PAE, GDT/IDT e paging iniziale. L'ordine effettivo e il codice assembly devono essere verificati rispetto a Intel SDM e AMD APM. L'ingresso UEFI x64/Multiboot2 EFI già long mode segue un percorso distinto.

### 11.2 ARM64

Fonti: MIDR_EL1 e ID_AA64* consentiti dal livello di eccezione e dalla piattaforma; Device Tree (DTB) e/o ACPI; UEFI System Table e Configuration Tables se avvio EFI; registri di interrupt controller, MMU e timer soltanto se accessibili nel livello corrente.

I campi dei registri ID sono valori codificati, spesso con livelli di feature e valori riservati; ogni campo deve essere interpretato da una tabella derivata dall'Arm Architecture Reference Manual della revisione supportata. Non usare != 0 come test universale di presenza.

AArch32 non viene forzato. L'ingresso è AArch64. Il livello di eccezione corrente, stato MMU/cache, interrupt mask e stato firmware sono registrati nel BOOT_CONTEXT.

### 11.3 ACPI e DTB

ACPI e DTB sono fonti alternative o complementari, non copie equivalenti. Il parser conserva la provenienza e segnala conflitti tra descrizioni. Non sceglie arbitrariamente un valore. La policy di precedenza deve essere definita per ogni classe di dato (CPU, NUMA, interrupt, device, memory map) e testata su firmware reali.

Il parser ACPI valida RSDP, revision, checksum, lunghezze, entry count e checksum delle tabelle; nessuna tabella è accettata solo perché puntata da un'altra tabella. Il parser DTB valida magic, totalsize, version/last compatible version, offset, dimensioni dei blocchi, terminatori, string offsets, reservation map e bounds di ogni token. I parser non eseguono AML o codice contenuto nel firmware.

## 12. Passaggio di ownership e lifetime

Il produttore garantisce che il buffer HCD sia fisicamente contiguo per l'intervallo dichiarato, allineato a 8 byte e non modificato dopo il checksum. Il contratto di handoff identifica base fisica e dimensione HCD; CPU e stato iniziale; intervalli memoria riservati; stato Boot Services; tabelle firmware esterne ancora valide; stack e page tables attivi e relativi owner.

Il Core valida HCD prima di assumere ownership. La memoria HCD non può essere reclamata finché il Core non ha completato validazione e registrato tutti i riferimenti necessari. La conferma di accettazione è un evento interno del Core, non un flag che il produttore può auto-attribuirsi.

## 13. Codici di errore stabili

HCD_OK; HCD_E_NULL_INPUT; HCD_E_BAD_MAGIC; HCD_E_UNSUPPORTED_MAJOR; HCD_E_UNSUPPORTED_MINOR_REQUIRED; HCD_E_BAD_HEADER_SIZE; HCD_E_BAD_TOTAL_SIZE; HCD_E_BAD_CHECKSUM; HCD_E_BAD_SECTION_DIRECTORY; HCD_E_SECTION_DUPLICATE; HCD_E_SECTION_UNKNOWN_REQUIRED; HCD_E_SECTION_BOUNDS; HCD_E_SECTION_OVERLAP; HCD_E_ALIGNMENT; HCD_E_RESERVED_NONZERO; HCD_E_COUNT_LIMIT; HCD_E_INTEGER_OVERFLOW; HCD_E_SEMANTIC_INCONSISTENCY; HCD_E_CPU_FEATURE_SET_INVALID; HCD_E_MEMORY_MAP_INVALID; HCD_E_EXTERNAL_REFERENCE_INVALID; HCD_E_FIRMWARE_TABLE_INVALID; HCD_E_INCOMPLETE_NOT_ALLOWED; HCD_E_PROFILE_MISMATCH.

Il primo errore è conservato con section type, record index e offset, senza esporre dati non validati come stringhe o puntatori. La diagnostica è bounded e non usa allocazione dinamica.

## 14. Compatibilità e versionamento

- Major differente: rifiuto, salvo esplicito parser di compatibilità isolato e testato.
- Minor maggiore: il parser v1 può accettarlo soltanto se header/section semantics restano compatibili e tutte le nuove sezioni obbligatorie sono assenti o note; altrimenti rifiuto.
- Sezione con versione maggiore sconosciuta e REQUIRED: rifiuto.
- Sezione opzionale sconosciuta: skip dopo verifica strutturale.
- Nuovo campo non può essere aggiunto in coda a un record v1 mantenendo la stessa versione del record.
- Le estensioni ISA-specifiche usano sezioni/record versionati; non alterano i campi comuni.

Ogni cambiamento è append-only nel decision log. La versione del documento, quella del formato HCD e quella del produttore sono indipendenti.

## 15. Test obbligatori prima di dichiarare HCD VERIFIED

### 15.1 Test statici e compile-time
- sizeof, _Alignof, offsetof per ogni wire struct su AMD64 e ARM64.
- Compilazione C17 con -Wall -Wextra -Werror -Wpedantic.
- Verifica endian helper, overflow helper e CRC test vectors pubblici.
- Verifica che nessun wire struct contenga pointer, long, size_t o bitfield implementation-defined.

### 15.2 Test parser
- magic/version/header size/total size errati;
- checksum errato e checksum valido;
- offset non allineati, fuori range e con overflow;
- directory troncata, count enorme, size*count overflow;
- sezioni duplicate, sovrapposte, vuote o mancanti;
- reserved non-zero, padding non-zero;
- tipo/major sconosciuto obbligatorio e opzionale;
- record CPU duplicati, feature subset violati;
- mappe memoria sovrapposte, overflow e range fuori mappa;
- riferimenti firmware non allineati, non mappabili o con checksum errato;
- input mutato durante validazione;
- ogni fault injection deve produrre errore deterministico senza OOB, UAF, panic incontrollato o accesso privilegiato.

### 15.3 Test piattaforma
- QEMU AMD64 BIOS e UEFI;
- QEMU ARM64 UEFI;
- CPU con feature diverse, SMP e single-core;
- ACPI e DTB, dove applicabili;
- memoria limitata e firmware table malformed;
- avvio con e senza virtualizzazione esposta.

QEMU dimostra il comportamento sui dispositivi emulati, non la compatibilità universale hardware. Servono test su hardware reale per dichiarazioni di compatibilità.

### 15.4 ADB e performance
Ogni test registra build ID, ISA, modello CPU, firmware, configurazione QEMU/hardware, compiler/flags, campioni, latenza, cicli quando affidabili, memoria massima e contesto. I risultati sono classificati WCET_PROVEN, BOUND_ANALYTIC, BOUND_OPERATIONAL, MAX_OBSERVED o AVERAGE/PERCENTILE come stabilito nella Parte II del documento fondativo. Nessun massimo osservato è presentato come WCET.

## 16. Gate di implementazione

Prima del codice di produzione devono essere completati:
1. header wire condiviso con layout e static assertions;
2. tabella canonica iniziale degli ID feature AMD64/ARM64 con fonti normative;
3. schema preciso dei record di ogni sezione e profilo minimo obbligatorio;
4. parser di validazione indipendente dal bootloader;
5. generatore HCD con builder bounded e CRC;
6. test unitari parser/CRC e test di corruzione;
7. integrazione separata AMD64 BIOS, AMD64 UEFI e ARM64 UEFI;
8. prova di ownership/lifetime per HCD e firmware tables.

Non dichiarare la specifica VERIFIED prima dei gate. I prototipi possono essere marcati EXPERIMENTAL.

## 17. Registro problemi iniziali

### ID: PROBLEM-HCD-001
**Titolo:** Protocollo Multiboot2 non definito per AArch64  
**Stato:** RESOLVED_BY_DESIGN  
**Impatto:** COMPATIBILITY  
**Descrizione:** Multiboot2 2.0 enumera i386 e MIPS32, non AArch64.  
**Analisi alternative:** inventare un architecture ID (vietato); usare un protocollo custom (richiede bootloader dedicato); avviare EFI PE/COFF ARM64 tramite GRUB arm64-efi.  
**Decision:** mantenere GRUB; AMD64 usa Multiboot2 dove supportato; ARM64 usa il percorso EFI chainloader e pre-kernel AArch64.  
**Test aggiunti:** gate di boot separati AMD64 BIOS/UEFI e ARM64 UEFI, da implementare.  
**Metriche prima/dopo:** N/A.  
**Riferimenti codice:** non ancora implementato.

### ID: PROBLEM-HCD-002
**Titolo:** Confusione tra feature dichiarata e feature realmente utilizzabile  
**Stato:** DESIGNED  
**Impatto:** SAFETY / COMPATIBILITY  
**Descrizione:** CPUID/ID registers non dimostrano da soli che il sistema operativo abbia configurato tutti i prerequisiti.  
**Analisi alternative:** un solo bitmap; tre bitmap per stato; catalogo di eventi di abilitazione.  
**Decision:** tre insiemi SUPPORTED/ENABLED/USABLE per CPU e riepilogo globale, con invariante di inclusione e provenienza.  
**Test aggiunti:** subset invariants, CPU eterogenee, prerequisiti OS state.  
**Metriche prima/dopo:** N/A.  
**Riferimenti codice:** non ancora implementato.

## 18. Riferimenti normativi e tecnici

Le fonti devono essere ricontrollate per la revisione effettivamente supportata prima di implementare ogni campo:

- UEFI Forum, UEFI Specification 2.11, inclusi GetMemoryMap, ExitBootServices, Configuration Tables e memory types: https://uefi.org/specifications
- UEFI 2.11 PDF: https://uefi.org/sites/default/files/resources/UEFI_Spec_Final_2.11.pdf
- UEFI Forum, ACPI 6.6 (May 2025): https://uefi.org/specifications
- GNU GRUB Manual 2.14 (8 Jan 2026): https://www.gnu.org/software/grub/manual/grub/grub.html
- GNU GRUB Multiboot2 Specification 2.0: https://www.gnu.org/software/grub/manual/multiboot2/multiboot.html
- Devicetree Specification: https://devicetree.org/specifications/
- Intel 64 and IA-32 Architectures SDM: https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html
- Arm Architecture Reference Manual for A-profile architecture: https://developer.arm.com/documentation/ddi0487/latest/
- Linux arm64 CPU feature register documentation (implementation cross-check, not normative ISA): https://docs.kernel.org/arch/arm64/cpu-feature-registers.html

---

**Fine SPEC-HCD-001.** La presente specifica è DESIGN; la verifica richiede implementazione, test riproducibili e prove sui due percorsi ISA.


## 19. ABI C condivisa e dimensioni normative

Il file preboot/common/hcd_wire.h è il binding C17 del wire format v1. È parte della specifica, non un'implementazione alternativa. I layout sono verificati tramite _Static_assert e devono compilare identici per i target x86_64-elf e aarch64-none-elf.

Dimensioni normative:
| Tipo C | Dimensione | Allineamento |
|---|---:|---:|
| hcd_header | 64 | 8 |
| hcd_section_entry | 40 | 8 |
| hcd_cpu_summary | 224 | 8 |
| hcd_cpu_record | 288 | 8 |
| hcd_cpu_feature_record | 32 | 4 |
| hcd_cpu_raw_register_record | 32 | 8 |
| hcd_topology_record | 32 | 8 |
| hcd_cache_record | 48 | 8 |
| hcd_memory_record | 40 | 8 |
| hcd_interrupt_controller_record | 64 | 8 |
| hcd_vector_record | 32 | 8 |
| hcd_mmu_capabilities | 64 | 8 |
| hcd_virtualization_record | 32 | 4 |
| hcd_firmware_reference | 48 | 8 |
| hcd_boot_context | 64 | 8 |
| hcd_timer_record | 48 | 8 |
| hcd_platform_device_record | 64 | 8 |

La sezione CPU_RAW_REGISTERS (type 15) è aggiunta al catalogo v1 come sezione opzionale di audit. Conserva le letture raw (CPU, spazio registro, selector, subselector, valore, stato accesso); non è fonte autonoma di autorizzazione e non sostituisce la normalizzazione CPU_FEATURE_CATALOG.

Le sezioni con record fissi devono dichiarare entry_size uguale alla dimensione normativa della tabella. Le sezioni singleton non-array dichiarano entry_count=1 ed entry_size uguale alla dimensione del record. Per sezioni con payload misto, la struttura interna deve essere definita da una versione specifica prima dell'implementazione; non si può fingere che sia un array omogeneo.

L'allineamento riportato in tabella è quello del target C ABI verificato. Il wire format resta little-endian e non dipende dal padding accidentale: se un compilatore/target non soddisfa gli assert, la build fallisce. Il parser continua a leggere byte da input non attendibile e non assume che l'indirizzo sorgente sia allineato.

## 20. Evidenza di compilazione iniziale

Il binding è stato compilato con C17, freestanding, -Wall -Wextra -Werror -Wpedantic e -fsyntax-only su:
- x86_64-elf
- aarch64-none-elf

Entrambi i target hanno superato gli static assertions del layout. Questa evidenza verifica soltanto dimensioni e compilabilità del binding; non dimostra ancora correttezza semantica del generatore, parser, checksum, boot o hardware.


## 21. Implementazione CRC e profilo freestanding

L'implementazione condivisa è in preboot/common/hcd_crc64.c. Usa una tabella costante di 256 valori U64, generata in modo riproducibile da tools/gen-hcd-crc-table.py e verificata nel repository. La tabella occupa 2048 byte statici; non viene inizializzata a runtime e non richiede allocazione.

La variante bitwise (otto iterazioni per byte) resta riferimento di confronto; la tabella statica riduce il lavoro per byte al costo di 2 KiB di memoria read-only. Le istruzioni hardware non sono assunte perché non esiste un percorso CRC-64 uniforme garantito per entrambe le ISA. La selezione è provvisoria fino ai benchmark comparativi e alle misure su AMD64 e ARM64.

Le funzioni rifiutano puntatori nulli, dimensioni oltre HCD_MAX_TOTAL_SIZE e buffer HCD inferiori all'header. Il ciclo di scansione ha bound statico HCD_MAX_TOTAL_SIZE e termina prima quando raggiunge la dimensione validata. HCD_ASSERT usa __builtin_trap per invarianti interne in build freestanding; è un'estensione Clang/GCC esplicita e non dipende da assert.h o libc.

Test vector verificato: ASCII 123456789 produce 0x6C40DF5F0B497347. La funzione HCD tratta i byte del campo checksum [24,32) come zero senza modificare il buffer sorgente. Il test fixture da 64 byte produce 0xC29F5C4AC075B014.


## 22. Profilo di validazione e semantica completa dei record

### 22.1 Singleton e array

Singleton v1 (ARRAY non impostato, entry_count=1):
- CPU_SUMMARY: hcd_cpu_summary, 224 byte.
- BOOT_CONTEXT: hcd_boot_context, 64 byte.

Array v1 (ARRAY impostato, entry_size uguale al record wire):
- CPU_RECORDS: hcd_cpu_record, 288 byte, PER_CPU.
- CPU_FEATURE_CATALOG: hcd_cpu_feature_record, 32 byte.
- CPU_RAW_REGISTERS: hcd_cpu_raw_register_record, 32 byte, PER_CPU.
- TOPOLOGY: hcd_topology_record, 32 byte, PER_CPU.
- CACHE: hcd_cache_record, 48 byte, PER_CPU.
- MEMORY_MAP: hcd_memory_record, 40 byte.
- INTERRUPT_CONTROLLERS: hcd_interrupt_controller_record, 64 byte.
- VECTOR_LAYOUT: hcd_vector_record, 32 byte, PER_CPU.
- MMU_CAPABILITIES: hcd_mmu_capabilities, 64 byte, PER_CPU.
- VIRTUALIZATION: hcd_virtualization_record, 32 byte, PER_CPU.
- FIRMWARE_REFERENCES: hcd_firmware_reference, 48 byte, EXTERNAL.
- TIMER_CAPABILITIES: hcd_timer_record, 48 byte.
- PLATFORM_DEVICES: hcd_platform_device_record, 64 byte, EXTERNAL quando contiene MMIO.

Per gli array opzionali è ammesso entry_count=0 e size=0. Per gli array richiesti dal profilo attivo è richiesto almeno un record, salvo eccezione esplicita del profilo di recovery. La sezione CPU_RAW_REGISTERS può essere assente o vuota se il produttore non dispone di una lettura raw sicura; non si inventano registri.

### 22.2 Profili

Il parser riceve un profilo richiesto dal chiamante; il profilo non viene deciso dal produttore HCD.

**HCD_PROFILE_CORE** richiede:
- CPU_SUMMARY;
- CPU_RECORDS;
- CPU_FEATURE_CATALOG;
- TOPOLOGY;
- MEMORY_MAP;
- INTERRUPT_CONTROLLERS;
- VECTOR_LAYOUT;
- MMU_CAPABILITIES;
- BOOT_CONTEXT;
- TIMER_CAPABILITIES.

Il Core rifiuta il profilo normale se una sezione richiesta manca, è incompleta o non soddisfa i requisiti semantici. La presenza della sezione non prova che il suo contenuto sia utilizzabile.

**HCD_PROFILE_RECOVERY** può accettare solo un HCD con HCD_F_INCOMPLETE e con tutte le verifiche strutturali, checksum, lifetime e sicurezza comunque superate. Il Core limita le operazioni alle capability e ai percorsi esplicitamente ammessi dalla policy di recovery; non abilita scheduler SMP, driver o feature non verificate. La recovery non trasforma dati invalidi in dati validi e non concede privilegi.

### 22.3 Semantica dei record

- CPU_RECORDS: logical_cpu_id univoco; architecture uguale all'header per CPU del tipo primario; vendor NUL-padded; reserved zero; bitmap feature con 8 parole U64 per stato. Il numero di record deve coincidere con CPU_SUMMARY.cpu_count.
- CPU_FEATURE_CATALOG: feature_id univoco nell'intervallo 0..511; architecture_mask coerente; source_register/selector/subselector/source_field identificano la fonte raw; reserved zero. Ogni bit impostato nei bitmap CPU deve avere una voce nel catalogo.
- CPU_RAW_REGISTERS: logical_cpu_id esistente; register_space/selector/subselector validi per l'ISA; access_state distingue letto, non accessibile, non implementato e fault controllato. Un valore non letto deve essere zero e lo stato deve dichiararlo.
- TOPOLOGY: node_id univoco; parent_node_id esistente o UNKNOWN; logical_cpu_id esistente o UNKNOWN; level appartiene all'enum di topologia; i livelli devono crescere in modo monotono dal parent verso il nodo figlio. Ogni CPU online deve appartenere a un nodo CPU.
- CACHE: logical_cpu_id esistente o UNKNOWN per cache condivisa; size_bytes > 0; line_size power-of-two quando noto; ways e sets coerenti quando entrambi noti; overflow vietato nei prodotti.
- MEMORY_MAP: length > 0; base+length senza overflow; type valido; ordinamento crescente; nessuna sovrapposizione. Nessun intervallo è libero se non è marcato USABLE e non è occupato da un owner.
- INTERRUPT_CONTROLLERS: controller_id univoco; intervalli interrupt non overflowanti; MMIO range coerente con MEMORY_MAP e marcato MMIO; parent controller valido o UNKNOWN.
- VECTOR_LAYOUT: vector_id nel dominio dell'architettura; owner_cpu esistente o UNKNOWN; nessuna assegnazione conflittuale non dichiarata.
- MMU_CAPABILITIES: un record per CPU online; logical_cpu_id esistente; physical/virtual address bits nel range architetturale; livelli attivi sottoinsieme dei livelli supportati; granularità attiva sottoinsieme di quelle supportate.
- VIRTUALIZATION: logical_cpu_id esistente; hypervisor_present booleano 0/1; features_enabled sottoinsieme di features_supported.
- FIRMWARE_REFERENCES: physical_address+length senza overflow; range contenuto in memoria riservata/mappabile; lifetime esplicito. Il parser non dereferenzia riferimenti fisici: il checksum dichiarato viene verificato dal sottosistema consumer dopo una mappatura autorizzata.
- BOOT_CONTEXT: protocollo e stato CPU d'ingresso coerenti con l'architettura; intervalli loader/HCD contenuti nella memoria riservata; flag UEFI coerenti con il completamento di ExitBootServices.
- TIMER_CAPABILITIES: frequenza non zero per un timer dichiarato utilizzabile; counter_width valido; interrupt ID compatibile con controller dichiarato; timer per-CPU identifica CPU esistente.
- PLATFORM_DEVICES: range MMIO non overflowante e coerente con MEMORY_MAP; source_id riconducibile a DTB/ACPI o altra fonte verificata.

### 22.4 Contratto del parser

Il parser non alloca memoria. Il chiamante fornisce:
- buffer HCD immutabile e lunghezza disponibile;
- workspace preallocato per massimo HCD_MAX_SECTIONS viste sezione;
- profilo HCD_PROFILE_CORE o HCD_PROFILE_RECOVERY;
- struttura errore preallocata.

Il parser non dereferenzia indirizzi fisici esterni. Restituisce viste limitate al buffer HCD e riferimenti esterni ancora non mappati. Il chiamante non può usare le viste finché hcd_validate non restituisce HCD_OK.

La validazione segue l'ordine: parametri e header minimo; limiti e checksum; directory; bounds/alignment/overlap; versioni e flag; presenza del profilo; semantica record; coerenza cross-section; pubblicazione della vista. In caso di errore, la vista non viene pubblicata e il dettaglio errore identifica code, section_type, record_index e offset.

Tutte le scansioni sono limitate da HCD_MAX_TOTAL_SIZE, HCD_MAX_SECTIONS, HCD_MAX_CPUS, HCD_MAX_MEMORY_REGIONS e dai conteggi validati. Il parser non usa ricorsione, allocazione dinamica, stringhe non terminate o puntatori a dati non validati.


## 23. Semantica CPU v1 e profilo recovery — chiarimenti normativi

I logical_cpu_id sono assegnati dal pre-kernel in modo denso nell'intervallo [0,cpu_count), indipendentemente dagli APIC ID, MPIDR affinity o altri identificatori hardware. CPU_RECORDS è ordinato per logical_cpu_id e il record all'indice i deve avere logical_cpu_id=i. Questo rende la verifica duplicati bounded senza hash table o allocazione.

CPU flags:
- bit 0 PRESENT;
- bit 1 BOOT;
- bit 2 ONLINE;
- bit 3 SCHEDULABLE;
- altri bit reserved.
PRESENT è obbligatorio. SCHEDULABLE implica ONLINE. Il boot CPU deve essere ONLINE e deve essere l'unico record con BOOT.

CPU_SUMMARY flags:
- bit 0 HETEROGENEOUS, presente se i bitmap SUPPORTED/ENABLED/USABLE differiscono tra almeno due CPU.
- altri bit reserved in v1.
HCD_F_HETEROGENEOUS_CPUS deve corrispondere al flag CPU_SUMMARY.

La CPU_FEATURE_CATALOG è richiesta anche nel profilo recovery, così il Core può verificare che ogni bit feature presente nei record CPU abbia una definizione canonica. Il profilo recovery richiede quindi CPU_SUMMARY, CPU_RECORDS, CPU_FEATURE_CATALOG, MEMORY_MAP e BOOT_CONTEXT. Non richiede interrupt/timer/topology completi e non abilita le normali funzioni Core.

Il parser calcola e verifica i bitmap globali: SUPPORTED = unione delle CPU presenti; ENABLED = intersezione delle CPU online; USABLE = intersezione delle CPU schedulable. Se una di queste popolazioni è vuota, il descrittore è semanticamente invalido.

La validazione semantica implementata in hcd_semantics.c copre le sezioni obbligatorie del profilo Core e tutte le sezioni opzionali quando presenti. Restano gate prima della qualifica complessiva: fuzzing, test su firmware/bootloader reali, test QEMU AMD64/ARM64, fault injection e misure WCET per piattaforma.


## 24. Ordinamento, budget e limiti del parser HCD v1

### 24.1 Record normalizzati e ordinamento

- CPU_RECORDS: logical_cpu_id denso e uguale all'indice.
- CPU_RAW_REGISTERS: ordinati lessicograficamente per logical_cpu_id, register_space, selector, subselector; raw_record_index punta al primo record della CPU o UNKNOWN.
- TOPOLOGY: node_id denso e uguale all'indice; parent_node_id è UNKNOWN o minore del node_id; il livello del parent è strettamente inferiore. I nodi THREAD associano un solo logical_cpu_id.
- CACHE: cache_id denso e uguale all'indice.
- INTERRUPT_CONTROLLERS: controller_id denso e uguale all'indice; intervalli interrupt ordinati e non sovrapposti per interrupt_base.
- VECTOR_LAYOUT: ordinato per owner_cpu e poi vector_id, senza duplicati.
- MMU_CAPABILITIES: un record per CPU, ordinato per logical_cpu_id.
- VIRTUALIZATION: se presente, un record per CPU, ordinato per logical_cpu_id.
- TIMER_CAPABILITIES: timer_id denso e uguale all'indice.
- PLATFORM_DEVICES: device_id denso e uguale all'indice.

### 24.2 Boot context wire layout

BOOT_CONTEXT resta 64 byte:
- offset 0: boot_protocol U32;
- offset 4: boot_mode U32;
- offset 8: entry_cpu_state U32;
- offset 12: flags U32, zero in v1;
- offset 16: loader_base U64;
- offset 24: loader_size U64;
- offset 32: hcd_physical_address U64;
- offset 40: hcd_reserved_base U64;
- offset 48: hcd_reserved_size U64;
- offset 56: firmware_revision U32;
- offset 60: reserved0 U32, zero.

boot_protocol v1: MULTIBOOT2=1, UEFI_APPLICATION=2. boot_mode: BIOS=1, UEFI=2. entry_cpu_state: AMD64_PROTECTED32=1, AMD64_LONG64=2, ARM64_EL1=3, ARM64_EL2=4. ARM64 richiede UEFI_APPLICATION e UEFI. AMD64 BIOS richiede Multiboot2 e protected32; AMD64 UEFI richiede long64.

Loader e intervallo HCD devono ricadere ciascuno in una singola regione MEMORY_MAP ammessa (RESERVED, FIRMWARE_RUNTIME o BOOTSTRAP). hcd_physical_address+total_size deve essere contenuto nell'intervallo hcd_reserved. Il parser non assume che due regioni adiacenti possano essere unite.

### 24.3 Validazione delle sezioni core

- TOPOLOGY: parent già incontrato, livelli crescenti, ogni CPU online rappresentata esattamente una volta.
- MMU_CAPABILITIES: una voce per CPU, architettura coerente, VA/PA bits validi, livelli attivi sottoinsieme dei supportati.
- INTERRUPT_CONTROLLERS: ID denso, intervalli ordinati/non sovrapposti, parent precedente, MMIO contenuto in MEMORY_MAP di tipo MMIO.
- VECTOR_LAYOUT: range architetturale, owner valido, ordinamento stretto.
- TIMER_CAPABILITIES: ID denso, frequenza e counter width validi; interrupt associato a un controller dichiarato; MMIO verificato se presente.
- BOOT_CONTEXT: protocollo/architettura coerenti, riserve e loader verificati contro MEMORY_MAP.
- CACHE, VIRTUALIZATION, CPU_RAW_REGISTERS, FIRMWARE_REFERENCES e PLATFORM_DEVICES sono verificati se presenti, anche in recovery.

### 24.4 Complessità e workspace

Il workspace è caller-owned e preallocato. Comprende 64 viste di sezione e una bitmap CPU da HCD_MAX_CPUS bit; il budget massimo verificato a compile-time è 12 KiB. Nessuna allocazione dinamica, ricorsione o pubblicazione parziale.

I controlli di intervallo MEMORY_MAP usano ricerca binaria con massimo 64 iterazioni; gli interrupt ID usano ricerca binaria con massimo 32 iterazioni. La directory ha al massimo 64 sezioni e il controllo overlap è limitato a 64×64 confronti. Tutti gli altri cicli sono limitati da HCD_MAX_TOTAL_SIZE, HCD_MAX_CPUS o dal conteggio validato della sezione. Questi limiti sono bound algoritmici, non una dichiarazione WCET: il WCET resta da misurare/provare per piattaforma e configurazione.


## 25. Vincoli semantici v1 aggiuntivi

- MEMORY_MAP attributes usa esclusivamente HCD_MEMORY_A_READ, WRITE, EXECUTE, CACHE_UC, CACHE_WC, CACHE_WT, CACHE_WB, ENCRYPTED, PERSISTENT, HOTPLUG, DMA e RUNTIME; i bit restanti sono riservati.
- INTERRUPT_CONTROLLERS è limitata a 256 record. controller_id è denso e ordinato per interrupt_base; gli intervalli sono non sovrapposti. parent_id può riferirsi a qualsiasi controller valido; il parser rileva cicli con Floyd, con massimo 256 passi per catena.
- La ricerca interrupt ID usa binary search sulle basi ordinate, massimo 32 iterazioni.
- CPU_RAW_REGISTERS register_space v1: CPUID=1, MSR=2, ARM_ID=3, ARM_SYSREG=4, PCI_CONFIG=5, FIRMWARE=6. Stati accesso: READ=1, UNIMPLEMENTED=2, UNAVAILABLE=3, FAULT=4. I valori diversi da READ devono essere zero.
- CACHE flags v1 devono essere zero; line_size, ways e sets sono controllati senza overflow.
- VIRTUALIZATION flags v1 devono essere zero; features_enabled è sottoinsieme di features_supported.
- FIRMWARE_REFERENCES flags v1 devono essere zero; kind è 1..5 e il range deve appartenere a una regione riservata, firmware runtime, ACPI reclaimable, ACPI NVS o bootstrap.
- PLATFORM_DEVICES kind v1: PCI=1, ACPI=2, DEVICETREE=3, VIRTIO=4, USB=5, STORAGE=6, NETWORK=7, OTHER=8. flags e campi riservati sono zero in v1; gli intervalli MMIO devono appartenere a MEMORY_MAP di tipo MMIO.
- MMU_CAPABILITIES v1: PA bits 1..52; VA bits AMD64 1..57, ARM64 1..52; page/block granule mask usa i tre bit v1; livelli attivi sono sottoinsieme dei supportati.


## 26. HCD builder e produzione zero-copy

Il builder usa un buffer fisico/virtuale fornito dal pre-kernel e un workspace caller-owned; non alloca memoria. In init riserva 64×40 byte per la directory massima dopo l'header (payload iniziale a offset 2624), azzera header/directory e imposta il cursore. La directory finale contiene solo le sezioni effettive; il gap residuo resta zero.

reserve_section verifica tipo v1, unicità, entry_size canonico, flag ARRAY/PER_CPU/EXTERNAL contro HCD_SECTION_CATALOG, count, alignment, overflow e capacità. La sezione viene azzerata e restituita come puntatore scrivibile: il produttore compila direttamente il payload senza copia intermedia.

finalize scrive header e directory little-endian, deriva HCD_F_EXTERNAL_REFERENCES dalle sezioni marcate EXTERNAL e calcola CRC-64. Il builder non dichiara semanticamente valido il risultato: il pre-kernel deve eseguire hcd_validate con il profilo richiesto e pubblicare il buffer solo dopo esito positivo.


## 27. Limite memory map e planner fisico preboot

HCD v1 limita MEMORY_MAP a HCD_MAX_MEMORY_REGIONS=4096 record. È un limite di formato v1 provvisorio da qualificare con memory map reali UEFI/E820/ACPI/DTB e test sintetici; non è un massimo imposto dagli standard firmware. Il produttore può coalescere soltanto intervalli contigui con type, attributes, owner, NUMA e lifetime equivalenti. Se il numero resta oltre il limite, il bundle/HCD è rifiutato o entra in recovery; non è ammesso troncare o scartare descrittori.

Il planner preboot consuma esclusivamente un HCD già validato e seleziona MEMORY_USABLE. Mantiene un extent list caller-owned, first-fit, ordinato e bounded. Ogni richiesta specifica resource kind, owner, size, alignment, min/max physical address, boundary, attributes, NUMA preference e fallback autorizzato. NUMA fallback non è implicito. Le allocazioni sono contigue; scatter/gather è una capability/operazione distinta e non viene simulata.

Limiti implementati: 64 richieste; fino a 4096 extent iniziali e 64 split aggiuntivi (4160 extent); workspace massimo 144 KiB. La struttura deve risiedere in memoria bootstrap esplicitamente riservata, non su uno stack di dimensione non verificata. Il planner non abilita MMU, non crea page tables e non inizializza ancora il contenuto delle pagine: produce soltanto un piano fisico verificabile.

Il criterio first-fit è iniziale e non congelato come ottimo universale. Prima del boot reale vanno confrontati best-fit, top-down, NUMA-aware e pool preallocati su mappe frammentate e carichi reali. Le metriche sono scan steps, split, frammentazione, workspace, latenza e cicli; MAX_OBSERVED non è WCET.


## 28. UEFI memory ownership e memoria unaccepted

HCD_MEMORY_UNACCEPTED (type 10) rappresenta memoria fisicamente presente ma non ancora accettata secondo il protocollo piattaforma; non è allocabile. HCD_MEMORY_USABLE è utilizzabile dal planner solo quando il protocollo di boot ha trasferito ownership. Il planner non converte automaticamente UNACCEPTED, ACPI_RECLAIMABLE, ACPI_NVS, FIRMWARE_RUNTIME, MMIO o BOOTSTRAP in USABLE.

Per UEFI, il firmware mantiene la proprietà della memory map durante Boot Services; l'immagine UEFI può usare solo memoria esplicitamente allocata tramite AllocatePages/AllocatePool. ExitBootServices riceve il MapKey corrente di GetMemoryMap; dopo un MapKey stale va rifatta la query. Dopo il primo tentativo fallito, il loader può usare soltanto Memory Allocation Services. Solo dopo ExitBootServices riuscita il loader assume ownership della memoria disponibile. EfiBootServicesCode/Data e EfiConventionalMemory diventano candidati USABLE; EfiRuntimeServices, ACPI NVS, ACPI reclaimable prima dell'attivazione ACPI, memoria non accettata e regioni di preboot ancora in uso restano distinte.

Riferimento normativo: UEFI Specification 2.11, §7.2 Memory Allocation Services e §7.4.6 ExitBootServices: https://uefi.org/specs/UEFI/2.11/07_Services_Boot_Services.html


## 29. HCD v2: estensione additiva MMU

HCD v2 è costruito sul contenitore HCD v1: non ridefinisce né rimuove header, directory, sezioni 1..15 o record v1. Mantiene `HCD_MAGIC`, header da 64 byte, endianness, checksum, directory e le versioni/layout dei record esistenti. Il campo `version_major` vale 2 e `version_minor` vale 0. Tutte le sezioni v1 presenti in v2 mantengono lo stesso `type`, `version`, `entry_size`, offset dei campi e semantica v1. `HCD_VERSION_MAJOR` resta 1 per l'API builder esistente; `HCD_VERSION_MAJOR_V2` e `hcd_builder_init_v2` selezionano esplicitamente la nuova versione.

Il validator v1 conserva il comportamento per HCD major 1. Un consumer che implementa solo major 1 rifiuta major 2 secondo §14; non si tenta downgrade o parsing parziale. Un consumer aggiornato può validare major 1 con le regole v1 oppure major 2 con le stesse regole strutturali/v1 più le sezioni additive qui definite. In major 1, i tipi sezione 16 e 17 restano sconosciuti: un tipo sconosciuto REQUIRED è rifiutato, uno opzionale è saltato solo dopo i controlli strutturali. Le sezioni 16 e 17 sono riconosciute soltanto in major 2.

### 29.1 Sezioni MMU additive

Major 2 aggiunge due array a record fissi, entrambi `version=1`, `ARRAY | PER_CPU | REQUIRED`, non `EXTERNAL`:

| Tipo | Nome | Entry size | Massimo |
|---:|---|---:|---:|
| 16 | MMU_TRANSLATION_CONFIGS | 64 byte | 16 configurazioni per CPU |
| 17 | MMU_MAPPING_SIZES | 32 byte | 32 record per configurazione |
| 18 | MMU_ARM64_CONTEXTS | 24 byte | 16 contesti per CPU |

`MMU_CAPABILITIES` v1 resta presente e conserva byte/layout/semantica originari. Il consumer v2 lo valida con le regole v1 e usa le nuove sezioni per i dettagli MMU non rappresentabili senza ambiguità nel record v1. Per il cross-reference v2, `architecture` e `logical_cpu_id` devono coincidere tra record base e configurazioni; i campi legacy PA/VA/granularity/livelli non sono reinterpretati come campi nuovi e non sostituiscono le configurazioni v2.

Le configurazioni sono ordinate per `(logical_cpu_id, configuration_id)`. Per ogni CPU gli ID iniziano da zero, sono densi e non superano 15. Ogni CPU rappresentata nel catalogo CPU deve avere almeno una configurazione supportata e precisamente una configurazione marcata CURRENT. `TRANSLATION_ENABLED` è ammesso solo sulla configurazione CURRENT. Una configurazione rappresenta una combinazione valida di regime, granulo, larghezze e livelli, non un prodotto cartesiano implicito di mask indipendenti.

La sezione 18 è richiesta in HCD v2 ARM64 e vietata in HCD AMD64. Contiene un record per ogni configurazione ARM64 stage 1 e non sostituisce le regole comuni sopra: il bit `TRANSLATION_ENABLED` della configurazione resta ammesso solo su CURRENT. Serve a descrivere separatamente l'attività contemporanea dei regimi TTBR0 e TTBR1, che il flag comune non può esprimere senza violare tale vincolo. I record sono ordinati per `(logical_cpu_id, configuration_id)`; ogni configurazione stage 1 deve avere esattamente un record corrispondente e ogni record deve riferirsi a una configurazione stage 1 esistente. Per EL1 devono essere presenti un contesto TTBR0/lower e un TTBR1/upper; per EL2 è ammesso il solo TTBR0/lower. Contesti duplicati, mancanti, non ordinati o con exception level/range incoerenti sono rifiutati.

Nel producer ARM64, `CURRENT` identifica il regime stage 1 che contiene il PC acquisito dall'entry adapter; se il PC non ricade esattamente in un range valido TTBR0/TTBR1, il producer rifiuta lo stato. Il record di contesto indica se il relativo regime è attivo per le traduzioni: SCTLR.M deve essere impostato e il corrispondente TCR.EPD0/EPD1 deve essere zero. Per il contesto CURRENT questo stato deve coincidere con il flag comune `TRANSLATION_ENABLED`; per un contesto non-CURRENT l'attività è riportata esclusivamente nell'estensione ARM64 e non autorizza quel flag comune. L'assenza dell'estensione o un cross-reference incoerente è un errore, non un fallback.

### 29.2 `hcd_mmu_translation_config` — 64 byte

Tutti i campi sono U32 little-endian e l'ordine seguente è ABI:

| Offset | Campo | Semantica |
|---:|---|---|
| 0 | architecture | HCD_ARCH_AMD64 o HCD_ARCH_ARM64 |
| 4 | logical_cpu_id | CPU owner, esistente nel catalogo CPU |
| 8 | configuration_id | ID denso per CPU |
| 12 | translation_regime | AMD64=1, ARM64 stage 1=2, ARM64 stage 2=3 |
| 16 | flags | bit 0 CURRENT, bit 1 TRANSLATION_ENABLED; altri zero |
| 20 | exception_level | NONE=0 per AMD64, EL1=1 o EL2=2 per ARM64 |
| 24 | physical_address_bits | PA finale valida per questa configurazione, 1..52 |
| 28 | intermediate_address_bits | zero salvo stage 2 ARM64, dove è la larghezza IPA 1..52 |
| 32 | virtual_address_bits | VA per AMD64/stage 1 ARM64; zero per stage 2 |
| 36 | page_granule_bytes | granulo base in byte: 4096, 16384 o 65536 |
| 40 | level_count | profondità page-table; level index 0 è la radice |
| 44 | virtual_address_range | AMD64 canonical=1, ARM64 lower=2, upper=3, stage2=4 |
| 48 | tlb_features | mask delle operazioni normalizzate sotto |
| 52 | memory_attributes | mask degli attributi normalizzati sotto |
| 56 | reserved0 | zero |
| 60 | reserved1 | zero |

Vincoli di combinazione:
- AMD64 usa regime=1, exception level=0, granulo 4096, range canonical, intermediate bits=0, livelli 4 o 5 e VA width non superiore rispettivamente a 48 o 57 bit.
- ARM64 stage 1 usa regime=2, EL1/EL2, intermediate bits=0, granulo 4K/16K/64K, livelli 1..5, VA width 1..52 e range lower/upper.
- ARM64 stage 2 usa regime=3, EL2, granulo 4K/16K/64K, livelli 1..5, IPA width in `intermediate_address_bits`, `virtual_address_bits=0` e range stage2.
- PA bits è sempre 1..52. I campi di indirizzo descrivono capacità/configurazioni MMU, non mappature effettivamente installate.
- Per ogni CPU il modello comune ammette una sola configurazione CURRENT. Se l'entry state ha regimi simultanei o dettagli ISA non rappresentabili, il profilo Core deve rifiutare l'input finché un'estensione ISA-specifica versionata non definisce tali dettagli; non sceglie una configurazione per approssimazione.

`tlb_features`:
- bit 0 INVALIDATE_PAGE;
- bit 1 INVALIDATE_CONTEXT;
- bit 2 INVALIDATE_GLOBAL;
- bit 3 INVALIDATE_RANGE;
- bit 4 INVALIDATE_BROADCAST;
- bit 5..31 reserved.

`memory_attributes`:
- bit 0 NORMAL_WB;
- bit 1 NORMAL_WT;
- bit 2 NORMAL_WC;
- bit 3 NORMAL_UC;
- bit 4 DEVICE;
- bit 5 INNER_SHAREABLE;
- bit 6 OUTER_SHAREABLE;
- bit 7 NON_SHAREABLE;
- bit 8 EXECUTE_NEVER;
- bit 9..31 reserved.

Questi bit descrivono attributi supportati dalla configurazione, non attributi assegnati a ogni pagina. I permessi W^X, i mapping MMIO effettivi e l'ownership appartengono al piano di mapping e alla sua validazione.

### 29.3 `hcd_mmu_mapping_size` — 32 byte

| Offset | Campo | Semantica |
|---:|---|---|
| 0 | logical_cpu_id | CPU della configurazione |
| 4 | configuration_id | ID configurazione v2 esistente |
| 8 | level_index | indice root-to-leaf, zero-based e `< level_count` |
| 12 | mapping_kind | PAGE=1 o BLOCK=2 |
| 16 | size_bytes | dimensione del mapping in byte, potenza di due, al massimo 2^52 |
| 24 | flags | zero in versione record 1 |
| 28 | reserved0 | zero |

I record sono ordinati strettamente per `(logical_cpu_id, configuration_id, level_index, mapping_kind, size_bytes)` e non sono duplicati. Ogni configurazione deve dichiarare almeno una PAGE alla profondità terminale: la dimensione è esattamente `page_granule_bytes`. Una BLOCK deve essere più grande del granulo base e trovarsi prima della profondità terminale. Ogni configurazione deve avere almeno un record PAGE e al massimo 32 record totali. Le capacità PAGE/BLOCK restano distinte: una pagina base non viene dedotta da un blocco e viceversa.

### 29.4 `hcd_mmu_arm64_context` — 24 byte

Tutti i campi sono U32 little-endian:

| Offset | Campo | Semantica |
|---:|---|---|
| 0 | logical_cpu_id | CPU owner |
| 4 | configuration_id | configurazione stage 1 v2 corrispondente |
| 8 | context_kind | TTBR0=1/lower, TTBR1=2/upper |
| 12 | flags | bit 0 TRANSLATION_ENABLED; altri zero |
| 16 | exception_level | EL1=1 o EL2=2; deve coincidere con la configurazione |
| 20 | reserved0 | zero |

Il validator percorre in modo bounded e lineare la lista ordinata delle configurazioni e i record di contesto, verificando corrispondenza uno-a-uno, `context_kind` contro `virtual_address_range`, unicità per CPU e completezza EL1/EL2. Le configurazioni stage 2, se presenti, non ricevono record TTBR e non possono essere usate per simulare un regime stage 1 non rappresentato.

### 29.5 Requisiti di profilo e limiti

In major 2, sia CORE sia RECOVERY richiedono le sezioni MMU_CAPABILITIES, MMU_TRANSLATION_CONFIGS e MMU_MAPPING_SIZES con flag REQUIRED e conteggio non nullo. Per ARM64 richiedono anche MMU_ARM64_CONTEXTS con flag REQUIRED e conteggio non nullo; AMD64 deve ometterla. La recovery non rilassa checksum, bounds, cross-reference o validazione semantica. I limiti globali sono `HCD_MAX_CPUS * 16` configurazioni e contesti, `HCD_MAX_CPUS * 32` mapping; il limite `HCD_MAX_TOTAL_SIZE` resta sempre applicato. Le scansioni di validazione sono lineari e bounded dai conteggi verificati.

L'ABI v2 è un descrittore di capacità/configurazione. Non sostituisce il page-table plan, non crea tabelle e non autorizza il Core a modificare la MMU. Il producer MMU da snapshot raw ha test host sintetici AMD64/ARM64, ma deve ancora essere collegato agli entry adapter firmware e validato con letture reali in QEMU; i test host non costituiscono prova di boot.
