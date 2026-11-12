# nxec — Decision Log

Append-only. Una decisione successiva non cancella quella precedente: la supersede con motivazione, ambito e test.

## ID: DEC-HCD-001
**Titolo:** Separazione tra protocollo di boot e HCD normalizzato  
**Stato:** DESIGNED  
**Impatto:** COMPATIBILITY / SAFETY  
**Descrizione:** Le informazioni Multiboot2, UEFI, ACPI e DTB sono fonti di ingresso, non il formato interno del Core.  
**Analisi alternative:** mantenere strutture native del bootloader; normalizzare in un HCD unico; usare un HCD differente per ogni ISA.  
**Decision:** un HCD binario versionato, indipendente dall'ISA e dal boot transport; i dati raw sono preservati in sezioni di audit.  
**Test aggiunti:** validazione indipendente dal protocollo; input malformed per ciascun trasporto.  
**Metriche prima/dopo:** non ancora misurate.  
**Riferimenti codice:** docs/HCD_SPEC.md; preboot/common/hcd_wire.h.

## ID: DEC-BOOT-001
**Titolo:** Ingresso GRUB differenziato per AMD64 e ARM64  
**Stato:** DESIGNED  
**Impatto:** COMPATIBILITY  
**Descrizione:** Multiboot2 2.0 non definisce un architecture ID AArch64.  
**Analisi alternative:** inventare un ID; boot protocol custom; EFI PE/COFF per ARM64 tramite GRUB arm64-efi.  
**Decision:** mantenere GRUB come riferimento. AMD64 usa Multiboot2 nei percorsi supportati; ARM64 UEFI usa un'applicazione EFI AArch64 avviata da GRUB. L'ingresso AMD64 UEFI già in long mode è distinto dalla transizione 32-bit→long mode del percorso BIOS.  
**Test aggiunti:** AMD64 BIOS, AMD64 UEFI e ARM64 UEFI come target separati.  
**Metriche prima/dopo:** non ancora misurate.  
**Riferimenti codice:** docs/HCD_SPEC.md, sezione 3.

## ID: DEC-HCD-WIRE-001
**Titolo:** Layout wire HCD v1  
**Stato:** IMPLEMENTED_LAYOUT_NOT_VERIFIED_SEMANTICALLY  
**Impatto:** SAFETY / COMPATIBILITY  
**Descrizione:** Le strutture wire non possono dipendere da puntatori, size_t, long, bool, bitfield o packing implicito.  
**Analisi alternative:** serializzazione manuale byte per byte; struct C con ABI assert; strutture packed.  
**Decision:** tipi a larghezza fissa little-endian, struct C naturali con _Static_assert su dimensioni/allineamento/offset, decoder endian-safe obbligatorio.  
**Test aggiunti:** tests/unit/hcd_layout_test.c compilato per x86_64-elf e aarch64-none-elf con warning-as-error.  
**Metriche prima/dopo:** compilazione riuscita; nessun benchmark temporale.  
**Riferimenti codice:** preboot/common/hcd_wire.h; tests/unit/hcd_layout_test.c.

## ID: DEC-HCD-CRC-001
**Titolo:** Checksum strutturale HCD v1  
**Stato:** DESIGNED  
**Impatto:** SAFETY / PERFORMANCE  
**Descrizione:** Il kernel deve rilevare corruzione del descrittore senza dipendere da librerie dinamiche o feature CPU opzionali.  
**Analisi alternative:** CRC-64/ECMA-182; hash BLAKE3; firma crittografica.  
**Decision:** CRC-64/ECMA-182 per rilevamento della corruzione; non è autenticazione. Firma/secure boot restano livelli separati da progettare.  
**Test aggiunti:** test vectors CRC, mutazioni e checksum su buffer con campo checksum azzerato, da implementare.  
**Metriche prima/dopo:** non misurate.  
**Riferimenti codice:** docs/HCD_SPEC.md, sezione 6.

## ID: DEC-HCD-FEATURES-001
**Titolo:** Stati distinti delle feature CPU  
**Stato:** DESIGNED  
**Impatto:** SAFETY / COMPATIBILITY  
**Descrizione:** Feature dichiarata dall'hardware non equivale a feature abilitata e utilizzabile dall'OS.  
**Analisi alternative:** bitmap unico; SUPPORTED/ENABLED/USABLE separati; capability per feature senza stato hardware.  
**Decision:** tre bitmap separati per CPU e piattaforma, con invariante USABLE ⊆ ENABLED ⊆ SUPPORTED e provenienza.  
**Test aggiunti:** subset invariants e CPU eterogenee, da implementare.  
**Metriche prima/dopo:** non misurate.  
**Riferimenti codice:** docs/HCD_SPEC.md, sezioni 9-10; preboot/common/hcd_wire.h.

## ID: DEC-HCD-CRC-002
**Titolo:** Implementazione CRC-64 freestanding a tabella statica  
**Stato:** IMPLEMENTED_PENDING_BENCHMARK  
**Impatto:** PERFORMANCE / BUILD  
**Descrizione:** Il CRC deve funzionare senza libc, allocazioni dinamiche o feature CPU opzionali, su AMD64 e ARM64.  
**Analisi alternative:** bitwise (8 iterazioni per byte, footprint minimo); tabella statica 256×U64 (2 KiB e un lookup per byte); accelerazione hardware (non uniforme tra ISA e non assunta disponibile).  
**Decision:** tabella statica generata deterministicamente da tools/gen-hcd-crc-table.py; nessuna inizializzazione runtime e nessuna allocazione. La scelta resta soggetta a benchmark AMD64/ARM64 e confronto con la variante bitwise.  
**Test aggiunti:** test vector standard CRC-64/ECMA-182, campo checksum HCD azzerato logicamente, input vuoto e parametri invalidi; cross-compile dei sorgenti.  
**Metriche prima/dopo:** correttezza PASS; performance non ancora misurata.  
**Riferimenti codice:** preboot/common/hcd_crc64.c; preboot/common/hcd_crc64_table.inc; preboot/common/hcd_assert.h; tools/gen-hcd-crc-table.py; tests/unit/hcd_crc64_test.c.

## ID: DEC-TEST-001
**Titolo:** Test runner riproducibile iniziale  
**Stato:** IMPLEMENTED  
**Impatto:** BUILD / VERIFICATION  
**Descrizione:** I test devono essere eseguibili con un comando ripetibile, senza dipendenze esterne dopo il setup della toolchain.  
**Analisi alternative:** comandi manuali; script shell; framework esterno.  
**Decision:** script POSIX shell tools/test-runner.sh e target Makefile; test host, cross-compile AMD64/ARM64 e analisi statica separabili.  
**Test aggiunti:** make test-host e make test-cross PASS.  
**Metriche prima/dopo:** esecuzione host e cross completata; timing non misurato.  
**Riferimenti codice:** Makefile; tools/test-runner.sh.

## ID: DEC-HCD-PARSER-001
**Titolo:** Validazione HCD con workspace preallocato  
**Stato:** IMPLEMENTED_PARTIAL_SEMANTICS  
**Impatto:** SAFETY / MEMORY / DETERMINISM  
**Descrizione:** Il parser deve validare input ostile senza allocazioni dinamiche e senza pubblicare viste parziali.  
**Analisi alternative:** allocazione durante parsing; workspace statico globale; workspace preallocato fornito dal chiamante.  
**Decision:** workspace fornito dal chiamante con massimo 64 viste sezione; vista pubblicata solo dopo esito positivo; error detail stabile; input little-endian letto byte per byte.  
**Test aggiunti:** fixture HCD recovery, checksum, directory bounds, duplicati, overlap, flag external, reserved, feature subset, CPU count e memory overflow.  
**Metriche prima/dopo:** host e cross compile AMD64/ARM64 PASS; nessuna misura temporale.  
**Riferimenti codice:** preboot/common/hcd_parser.c; preboot/common/hcd_semantics.c; preboot/common/hcd_parser.h; tests/unit/hcd_parser_test.c.

## ID: DEC-HCD-BOUNDS-001
**Titolo:** Ordinamento canonico e workspace HCD bounded  
**Stato:** IMPLEMENTED  
**Impatto:** DETERMINISM / MEMORY / SAFETY  
**Descrizione:** Evitare allocazioni e lookup lineari annidati su dati HCD non fidati.  
**Analisi alternative:** hash map dinamiche; scansioni annidate; ordinamento canonico con bitmap e ricerche binarie.  
**Decision:** ID logici densi, record ordinati, workspace caller-owned entro 12 KiB, binary search su memory map e intervalli interrupt.  
**Test aggiunti:** profilo core valido e mutazioni di MMU, boot range, topology, MMIO controller e timer interrupt.  
**Metriche prima/dopo:** host e cross compile PASS; bound di iterazioni documentati; WCET non misurato.  
**Riferimenti codice:** hcd_parser.h; hcd_semantics.c; hcd_core_profile_test.c.

## ID: DEC-HCD-INTERRUPTS-001
**Titolo:** Intervalli interrupt ordinati e parent graph verificato  
**Stato:** IMPLEMENTED  
**Impatto:** INTERRUPTS / SAFETY / BOUNDEDNESS  
**Descrizione:** La relazione gerarchica dei controller non deve essere confusa con l'ordinamento degli intervalli interrupt.  
**Analisi alternative:** parent_id minore del controller_id; scansioni pairwise illimitate; controller ordinati per base con ciclo parent separato.  
**Decision:** massimo 256 controller, ID densi e intervalli ordinati/non sovrapposti; parent valido e ciclo rilevato con Floyd in massimo 256 passi.  
**Test aggiunti:** fixture core e mutazione self-parent cycle.  
**Metriche prima/dopo:** host e cross compile PASS; WCET non misurato.  
**Riferimenti codice:** hcd_semantics.c; hcd_core_profile_test.c.

## ID: DEC-HCD-BUILDER-001
**Titolo:** Builder HCD caller-owned con scrittura diretta delle sezioni  
**Stato:** IMPLEMENTED  
**Impatto:** BOOT / MEMORY / ZERO-COPY  
**Descrizione:** Produrre HCD senza allocazioni dinamiche e senza buffer temporanei per sezione.  
**Analisi alternative:** buffer per sezione con copia finale; directory massima preallocata; doppio passaggio con directory finale.  
**Decision:** buffer caller-owned, directory riservata massima 64 entry, payload scritto direttamente, classificazione sezioni centralizzata e finalizzazione CRC.  
**Test aggiunti:** round-trip builder→parser recovery; duplicato, capacità e alignment errati.  
**Metriche prima/dopo:** host PASS; cross compile AMD64/ARM64 PASS; tempo builder non misurato.  
**Riferimenti codice:** hcd_builder.c; hcd_builder.h; hcd_section_catalog.c; hcd_builder_test.c.


## ID: DEC-ARCH-PROJECT-001
**Titolo:** Sottoprogetti indipendenti e root orchestration  
**Stato:** DESIGNED  
**Impatto:** BUILD / MAINTAINABILITY / SECURITY  
**Descrizione:** Preboot, kernel, moduli, userland e bootloader devono mantenere build, ABI, manifest, test e revisioni separati.  
**Analisi alternative:** monorepo monolitico; sottoprogetti con build autonome; repository separati con root orchestrator.  
**Decision:** sottoprogetti autonomi nello stesso workspace, ciascuno con build propria; root orchestration verifica artefatti e versioni. Directory create progressivamente con contenuto reale.  
**Test aggiunti:** preboot Makefile indipendente; kernel Makefile indipendente e gate HCD.  
**Metriche prima/dopo:** build host/cross preboot e kernel PASS; nessuna metrica temporale.  
**Riferimenti codice:** PROJECT_BUILD_GRAPH.md; preboot/Makefile; kernel/Makefile.

## ID: DEC-BOOT-IMAGE-001
**Titolo:** Kernel precompilato in bundle verificabile  
**Stato:** DESIGNED — formato non congelato  
**Impatto:** BOOT / SECURITY / PERFORMANCE  
**Descrizione:** Il preboot deve validare, decomprimere e caricare un'immagine kernel precompilata prima del salto all'entry ELF.  
**Analisi alternative:** ELF diretto; cpio/tar; bundle NXB con manifest e componenti.  
**Decision:** adottare il modello bundle versionato NXB come direzione; tenere kernel image e userland image separate. NXB layout, firma, compressione e relocation restano gate di confronto.  
**Test aggiunti:** catalogo malformed bundle, signature, decompression, ELF, W^X e handoff.  
**Metriche prima/dopo:** non misurate.  
**Riferimenti codice:** docs/BOOT_IMAGE_SPEC.md; docs/ARCHITECTURE_V2.md.

## ID: DEC-PREBOOT-HAL-001
**Titolo:** HAL ISA anche nel preboot  
**Stato:** DESIGNED  
**Impatto:** PORTABILITY / BOOT / COMPATIBILITY  
**Descrizione:** Il preboot deve supportare più ISA tramite adapter specifici e un contratto comune, senza confondere i diversi stati di ingresso.  
**Analisi alternative:** codice per-ISA duplicato; preboot comune con HAL ISA; boot protocol unico forzato.  
**Decision:** HAL ISA preboot separata dalla HAL kernel; HCD resta indipendente dall'ISA; ingressi AMD64 BIOS, AMD64 UEFI e ARM64 UEFI restano distinti.  
**Test aggiunti:** cross compile HCD e kernel HCD gate su AMD64/ARM64; boot QEMU ancora OPEN.  
**Metriche prima/dopo:** cross compile PASS; nessun boot reale.  
**Riferimenti codice:** docs/ARCHITECTURE_V2.md; docs/HCD_SPEC.md.

## ID: DEC-REG-001
**Titolo:** REG come protocollo comune asincrono di risorse ed eventi  
**Stato:** DESIGNED — wire format OPEN  
**Impatto:** SECURITY / PERFORMANCE / IPC  
**Descrizione:** Comunicazione comune, push/request, eventi, capability e buffer condivisi, con percorso per-core e registry logicamente centralizzato.  
**Analisi alternative:** RPC sincrono; 9P-like request/reply; shared-memory rings; modello ibrido.  
**Decision:** REG è protocollo tipizzato asincrono; 9P ispira framing e cancellation, non autorità capability. Code bounded per endpoint/core, backpressure, completion e revocation esplicite. REG non esegue codice ricevuto.  
**Test aggiunti:** pianificati cancellation race, replay, backpressure, queue overflow, revoke e DMA.  
**Metriche prima/dopo:** non misurate.  
**Riferimenti codice:** docs/REG_PROTOCOL.md.

## ID: DEC-SEC-DOMAIN-001
**Titolo:** Domini MACHINE/SYSTEM/ROOT/USER/SHARED e classe DRIVER  
**Stato:** DESIGNED — threat model da completare  
**Impatto:** SECURITY / ISOLATION  
**Descrizione:** Distinguere privilegio hardware, servizi privilegiati userland, sandbox amministrativa, utenti e risorse condivise.  
**Analisi alternative:** UID/privilege tradizionale; capability domains; mapping per-ISA con principal logici.  
**Decision:** domini logici indipendenti dai livelli ISA. MACHINE non è accessibile da userland; SYSTEM è servizio userland e syscall API; ROOT resta sandbox; DRIVER è modulo userland con capability device.  
**Test aggiunti:** pianificati privilege escalation, cross-tenant, symlink escape, capability forgery e revoke races.  
**Metriche prima/dopo:** non misurate.  
**Riferimenti codice:** docs/SECURITY_DOMAINS.md.

## ID: DEC-USERLAND-IMAGE-001
**Titolo:** Userland come seconda immagine e VFS come servizio userland  
**Stato:** DESIGNED — formato OPEN  
**Impatto:** ISOLATION / STORAGE / COMPATIBILITY  
**Descrizione:** Spazio kernel separato dalla VFS utenti; mount mediato da registry e capability.  
**Analisi alternative:** VFS nel kernel; root image cpio; SquashFS; container NXUI custom.  
**Decision:** kernel mantiene mount authority e namespace; VFS e filesystem driver sono moduli userland. NXUI è immagine separata read-only con overlay dati per utente; cpio/tar/SquashFS/NXFS da confrontare.  
**Test aggiunti:** pianificati sdN isolation, mount revoke, symlink traversal, image integrity e journal recovery.  
**Metriche prima/dopo:** non misurate.  
**Riferimenti codice:** docs/VFS_USERLAND_SPEC.md.

## ID: DEC-PREBOOT-RECLAIM-001
**Titolo:** Reclaim preboot dopo trasferimento ownership  
**Stato:** DESIGNED  
**Impatto:** MEMORY / SECURITY / RECOVERY  
**Descrizione:** Valutare se conservare preboot come monitor di basso livello o recuperare le sue pagine.  
**Analisi alternative:** reclaim completo; codice residente; audit data residente e monitor kernel separato.  
**Decision:** default reclaim del codice dopo handoff verificato; conservare audit record immutabile. Un servizio di prevenzione errori è SYSTEM userland. Eventuale monitor MACHINE è modulo kernel separato, non accesso implicito al preboot.  
**Test aggiunti:** pianificati lifetime/mapping reclamation e audit integrity.  
**Metriche prima/dopo:** non misurate.  
**Riferimenti codice:** docs/BOOT_IMAGE_SPEC.md, sezione 9.

## ID: DEC-GRUB-LICENSE-001
**Titolo:** Sorgente GRUB come progetto upstream GPL separato  
**Stato:** DESIGNED — audit distribuzione richiesto  
**Impatto:** LEGAL / BUILD / BOOT  
**Descrizione:** Il requisito di conservare e compilare il source GRUB incontra la restrizione fondativa sulle licenze ammesse per codice incorporato.  
**Analisi alternative:** non includere GRUB source; importare codice nel kernel; mantenere GRUB come progetto upstream indipendente.  
**Decision:** sorgente GRUB 2.14 mantenuto in repository Git separato dentro boot/grub/upstream, non incorporato né linkato nel kernel; GPL-3.0-or-later e source obligations restano applicabili. Root license nxec non lo relicenzia.  
**Test aggiunti:** revisione tag/commit e presenza COPYING; build target ancora da completare.  
**Metriche prima/dopo:** clone sorgente PASS; build non ancora eseguita.  
**Riferimenti codice:** boot/grub/README.md; boot/grub/UPSTREAM_REVISION.


## ID: DEC-PREBOOT-MEMORY-001
**Titolo:** Extent planner bounded per il bootstrap fisico  
**Stato:** IMPLEMENTED_TESTED — policy first-fit iniziale  
**Impatto:** MEMORY / BOOT / NUMA / SECURITY  
**Descrizione:** Il preboot deve assegnare range fisici per kernel, stack, page tables, HCD/BHD, workspace e immagini senza heap dinamico e senza usare regioni non USABLE.  
**Analisi alternative:** bump allocator, bitmap, buddy, extent allocator, allocator firmware; first-fit vs best-fit/top-down/NUMA-aware.  
**Decision:** extent planner caller-owned e bounded, first-fit iniziale, coalescing solo per classi identiche, richieste con size/alignment/min/max/boundary/attributes/NUMA/owner e fallback NUMA esplicito.  
**Test aggiunti:** split, first-fit, NUMA fallback, boundary, attributes, duplicate ID, bad alignment e cap regioni.  
**Metriche prima/dopo:** host PASS; cross AMD64/ARM64 PASS; workspace <=144 KiB; nessuna misura timing/WCET.  
**Riferimenti codice:** preboot/common/memory_plan.c; preboot/tests/unit/memory_plan_test.c; docs/MEMORY_BOOTSTRAP_SPEC.md.

## ID: DEC-HCD-MEMORY-LIMIT-001
**Titolo:** Limite v1 di 4096 record MEMORY_MAP  
**Stato:** IMPLEMENTED / PROVISIONAL  
**Impatto:** HCD ABI / BOOT / BOUNDED WORK  
**Descrizione:** HCD_MAX_TOTAL_SIZE da solo consentiva un numero elevato di record memory map, oneroso per workspace e scansioni.  
**Analisi alternative:** limite non specificato; 4096; 16384; coalescing lato produttore; rifiuto recovery.  
**Decision:** 4096 record v1 provvisorio; coalescing solo se type, attributes, owner, NUMA e lifetime sono identici; oltre il limite recovery/rifiuto, mai truncation. Il valore resta da qualificare con mappe firmware reali.  
**Test aggiunti:** parser e builder rifiutano 4097 record; memory planner workspace 4160 extent slots.  
**Metriche prima/dopo:** HCD parser/builder host e cross PASS; nessun WCET dichiarato.  
**Riferimenti codice:** preboot/common/hcd_wire.h; hcd_parser.c; hcd_builder.c.


## ID: DEC-UEFI-MEMORY-OWNERSHIP-001
**Titolo:** Ownership UEFI e allocazione fisica solo dopo ExitBootServices  
**Stato:** IMPLEMENTED_TESTED — UEFI adapter/retry ancora da implementare  
**Impatto:** BOOT / MEMORY / SECURITY  
**Descrizione:** Prima di ExitBootServices la memory map appartiene al firmware; il preboot non può allocare direttamente tutti i range ConventionalMemory.  
**Analisi alternative:** allocator firmware; extent allocator pre-EBS; allocator extent dopo ownership transfer.  
**Decision:** prima di EBS si usano AllocatePages/AllocatePool e si riservano tutti i buffer; dopo EBS si costruisce HCD Core e si inizializza extent planner. Il profilo CORE in modalità UEFI richiede EFI_EXIT_BOOT_SERVICES_DONE e vieta FIRMWARE_BOOT_SERVICES_ACTIVE.  
**Test aggiunti:** UEFI active rejected; UEFI done accepted; unaccepted memory never allocated.  
**Metriche prima/dopo:** host/cross/sanitizer PASS; retry count e timing non misurati.  
**Riferimenti codice:** preboot/common/hcd_semantics.c; memory_plan.c; docs/MEMORY_BOOTSTRAP_SPEC.md.

## ID: DEC-HCD-UNACCEPTED-MEMORY-001
**Titolo:** Memoria fisica unaccepted mai allocabile  
**Stato:** IMPLEMENTED_TESTED  
**Impatto:** MEMORY / PLATFORM SECURITY  
**Descrizione:** Alcune piattaforme dichiarano memoria presente ma non ancora accettata secondo il protocollo architetturale.  
**Analisi alternative:** trattarla come reserved; accettarla nel preboot; tipizzarla e attendere un acceptance procedure.  
**Decision:** nuovo HCD_MEMORY_UNACCEPTED; il planner alloca soltanto HCD_MEMORY_USABLE. La procedura di acceptance è specifica per piattaforma e non è implicita.  
**Test aggiunti:** parser accetta il tipo e memory planner non lo considera allocabile.  
**Metriche prima/dopo:** host/cross PASS; nessun timing dichiarato.  
**Riferimenti codice:** preboot/common/hcd_wire.h; memory_plan.c; memory_plan_test.c.

## ID: DEC-REG-CANCELLATION-001
**Titolo:** Cancellation REG con drenaggio e acknowledgement ispirati a 9P Tflush  
**Stato:** DESIGN  
**Impatto:** IPC / ASYNC / MEMORY LIFETIME / SECURITY  
**Descrizione:** Le richieste asincrone devono poter essere annullate senza riuso prematuro di ID o buffer e senza fingere rollback di effetti già committati.  
**Analisi alternative:** cancel best-effort; flush/ack; transactional cancel; timeout-only.  
**Decision:** request_id+generation, CANCELLED/TOO_LATE/ALREADY_TERMINAL/DENIED, ack dopo garanzia di nessun ulteriore effetto, buffer/ID mantenuti fino alla terminal resolution. Revocation resta separata.  
**Test aggiunti:** da implementare con REG endpoint e race completion/cancel.  
**Metriche prima/dopo:** non implementato, nessuna misura.  
**Riferimenti codice:** docs/REG_PROTOCOL.md; 9P2000 Tflush/Rflush.

## ID: DEC-SEL4-BOOTSTRAP-001
**Titolo:** seL4 come modello root task/BootInfo/capability, non VFS kernel  
**Stato:** DESIGN  
**Impatto:** BOOT / CAPABILITY / USERLAND  
**Descrizione:** Il modello seL4 utile è il trasferimento a un initial/root task con BootInfo e capability untyped; la VFS non viene attribuita al kernel.  
**Analisi alternative:** rootfs nel kernel; initramfs userland; initial task con capability e servizio VFS.  
**Decision:** kernel image e userland root image separate; il kernel passa capability iniziali, il servizio SYSTEM monta VFS e root image.  
**Test aggiunti:** da implementare nel boot handoff e userland image loader.  
**Metriche prima/dopo:** non implementato.  
**Riferimenti codice:** docs/VFS_USERLAND_SPEC.md; seL4 BootInfo/capability documentation.

## ID: DEC-UEFI-MEMORY-NORMALIZER-001

**Titolo:** Normalizzazione UEFI bounded dopo trasferimento ownership

**Stato:** IMPLEMENTED_TESTED — adapter firmware e qualificazione aperti

**Impatto:** BOOT / MEMORY / SAFETY

**Descrizione:** La mappa UEFI deve essere convertita in record HCD senza cast su input non allineati, perdita di extent o dipendenze da heap.

**Analisi alternative:** mantenere descrittori firmware nel kernel; convertire in output allocato separatamente; compattare nel buffer caller-owned e ordinare solo quando necessario.

**Decision:** consumare soltanto una mappa con ExitBootServices riuscita; validare descriptor v1/stride/bounds e pagine; convertire in place; heapsort bounded solo se l'input non è ordinato; coalescing solo con policy identica; rifiuto esplicito di tipi/attributi non rappresentati e mappe oltre i limiti HCD. Il contratto HCD non cambia.

**Test aggiunti:** TEST-UEFI-MEMORY-MAP-NORMALIZER-001.

**Metriche prima/dopo:** host, AMD64/ARM64 cross-link, ASan/UBSan, clang-tidy e cppcheck PASS; nessun timing/WCET misurato.

**Riferimenti codice:** preboot/hal/uefi_memory_map.c; preboot/tests/unit/uefi_memory_map_test.c; docs/MEMORY_BOOTSTRAP_SPEC.md.

## ID: DEC-UEFI-ENTRY-OWNERSHIP-001

**Titolo:** Ownership della memory map e di ExitBootServices per i percorsi GRUB

**Stato:** RESOLVED_BY_DESIGN / PINNED_GRUB_SOURCE_VERIFIED

**Impatto:** BOOT / MEMORY OWNERSHIP / FIRMWARE

**Descrizione:** I percorsi AMD64 BIOS/Multiboot2, AMD64 UEFI/Multiboot2 e ARM64 UEFI hanno trasporti e ownership firmware distinti; l'entry path deve dichiarare esplicitamente owner e fonte della mappa.

**Analisi alternative:** un adapter EFI comune; delegare EBS a GRUB su tutti i target; affidare EBS all'applicazione EFI soltanto quando il firmware la avvia direttamente.

**Decision:** AMD64 BIOS/Multiboot2 consuma la memory map Multiboot2 BIOS/E820 e non usa EBS. AMD64 UEFI/Multiboot2 delega EBS a GRUB: la build nxec non richiede il tag header EFI Boot Services e deve richiedere/validare il tag EFI memory map post-EBS. ARM64 UEFI viene avviato da GRUB chainloader con EFI Boot Services attivi; l'applicazione preboot nxec possiede EBS, usa `preboot_uefi_exit_boot_services` e normalizza la mappa risultante. Non si usano Boot Services dopo EBS riuscita. Ogni trasporto viene validato prima di pubblicare un HCD Core; una mappa assente o incoerente causa rifiuto/recovery bounded.

**Test aggiunti:** ispezione del sorgente GRUB pinned 2.14: `loader/multiboot_mbi2.c` disabilita `keep_bs` per default e chiama `grub_efi_finish_boot_services` quando costruisce il tag EFI memory map; `kern/efi/mm.c` chiama `ExitBootServices`; `loader/efi/chainloader.c` avvia il PE/COFF figlio tramite `StartImage`. Test d'integrazione firmware/QEMU ancora da implementare.

**Metriche prima/dopo:** N/A.

**Riferimenti codice:** boot/grub/upstream/grub-core/loader/multiboot_mbi2.c; boot/grub/upstream/grub-core/kern/efi/mm.c; boot/grub/upstream/grub-core/loader/efi/chainloader.c; docs/HCD_SPEC.md §3; docs/MEMORY_BOOTSTRAP_SPEC.md §14.

## ID: DEC-MULTIBOOT2-MEMORY-MAP-001

**Titolo:** Normalizzazione bounded del tag memory map Multiboot2

**Stato:** IMPLEMENTED_TESTED — host/cross/sanitizer/static; firmware integration pending

**Impatto:** BOOT / MEMORY / SAFETY

**Descrizione:** AMD64 BIOS/Multiboot2 deve tradurre il tag type 6 in record HCD senza cast su input non allineati, dipendenze da heap o perdita di extent.

**Analisi alternative:** usare soltanto le EFI descriptor; fidarsi dell'input senza validazione; ordinare in-place; rifiutare mappe non canoniche e normalizzare in output caller-owned.

**Decision:** richiedere tag completo type 6, entry version 0, stride almeno 24 byte, reserved zero, tipi noti, size/base non overflowanti e input ordinato/non sovrapposto. Mappare available/reserved/ACPI reclaimable/NVS/bad RAM rispettivamente ai tipi HCD equivalenti; rifiutare tipi ignoti. Coalescere solo intervalli adiacenti dello stesso tipo. Gli attributi cache/protection, owner e NUMA non presenti nel tag rimangono unknown/zero, mai inferiti. Output caller-owned con capienza HCD esplicita, senza truncation.

**Test aggiunti:** TEST-MULTIBOOT2-MEMORY-MAP-001.

**Metriche prima/dopo:** host, AMD64/ARM64 freestanding cross-link, ASan/UBSan, clang-tidy e cppcheck PASS; firmware/QEMU non eseguito.

**Riferimenti codice:** preboot/hal/multiboot2_memory_map.c; preboot/tests/unit/multiboot2_memory_map_test.c; Multiboot2 Specification, memory map tag.

## ID: DEC-MULTIBOOT2-EFI-MAP-TAG-001

**Titolo:** Estrazione della mappa EFI post-EBS dal trasporto Multiboot2

**Stato:** IMPLEMENTED_TESTED — parser host/cross/sanitizer/static; entry firmware pending

**Impatto:** BOOT / MEMORY OWNERSHIP / FIRMWARE

**Descrizione:** AMD64 UEFI/Multiboot2 riceve da GRUB una EFI memory map post-EBS; il preboot deve distinguere questo percorso da un tag che dichiari Boot Services ancora attivi.

**Analisi alternative:** assumere EBS completato senza ispezionare i tag; parsare la mappa perdendo descriptor EFI e attributi; validare l'information block e restituire la vista dei descriptor originale.

**Decision:** validare bounds, header, allineamento e terminazione dell'intero Multiboot2 information block; richiedere un singolo tag EFI memory map con descriptor v1/stride >=40 e payload divisibile; rifiutare EFI_BS, duplicati e mappe mancanti. Passare il payload originale al normalizzatore UEFI esistente, senza copie o inferenze. Questo parser non implementa l'entry runtime.

**Test aggiunti:** TEST-MULTIBOOT2-EFI-MAP-TAG-001.

**Metriche prima/dopo:** host, AMD64/ARM64 freestanding cross-link, ASan/UBSan, clang-tidy e cppcheck PASS; firmware/QEMU non eseguito.

**Riferimenti codice:** preboot/hal/multiboot2_efi_map.c; preboot/tests/unit/multiboot2_efi_map_test.c; Multiboot2 Specification; GRUB 2.14 pinned loader.

## ID: DEC-HCD-MMU-V2-001

**Titolo:** Estensione additiva HCD v2 per configurazioni e mapping MMU

**Stato:** IMPLEMENTED_TESTED — host/cross/sanitizer/static; firmware wiring and QEMU pending

**Impatto:** COMPATIBILITY / SAFETY / MMU

**Descrizione:** I record MMU v1 non descrivono senza ambiguità regimi, granularità attiva e mapping supportati. La loro reinterpretazione o modifica romperebbe la compatibilità wire.

**Analisi alternative:** ridefinire i record v1; sostituire l'intero schema; aggiungere sezioni versionate in major 2 conservando contenitore e sezioni v1; rinviare la semantica MMU.

**Decision:** conservare HCD major 1 e i record v1 invariati. Major 2 aggiunge le sezioni REQUIRED `MMU_TRANSLATION_CONFIGS` e `MMU_MAPPING_SIZES`; i consumer validano esplicitamente la major e rifiutano downgrade impliciti. Le configurazioni descrivono capability/regime, non il page-table plan. I dettagli non rappresentabili devono essere rifiutati finché non definiti in un'estensione versionata.

**Test aggiunti:** TEST-HCD-MMU-V2-001.

**Metriche prima/dopo:** `make -C preboot test` e `make -C kernel test` PASS, inclusi test host, cross-link AMD64/ARM64, ASan/UBSan, clang-tidy e cppcheck. I casi MMU v2 coprono compatibilità v1, AMD64 4/5 livelli, ARM64 stage 1 e input ostili. La cattura/produzione da snapshot raw è stata aggiunta dopo questa decisione; resta da collegare agli entry firmware e verificare in QEMU.

**Riferimenti codice:** docs/HCD_SPEC.md §29; preboot/common/hcd_wire.h; kernel/core/hcd/hcd_wire.h; validator, builder e test HCD preboot/kernel.

## ID: DEC-HCD-ARM64-TTBR-CONTEXT-001

**Titolo:** Estensione versionata per i contesti ARM64 TTBR simultanei

**Stato:** IMPLEMENTED_TESTED_HOST — cross, sanitizer e analisi statiche da verificare

**Impatto:** COMPATIBILITY / SAFETY / MMU

**Descrizione:** HCD v2 comune permette un solo `CURRENT` e ammette `TRANSLATION_ENABLED` soltanto su tale configurazione. ARM64 EL1 può avere contemporaneamente attivi i regimi TTBR0 e TTBR1; rimuovere quei controlli o selezionare arbitrariamente un solo regime perde informazioni e rende i validator non conformi.

**Analisi alternative:** rilassare la semantica comune; rifiutare ogni entry state ARM64 EL1 con entrambi i regimi; aggiungere una sezione ISA-specifica versionata, mantenendo invariati tutti i controlli comuni e descrivendo ogni contesto stage 1.

**Decision:** aggiungere la sezione HCD v2 type 18 `MMU_ARM64_CONTEXTS`. Ogni record riferisce una configurazione stage 1 esistente per CPU/configuration ID, identifica TTBR0/lower o TTBR1/upper e dichiara lo stato di traduzione indipendente. I record sono ordinati e verificati uno-a-uno; EL1 richiede entrambi i contesti, EL2 il solo TTBR0. Il producer acquisisce il PC e marca `CURRENT` sulla configurazione il cui range contiene l'istruzione corrente. Per CURRENT, lo stato attivo dell'estensione deve coincidere con il flag comune. Per un contesto non-CURRENT, l'attività si rappresenta solo nell'estensione; il flag comune resta vietato. Entrambi i validator restano indipendenti e conservano il controllo comune.

**Test aggiunti:** TEST-HCD-ARM64-MMU-CONTEXT-001.

**Metriche prima/dopo:** suite host preboot/kernel PASS. La verifica cross-link AMD64/ARM64, ASan/UBSan, clang-tidy e cppcheck è ancora necessaria; nessun boot QEMU o entry firmware verificato.

**Riferimenti codice:** docs/HCD_SPEC.md §29; `preboot/hal/mmu.c`; `preboot/hal/arm64/isa.c`; validator indipendenti preboot/kernel e fixture MMU.
