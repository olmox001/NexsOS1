# nxec — Preboot Memory Bootstrap Specification

**ID:** SPEC-PREBOOT-MEM-001  
**Versione:** 0.1.0-DESIGN  
**Stato:** DESIGN / IMPLEMENTATION GATE

## 1. Scopo e ownership

Il preboot costruisce il piano fisico e virtuale iniziale prima di consegnare il controllo al kernel. Il kernel deve poter ricostruire e verificare la stessa mappa senza fidarsi di puntatori o allocator del preboot.

La memoria è una risorsa registry-classified. Ogni extent dichiara base, length, memory type, attributes, NUMA node, owner e lifetime. La disponibilità non deriva dal fatto che una regione non sia elencata: solo HCD_MEMORY_USABLE è candidata all'allocazione generale, e solo dopo aver sottratto riserve.

## 2. Stati del bootstrap

DISCOVERED → NORMALIZED → RESERVED → PAGE_TABLES_READY → HANDOFF_READY → KERNEL_OWNED.

Ogni transizione ha precondizioni, output, errore stabile e possibilità di recovery. Nessuna fase pubblica una mappa parziale come valida. La mappa finale è immutabile al momento del handoff.

## 3. Fonti e normalizzazione

Fonti AMD64: UEFI memory map EFI post-EBS su Multiboot2 UEFI, Multiboot2 BIOS memory-map tag (derivato dalla mappa firmware/E820) su BIOS, ACPI SRAT/SLIT e firmware tables. Fonti ARM64: UEFI memory map acquisita e conclusa dal preboot prima di EBS, DTB/ACPI, NUMA e firmware tables. La precedenza e la risoluzione dei conflitti sono per fonte e campo, non un merge generico.

Gli intervalli sono validati con overflow-safe arithmetic, ordinati, non sovrapposti e con attributi coerenti. Intervalli contigui possono essere coalesced solo se type, attributes, owner, NUMA e lifetime sono equivalenti. Un conflitto non risolvibile porta a recovery o rifiuto.

## 4. Alternative allocator preboot

| Modello | Vantaggi | Costi |
|---|---|---|
| Bump allocator | semplice e bounded | non restituisce range e può frammentare |
| Bitmap allocator | facile per page granularity | workspace proporzionale a RAM |
| Buddy allocator | split/merge e classi di ordine | più complesso; non ideale prima di avere pagine |
| Extent allocator | gestisce range e allineamenti arbitrari | richiede tabella fissa e split/merge |
| Allocator firmware | già disponibile in boot services | lifetime e protocollo dipendono dal firmware |

Direzione proposta: extent allocator bounded nel preboot; il kernel inizializza un allocator page-based proprio dopo il trasferimento di ownership. L'uso dell'allocator firmware termina prima di ExitBootServices e non viene mantenuto come authority runtime.

## 5. Allocation request

Ogni richiesta dichiara:
- request ID e resource kind;
- size e alignment;
- min/max physical address;
- boundary constraint;
- memory attributes richiesti/vietati;
- NUMA preference e fallback policy;
- contiguity requirement;
- lifetime: permanent, until-kernel-adoption, reclaimable;
- owner e capability/boot role;
- failure policy.

Le richieste sono ordinate per dipendenza e priorità: page tables e stack necessari al passaggio, kernel image, HCD/BHD, decompression workspace, moduli, userland image e pool iniziali. L'ordine definitivo dipende dal boot path e non può sovrascrivere buffer ancora in uso.

## 6. Risorse e riserve

Riserve obbligatorie: firmware runtime, MMIO, framebuffer, loader/preboot attivo, HCD/BHD, stack, page tables, kernel segments, decompress workspace, boot modules e userland image. Le regioni ACPI reclaimable e firmware runtime non diventano RAM libera prima del rispettivo protocollo di rilascio.

Le pagine del kernel hanno permessi derivati dai segmenti ELF; stack NX; guard pages; page tables non eseguibili; MMIO con cache type coerente e nessun alias cache incompatibile. DMA è separato dalla normale allocazione CPU e richiede IOMMU o bounce pool.

## 7. Capacità e limiti

Il numero massimo di extent, richieste, riserve, split e page-table pages deve essere esplicito. Non si tronca mai la mappa. Se una piattaforma supera il limite, il preboot tenta coalescing semanticamente valido; se non basta, entra in recovery o rifiuta il boot.

I limiti iniziali sono parametri di progetto, non proprietà dello standard firmware. Vanno confrontati con mappe reali e sintetiche, poi congelati con test di saturazione. Nessuna allocazione runtime deve dipendere da heap non bounded.

## 8. Mapping virtuale

Il preboot prepara il mapping transitorio minimo per entrare nel kernel; il kernel costruisce la mappa definitiva. Le mappe dichiarano VA, PA, size, page granule, permissions, cacheability, shareability e lifetime. Tutti i range sono canonical/aligned secondo ISA e translation regime.

AMD64: 4-level/5-level paging se supportato e attivabile; NX, W^X, SMEP/SMAP/PCID sono capability distinte. ARM64: translation granule 4K/16K/64K, TCR/MAIR e livelli di translation sono specifici del regime; EL2/stage-2 è condizionale. Non si assume che una page size sia comune a entrambe le ISA.

## 9. NUMA e CPU eterogenee

Le riserve sono NUMA-aware quando la topologia è attendibile. Fallback remoto è esplicito e misurato. Il kernel può usare page pools per nodo e per CPU, ma il registry canonico mantiene owner e generation. CPU feature K2 richiedono affinity/migration policy; il memory planner non attribuisce feature di una CPU a tutte le altre.

## 10. Failure e graceful degradation

- memoria insufficiente: richiedere modalità alternativa solo se già autorizzata e compatibile; altrimenti recovery/rifiuto.
- frammentazione: provare un extent alternativo o scatter/gather solo per risorse che lo consentono.
- NUMA non disponibile: fallback dichiarato se la policy lo consente.
- MMU feature non disponibile: scegliere translation mode supportato senza indebolire W^X/capability.
- firmware map inconsistente: non indovinare range; recovery o rifiuto.
- overflow/capacity exhaustion: nessun wrap, nessuna truncation.

## 11. Test e metriche

Test su regioni sovrapposte, overflow base+length, allineamenti, boundary, memory type, attributes, NUMA, frammentazione, pool esauriti, coalescing, split capacity, region count massimo, firmware runtime, MMIO alias, DMA mask e page table limits.

QEMU: AMD64 q35/pc, BIOS/UEFI, 4-level/5-level quando esposto, RAM ridotta, SMP/topology, NUMA e feature flags; ARM virt con UEFI, 4K/16K/64K granule quando supportato, GIC e NUMA dove emulato. Ogni risultato distingue TCG, accelerazione host e hardware fisico.

Metriche: numero extent, scan steps, split/merge, frammentazione, massimo workspace, max observed latency, percentili e cicli. MAX_OBSERVED non è WCET; WCET richiede analisi separata.

## 12. Gate

Prima di collegare il planner al boot reale: congelare limiti dopo confronto, implementare extent planner host-testable, validare la mappa HCD, costruire page table plan senza scrivere registri, aggiungere test saturazione e poi adapter AMD64/ARM64. Nessun enable MMU prima che mapping e handoff siano verificati.


## 13. Primo planner implementato — stato e limiti v1

Il primo extent planner è implementato in preboot/common/memory_plan.c. Consuma solo una vista HCD già validata, seleziona MEMORY_USABLE, coalescesce soltanto extent contigui con attributes/NUMA/owner identici, usa first-fit e supporta alignment, min/max address, boundary, attribute masks, NUMA preference e fallback esplicito.

Limiti v1 provvisori: 4096 memory records HCD, 64 allocation requests, 4160 extent slots (un possibile split per request), workspace massimo 144 KiB. Il workspace è caller-owned e deve essere collocato in una riserva bootstrap, non su stack non qualificato. Il limite 4096 deve essere validato su firmware reali e sintetici; superamento comporta coalescing semanticamente sicuro o recovery/rifiuto, mai truncation.

Il workspace del planner deve essere disgiunto dalla vista HCD e dal relativo buffer di input. Request e risultato di un'allocazione devono essere disgiunti tra loro e dal workspace; intervalli con wrap o overlap sono rifiutati prima di qualsiasi modifica. Il planner non scrive page tables, non abilita MMU, non inizializza pagine, non esegue DMA e non implementa reclaim. Queste sono fasi separate. Test host/cross coprono first-fit, split, NUMA fallback esplicito, boundary, attributes, request validation, duplicate ID, memory-record cap e rifiuto alias con preservazione dei buffer.


## 14. UEFI memory ownership — protocol gate

UEFI firmware owns the physical memory map during preboot. A UEFI image may use only pages it has explicitly allocated with Boot Services; it must not treat all EfiConventionalMemory as directly allocatable before ExitBootServices. All buffers needed for handoff (memory map, HCD/BHD, kernel image, decompression workspace, page tables, stack and allocator workspace) must be acquired before the final memory-map/ExitBootServices sequence and recorded as reserved/BOOTSTRAP.

The UEFI adapter obtains the current map and MapKey with GetMemoryMap immediately before ExitBootServices. If the key is stale, it refreshes the map and retries. After the first failed ExitBootServices attempt, only Memory Allocation Services may be called; the retry path must not use filesystem, console, protocol or unrelated Boot Services.

The host-testable preboot/hal/uefi_exit.c protocol helper enforces a project bound of three ExitBootServices calls and four memory-map buffer growths. Buffer growth reserves room for eight additional descriptors, and each map is checked for nonzero size, descriptor version 1, minimum descriptor size and exact descriptor-size divisibility before ExitBootServices is called. The callback interface exposes only GetMemoryMap, memory allocation and ExitBootServices, including on retries. These are bounded project policy values, not UEFI requirements, and still require qualification against firmware. The helper is not yet wired to an EFI application entry point or a concrete firmware ABI adapter.

After ExitBootServices succeeds, the loader owns available memory and may treat EfiConventionalMemory and EfiBootServicesCode/Data as available. EfiRuntimeServicesCode/Data and EfiACPIMemoryNVS remain reserved; EfiACPIReclaimMemory is preserved until ACPI is enabled; EfiUnacceptedMemoryType is never allocated until the platform acceptance procedure completes. EfiLoaderCode/Data still containing preboot code/data remain HCD_MEMORY_BOOTSTRAP until kernel adoption and explicit reclaim.

The host-testable `preboot/hal/uefi_memory_map.c` normalizer now consumes a successful `preboot_uefi_exit_map` and transforms its caller-owned buffer in place into 40-byte HCD memory records. It accepts descriptor version 1, descriptor strides of at least 40 bytes, and at most HCD_MAX_TOTAL_SIZE input bytes. It validates reserved padding, page alignment, nonzero page counts and overflow; maps firmware memory types and representable cache/access/runtime/persistent attributes; sorts unsorted descriptors with bounded heapsort; coalesces only adjacent records with identical type, attributes, NUMA and owner; rejects overlaps and rejects a final map over 4096 records. Unknown types, MMIO port-space descriptors and attributes not represented by HCD v1 are explicit errors, never silently dropped. The map buffer is not valid normalized output on any error. This is a pure conversion helper, not a UEFI ABI adapter or boot claim.

UEFI descriptor type mapping is explicit: Reserved and PAL code -> HCD reserved; Loader code/data -> bootstrap; Boot Services code/data and Conventional -> usable (post-EBS only); Runtime Services code/data, or any descriptor with EFI_MEMORY_RUNTIME -> firmware runtime; Unusable -> unusable; ACPI Reclaim/NVS -> the matching HCD types; MMIO -> MMIO; Persistent and Unaccepted -> their matching HCD types. MMIO Port Space and unknown types are rejected. Cache attributes UC/UCE, WC, WT and WB map to the corresponding HCD cache policy; conflicting cache classes are rejected. WP/RO, RP and XP clear HCD write, read and execute permissions respectively. NV and Persistent descriptors set HCD persistent; runtime descriptors set HCD runtime. Other UEFI attributes are rejected because HCD v1 has no representation for them.

**Entry ownership decision:** AMD64 BIOS/Multiboot2 consumes the Multiboot2 BIOS/E820 memory map and has no EBS call. `preboot_multiboot2_normalize_memory_map` validates and maps this tag into HCD records; unknown types and malformed/unsorted/overlapping ranges are rejected, and absent attributes are not inferred. AMD64 UEFI/Multiboot2 uses GRUB's default path: GRUB calls ExitBootServices and supplies its post-EBS descriptors in the Multiboot2 EFI memory-map tag; nxec must not request the EFI Boot Services header tag. `preboot_multiboot2_get_efi_memory_map` validates the complete Multiboot2 information block and returns that tag's descriptor buffer to the existing UEFI normalizer; EFI_BS, duplicate, malformed and absent map tags are rejected. ARM64 UEFI is chainloaded as an EFI application; Boot Services remain active and nxec preboot owns EBS, using the bounded retry helper and the resulting map. For all paths, missing or malformed memory-map data is a bounded boot failure, not a guessed fallback. The path contracts are supported by inspection of the pinned GRUB 2.14 source; runtime entry adapter and QEMU validation remain required. See DEC-UEFI-ENTRY-OWNERSHIP-001 and PROBLEM-UEFI-ENTRY-OWNERSHIP-001.

HCD Core validation requires UEFI protocol images to set HCD_F_EFI_EXIT_BOOT_SERVICES_DONE and clear HCD_F_FIRMWARE_BOOT_SERVICES_ACTIVE. A recovery/incomplete descriptor may describe the active preboot phase, but it is not admissible as the kernel handoff. The physical extent planner independently checks the ownership flags before allocating.

Riferimento primario: UEFI Specification 2.11, Boot Services, Memory Allocation Services and ExitBootServices: https://uefi.org/specs/UEFI/2.11/07_Services_Boot_Services.html
