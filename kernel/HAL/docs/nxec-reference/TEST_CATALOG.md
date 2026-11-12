# nxec — Test Catalog

## ID: TEST-HCD-LAYOUT-001
**Titolo:** Compilazione layout HCD wire su AMD64 e ARM64  
**Stato:** PASS  
**Impatto:** SAFETY / COMPATIBILITY  
**Descrizione:** Verifica compile-time delle dimensioni e degli allineamenti di tutte le strutture wire HCD v1 tramite _Static_assert.  
**Analisi alternative:** test host-only; cross compile AMD64; cross compile AMD64 e ARM64.  
**Decision:** cross-compilazione per entrambi i target freestanding.  
**Test aggiunti:** tests/unit/hcd_layout_test.c.  
**Metriche prima/dopo:** x86_64-elf: PASS; aarch64-none-elf: PASS. Nessun tempo WCET misurato.  
**Riferimenti codice:** preboot/common/hcd_wire.h; tests/unit/hcd_layout_test.c.  
**Comando:** clang -std=c17 -target TARGET -ffreestanding -Wall -Wextra -Werror -Wpedantic -I. -c tests/unit/hcd_layout_test.c -o /tmp/hcd-layout.o

## ID: TEST-HCD-CRC-001
**Titolo:** CRC-64/ECMA-182 test vectors  
**Stato:** PLANNED  
**Impatto:** SAFETY  
**Descrizione:** Verificare il test vector standard "123456789" e i casi buffer vuoto, zero, checksum field azzerato e input mutato.  
**Analisi alternative:** test vector standard; confronto con implementazione indipendente; fuzzing bounded.  
**Decision:** usare test vector pubblico e cross-check indipendente.  
**Test aggiunti:** da implementare.  
**Metriche prima/dopo:** N/A.  
**Riferimenti codice:** docs/HCD_SPEC.md, sezione 6.

## ID: TEST-HCD-PARSER-001
**Titolo:** Parser HCD ostile e validazione bounds  
**Stato:** PLANNED  
**Impatto:** SAFETY  
**Descrizione:** Copertura di overflow, directory corrotta, sovrapposizioni, versioni sconosciute, record malformati e riferimenti firmware invalidi.  
**Analisi alternative:** test unitari deterministici; fuzzing con limiti statici; fault injection.  
**Decision:** tutte e tre le classi, in ordine incrementale.  
**Test aggiunti:** da implementare.  
**Metriche prima/dopo:** N/A.  
**Riferimenti codice:** docs/HCD_SPEC.md, sezione 15.

## ID: TEST-HCD-CRC-001
**Titolo:** CRC-64/ECMA-182 e checksum HCD  
**Stato:** PASS  
**Impatto:** SAFETY / CORRECTNESS  
**Descrizione:** Verifica test vector pubblico, input vuoto, azzeramento logico del campo checksum e rifiuto parametri invalidi.  
**Analisi alternative:** confronto con test vector standard; confronto con implementazione indipendente; fuzzing successivo.  
**Decision:** test vector standard e fixture HCD deterministica; fuzzing rimane pianificato.  
**Test aggiunti:** tests/unit/hcd_crc64_test.c.  
**Metriche prima/dopo:** HOST PASS; sorgente CRC cross-compilato per x86_64-elf e aarch64-none-elf.  
**Riferimenti codice:** preboot/common/hcd_crc64.c; preboot/common/hcd_crc64_table.inc; tests/unit/hcd_crc64_test.c.  
**Nota:** il primo tentativo di cross-compilazione ha rilevato l'assenza di assert.h nel target freestanding; corretto con HCD_ASSERT basato su __builtin_trap e ricompilato con successo.

## ID: TEST-RUNNER-001
**Titolo:** Runner host e cross iniziale  
**Stato:** PASS  
**Impatto:** REPRODUCIBILITY  
**Descrizione:** Verifica che i target Makefile eseguano i test CRC host e compilino i binding/test per AMD64 e ARM64.  
**Analisi alternative:** invocazione manuale; script; sistema build completo.  
**Decision:** Makefile con runner POSIX.  
**Test aggiunti:** make test-host; make test-cross.  
**Metriche prima/dopo:** host CRC PASS; x86_64-elf PASS; aarch64-none-elf PASS.  
**Riferimenti codice:** Makefile; tools/test-runner.sh.

## ID: TEST-HCD-PARSER-001
**Titolo:** Validazione strutturale e semantica iniziale HCD  
**Stato:** PASS — copertura parziale, non ancora parser VERIFIED  
**Impatto:** SAFETY / CORRECTNESS  
**Descrizione:** Verifica header, CRC, directory, bounds, duplicati, overlap, profilo recovery, CPU count, feature subset e memory map.  
**Analisi alternative:** test valid-only; mutazioni deterministiche; fuzzing.  
**Decision:** fixture deterministica e test di mutazione; fuzzing resta gate successivo.  
**Test aggiunti:** tests/unit/hcd_parser_test.c.  
**Metriche prima/dopo:** host PASS; cross compilation parser/semantics per x86_64-elf e aarch64-none-elf PASS.  
**Riferimenti codice:** preboot/common/hcd_parser.c; preboot/common/hcd_semantics.c; tests/unit/hcd_parser_test.c.  
**Copertura mancante:** topology, interrupt controllers, vectors, MMU, virtualization, firmware references, boot context, timers, platform devices e raw CPU registers.

## ID: TEST-HCD-CORE-001
**Titolo:** Profilo HCD Core e vincoli cross-section  
**Stato:** PASS  
**Impatto:** BOOT / MEMORY / INTERRUPTS / CPU  
**Descrizione:** Fixture core completa con CPU, topology, memory map, controller, vector, MMU, boot context e timer.  
**Analisi alternative:** test isolati per record; fixture end-to-end; fuzzing.  
**Decision:** fixture end-to-end più mutazioni deterministiche.  
**Test aggiunti:** tests/unit/hcd_core_profile_test.c.  
**Metriche prima/dopo:** core profile host PASS; cross compile AMD64/ARM64 PASS.  
**Riferimenti codice:** preboot/common/hcd_semantics.c; tests/unit/hcd_core_profile_test.c.  
**Copertura mutazioni:** limiti PA/VA, mask page/block granule, livelli supportati/attivi e logical CPU ID MMU; riserva HCD, parent topology, controller MMIO e parent cycle, interrupt timer non mappato, loader fuori memory map, indice raw CPU, geometria cache, subset virtualization, range firmware e platform MMIO.

## ID: TEST-HCD-INTERRUPTS-001
**Titolo:** Integrità del grafo controller interrupt  
**Stato:** PASS  
**Impatto:** INTERRUPTS / SAFETY  
**Descrizione:** Verifica intervalli non sovrapposti, MMIO valido, parent esistente e assenza di cicli.  
**Analisi alternative:** solo parent locale; ricerca lineare; binary search e Floyd bounded.  
**Decision:** test di mutazione self-cycle su fixture core.  
**Test aggiunti:** test_core_rejects_interrupt_parent_cycle.  
**Metriche prima/dopo:** host PASS; cross compile AMD64/ARM64 PASS.  
**Riferimenti codice:** hcd_semantics.c; hcd_core_profile_test.c.


## ID: TEST-HCD-OPTIONAL-001
**Titolo:** Validazione sezioni HCD opzionali e cross-reference  
**Stato:** PASS  
**Impatto:** CPU / CACHE / VIRTUALIZATION / FIRMWARE / DEVICES  
**Descrizione:** La fixture Core include anche CPU_RAW_REGISTERS, CACHE, VIRTUALIZATION, FIRMWARE_REFERENCES e PLATFORM_DEVICES.  
**Analisi alternative:** test isolati; fixture core con sezioni opzionali; fuzzing strutturato.  
**Decision:** fixture core estesa e mutazioni di indici, subset, geometria e range fisici.  
**Test aggiunti:** tests/unit/hcd_core_profile_test.c.  
**Metriche prima/dopo:** host PASS; cross compile AMD64/ARM64 PASS.  
**Riferimenti codice:** preboot/common/hcd_semantics.c; tests/unit/hcd_core_profile_test.c.

## ID: TEST-HCD-BUILDER-001
**Titolo:** Produzione HCD e round-trip parser  
**Stato:** PASS  
**Impatto:** BOOT / MEMORY / ZERO-COPY  
**Descrizione:** Builder scrive sezioni nel buffer caller-owned, finalizza directory e CRC e produce un descrittore accettato dal parser Recovery.  
**Analisi alternative:** fixture manuale; builder con copie; builder con puntatori diretti alle sezioni.  
**Decision:** builder con puntatori diretti e zeroing bounded.  
**Test aggiunti:** tests/unit/hcd_builder_test.c.  
**Metriche prima/dopo:** host PASS; cross compile AMD64/ARM64 PASS.  
**Riferimenti codice:** preboot/common/hcd_builder.c; preboot/common/hcd_section_catalog.c; tests/unit/hcd_builder_test.c.


## ID: TEST-KERNEL-HCD-GATE-001
**Titolo:** Kernel admission gate HCD  
**Stato:** PASS  
**Impatto:** BOOT / SECURITY / CORRECTNESS  
**Descrizione:** Il kernel accetta una fixture HCD Core valida, rifiuta checksum corrotto e profilo sconosciuto; la vista non viene pubblicata su errore.  
**Analisi alternative:** chiamata diretta parser; gate kernel; copia del buffer.  
**Decision:** gate dedicato con workspace preallocato e buffer input read-only/immutabile per contratto.  
**Test aggiunti:** kernel/tests/hcd_gate_test.c.  
**Metriche prima/dopo:** HOST PASS; x86_64-elf PASS; aarch64-none-elf PASS. Nessun WCET misurato.  
**Riferimenti codice:** kernel/core/hcd_gate.c; kernel/Makefile.

## ID: TEST-BOOT-BUNDLE-001
**Titolo:** Validazione NXB e decompressione bounded  
**Stato:** PLANNED  
**Impatto:** BOOT / SECURITY / MEMORY  
**Descrizione:** Header, manifest, signature, offsets, compression bounds, ELF e relocation.  
**Analisi alternative:** fuzzing, property-based mutation, fixtures firmate e malformed.  
**Decision:** test unitari e fuzz harness prima del boot QEMU.  
**Test aggiunti:** da implementare.  
**Metriche prima/dopo:** N/A.  
**Riferimenti codice:** docs/BOOT_IMAGE_SPEC.md.

## ID: TEST-REG-001
**Titolo:** REG concurrency, cancellation e capability  
**Stato:** PLANNED  
**Impatto:** IPC / SECURITY / PERFORMANCE  
**Descrizione:** Test ring overflow, backpressure, replay, cancellation/completion race, revoke e zero-copy lifetime.  
**Analisi alternative:** stress test, model checking e fault injection.  
**Decision:** harness deterministico più test concorrenti e model checking.  
**Test aggiunti:** da implementare.  
**Metriche prima/dopo:** N/A.  
**Riferimenti codice:** docs/REG_PROTOCOL.md.

## ID: TEST-SEC-DOMAIN-001
**Titolo:** Isolamento domini e multiutente  
**Stato:** PLANNED  
**Impatto:** SECURITY / ISOLATION  
**Descrizione:** Verificare MACHINE isolation, SYSTEM/ROOT separation, sdN ownership, SHARED grants e driver capabilities.  
**Analisi alternative:** unit tests, integration sandbox tests e fuzzing namespace.  
**Decision:** tutte le classi con fault injection e cross-tenant tests.  
**Test aggiunti:** da implementare.  
**Metriche prima/dopo:** N/A.  
**Riferimenti codice:** docs/SECURITY_DOMAINS.md; docs/VFS_USERLAND_SPEC.md.


## ID: TEST-FREESTANDING-LINK-001
**Titolo:** Link relocatable freestanding HCD preboot e kernel  
**Stato:** PASS  
**Impatto:** BOOT / ABI / RUNTIME INDEPENDENCE  
**Descrizione:** Compilazione freestanding con stack protector e builtins disabilitati, controllo dei simboli undefined e link relocatable dei componenti HCD per AMD64 e ARM64.  
**Analisi alternative:** compilazione oggetti isolati; link relocatable per target; link finale con runtime.  
**Decision:** link relocatable per verificare che il sottosistema non richieda libc o compiler runtime implicito.  
**Test aggiunti:** target test-cross nei Makefile preboot e kernel.  
**Metriche prima/dopo:** x86_64-elf PASS; aarch64-none-elf PASS; nessun simbolo undefined dopo link HCD.  
**Riferimenti codice:** preboot/Makefile; kernel/Makefile; kernel/core/hcd_gate.c.


## ID: TEST-PREBOOT-MEMORY-PLAN-001
**Titolo:** Extent planner fisico bounded  
**Stato:** PASS  
**Impatto:** MEMORY / BOOT / SECURITY  
**Descrizione:** Il planner usa soltanto HCD_MEMORY_USABLE, effettua first-fit/split, rispetta alignment, min/max, boundary, attributes e NUMA, non attiva fallback NUMA senza flag e rifiuta input/output alias prima di modificare i buffer.
**Analisi alternative:** allocazione semplice; extent split; allocator per-core; fallback NUMA implicito.  
**Decision:** extent split deterministico, fallback esplicito, no memory se i vincoli non sono soddisfatti.  
**Test aggiunti:** preboot/tests/unit/memory_plan_test.c; overlap del workspace con HCD input, overlap del risultato col workspace e overlap request/risultato.
**Metriche prima/dopo:** HOST PASS; x86_64-elf e aarch64-none-elf compile/link PASS; no libc/runtime imports. ASan/UBSan, clang-tidy e cppcheck PASS nell'orchestrazione `make test`.
**Riferimenti codice:** preboot/common/memory_plan.c; preboot/Makefile.

## ID: TEST-HCD-MEMORY-COUNT-001
**Titolo:** Rifiuto memory map oltre 4096 record  
**Stato:** PASS  
**Impatto:** HCD ABI / BOUNDED WORK  
**Descrizione:** Parser e builder rifiutano memory map con 4097 record prima di consumare un workspace oltre il limite.  
**Analisi alternative:** accettare fino al limite totale; coalescing; rifiuto esplicito.  
**Decision:** limite v1 4096, coalescing semantico solo nel producer, rifiuto in validator.  
**Test aggiunti:** hcd_core_profile_test.c e hcd_builder_test.c.  
**Metriche prima/dopo:** host PASS; cross compile/link AMD64/ARM64 PASS.  
**Riferimenti codice:** preboot/common/hcd_parser.c; hcd_builder.c.


## ID: TEST-UEFI-OWNERSHIP-001
**Titolo:** HCD Core richiede ExitBootServices in modalità UEFI  
**Stato:** PASS  
**Impatto:** BOOT / MEMORY OWNERSHIP  
**Descrizione:** HCD Core UEFI con Boot Services active è rifiutato; HCD Core con ExitBootServices done è accettato e può inizializzare il planner.  
**Analisi alternative:** verificare solo nel planner; verificare già al parser HCD; lasciare la decisione al kernel.  
**Decision:** enforce nel semantic validator e defense-in-depth nel planner.  
**Test aggiunti:** preboot/tests/unit/memory_plan_test.c.  
**Metriche prima/dopo:** host PASS; AMD64/ARM64 cross PASS; ASan/UBSan PASS.  
**Riferimenti codice:** preboot/common/hcd_semantics.c; memory_plan.c.

## ID: TEST-UNACCEPTED-MEMORY-001
**Titolo:** Memory planner non alloca HCD_MEMORY_UNACCEPTED  
**Stato:** PASS  
**Impatto:** MEMORY / PLATFORM SECURITY  
**Descrizione:** Una mappa valida composta da regioni non-usable non viene interpretata come RAM allocabile.  
**Analisi alternative:** fallback su reserved/unaccepted; accettazione implicita; rifiuto esplicito.  
**Decision:** solo MEMORY_USABLE è candidato.  
**Test aggiunti:** memory_plan_test.c.  
**Metriche prima/dopo:** host/cross/sanitizer PASS.  
**Riferimenti codice:** preboot/common/memory_plan.c; hcd_wire.h.

## ID: TEST-UEFI-MEMORY-MAP-NORMALIZER-001

**Titolo:** Conversione bounded della mappa UEFI in HCD MEMORY_MAP

**Stato:** PASS — host sintetico; nessun firmware/QEMU

**Impatto:** BOOT / MEMORY / SAFETY

**Descrizione:** Verifica ownership post-ExitBootServices, descriptor version/stride, mapping tipi e attributi, ordinamento, coalescing, mappe oltre il limite HCD riducibili tramite coalescing, overlap, overflow, tipo/attributo non supportato e buffer insufficiente.

**Analisi alternative:** fixture ordinata soltanto; convertire e ordinare in-place; allocare una seconda mappa.

**Decision:** test sintetici per percorsi ordinati e non ordinati, stride esteso e coalescing senza truncation.

**Test aggiunti:** preboot/tests/unit/uefi_memory_map_test.c.

**Metriche prima/dopo:** `PATH=/usr/local/opt/llvm/bin:$PATH make -C preboot test`: host PASS; freestanding cross-link x86_64-elf/aarch64-none-elf PASS; ASan/UBSan PASS; clang-tidy/cppcheck PASS. Nessun timing misurato.

**Riferimenti codice:** preboot/hal/uefi_memory_map.c; preboot/hal/uefi_exit.h.

## ID: TEST-MULTIBOOT2-MEMORY-MAP-001

**Titolo:** Validazione e normalizzazione del tag memory map Multiboot2

**Stato:** PASS — host sintetico; firmware/QEMU non eseguito

**Impatto:** BOOT / MEMORY / SAFETY

**Descrizione:** Copre estrazione bounded dal Multiboot2 information block completo, tag allineati da size non multipla di 8, terminatore assente/non terminale, tag type 6 assente/duplicato/malformato; mapping type 1-5, coalescing, stride esteso, tag/version/stride invalidi, reserved nonzero, lunghezze nulle, overflow dei range host-side e dei record fisici, tipo sconosciuto, input unsorted/overlap, output/record_count sovrapposti agli input o tra loro, output tag/tag-size sovrapposti e output capacity insufficiente.

**Analisi alternative:** fixture valida sola; fidarsi dei campi; ordinare input non valido.

**Decision:** il blocco deve terminare con l'end tag previsto e contenere un unico tag memory map; i record source devono essere validi, ordinati e non sovrapposti. Gli span input, output e output-count devono essere disgiunti; gli output del getter devono essere disgiunti anche tra loro. Gli errori di alias/range non modificano i buffer sovrapposti, e gli span host-side che superano `UINTPTR_MAX` sono rifiutati prima dell'accesso. Non si inventano attributi assenti e non si tronca l'output.

**Test aggiunti:** preboot/tests/unit/multiboot2_memory_map_test.c.

**Metriche prima/dopo:** `PATH=/usr/local/opt/llvm/bin:$PATH make -C preboot test`: host PASS; freestanding cross-link x86_64-elf e aarch64-none-elf PASS; ASan/UBSan PASS; clang-tidy/cppcheck PASS. Nessun boot firmware/QEMU eseguito.

**Riferimenti codice:** preboot/hal/multiboot2_info.c; preboot/hal/multiboot2_info.h; preboot/hal/multiboot2_memory_map.c; preboot/hal/multiboot2_memory_map.h.

## ID: TEST-MULTIBOOT2-EFI-MAP-TAG-001

**Titolo:** Estrazione bounded della EFI memory map post-EBS da Multiboot2

**Stato:** PASS — host/cross/sanitizer/static; nessun boot QEMU

**Impatto:** BOOT / MEMORY OWNERSHIP / FIRMWARE

**Descrizione:** Verifica il parsing del Multiboot2 information block, il tag EFI memory map descriptor v1, l'assenza del tag EFI Boot Services attivi, duplicati, end tag, version/stride e bounds. Il buffer estratto viene passato al normalizzatore UEFI esistente.

**Analisi alternative:** affidarsi solo alla sorgente GRUB; cercare il tag senza validare l'intero information block; copiare il payload prima della normalizzazione.

**Decision:** restituire un riferimento al payload originale dopo la validazione completa; rifiutare EFI_BS, mappe duplicate o malformate. La validazione host/cross non prova l'entry runtime.

**Test aggiunti:** preboot/tests/unit/multiboot2_efi_map_test.c.

**Metriche prima/dopo:** `PATH=/usr/local/opt/llvm/bin:$PATH make -C preboot test`: host, freestanding cross-link x86_64-elf/aarch64-none-elf, ASan/UBSan, clang-tidy e cppcheck PASS; firmware/QEMU non eseguito.

**Riferimenti codice:** preboot/hal/multiboot2_efi_map.c; preboot/hal/uefi_memory_map.c; GRUB 2.14 pinned Multiboot2 loader.

## ID: TEST-HCD-MMU-V2-001

**Titolo:** Compatibilità additiva e semantica ostile MMU HCD v2

**Stato:** PASS — host preboot/kernel, cross, ASan/UBSan, clang-tidy e cppcheck; nessun boot QEMU

**Impatto:** SAFETY / COMPATIBILITY / MMU

**Descrizione:** Valida separatamente i validator preboot e kernel per HCD v1/v2. Copre HCD v1 valido, sezioni v2 opzionali/required viste da v1, configurazioni AMD64 a quattro e cinque livelli, ARM64 stage 1, dimensioni mapping non valide, configurazione CURRENT assente/duplicata, bit TLB sconosciuti, mapping non ordinati e riferimenti a configurazioni inesistenti.

**Analisi alternative:** testare solo il producer; condividere il validator tra preboot e kernel; mantenere due implementazioni indipendenti e testare fixture equivalenti.

**Decision:** entrambe le implementazioni devono accettare gli stessi wire fixture validi e rifiutare i casi ostili nella sezione semantica corretta. Nessuna verifica host implica un boot firmware.

**Test aggiunti:** preboot/tests/unit/hcd_core_profile_test.c; kernel/tests/hcd_core_fixture.c; kernel/tests/hcd_gate_test.c.

**Metriche prima/dopo:** `PATH=/usr/local/opt/llvm/bin:$PATH make -C preboot test` e `PATH=/usr/local/opt/llvm/bin:$PATH make -C kernel test`: PASS. I test fixture HCD sono eseguiti con ASan/UBSan; i runtime freestanding sono cross-linkati per x86_64-elf e aarch64-none-elf; clang-tidy e cppcheck PASS.

**Riferimenti codice:** docs/HCD_SPEC.md §29; preboot/common/hcd_semantics.c; kernel/core/hcd/hcd_semantics.c.

## ID: TEST-HCD-ARM64-MMU-CONTEXT-001

**Titolo:** Contesti TTBR ARM64, selezione PC e vincoli HCD comuni

**Stato:** PASS — host preboot/kernel; cross/sanitizer/static e QEMU pending

**Impatto:** SAFETY / COMPATIBILITY / MMU

**Descrizione:** Verifica il producer da snapshot raw e i due validator HCD indipendenti. Copre i contesti TTBR0/TTBR1 simultanei con granularità lower/upper distinte, `CURRENT` derivato dal PC alto o basso, SCTLR.M/EPD, rifiuto del PC fuori dai range, PA non supportata, riferimenti stage1 incoerenti e configurazione non-CURRENT con `TRANSLATION_ENABLED`. La fixture positiva dimostra che il flag comune resta vincolato a CURRENT mentre l'estensione rappresenta l'altro contesto attivo.

**Analisi alternative:** rimuovere il controllo comune; assumere sempre TTBR0 come CURRENT; estendere il wire con record per-context e cross-reference espliciti, mantenendo controlli e validator comuni.

**Decision:** le due implementazioni indipendenti devono accettare gli stessi record validi e rifiutare i casi ostili nella sezione corretta. Nessun test sintetico equivale a un boot firmware.

**Test aggiunti:** preboot/tests/unit/isa_mmu_test.c; preboot/tests/unit/hcd_core_profile_test.c; kernel/tests/hcd_core_fixture.c.

**Metriche prima/dopo:** `make -C preboot test-host` e `make -C kernel test-host` PASS. Le verifiche cross-link AMD64/ARM64, ASan/UBSan, clang-tidy e cppcheck sono da eseguire; nessun QEMU boot.

**Riferimenti codice:** docs/HCD_SPEC.md §29.1 e §29.4; preboot/hal/arm64/isa.c; preboot/hal/mmu.c; validator preboot/kernel.
