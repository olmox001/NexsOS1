# nxec — Performance Log

Ogni numero deve indicare il metodo e la classe di evidenza. MAX_OBSERVED non è WCET.

## ID: PERF-HCD-LAYOUT-001
**Titolo:** Cross-compilazione layout HCD v1  
**Stato:** PASS — solo correttezza di compilazione  
**Impatto:** BUILD / COMPATIBILITY  
**Descrizione:** Compilazione freestanding del test layout con C17 e warning-as-error.  
**Analisi alternative:** host build; target AMD64; target AMD64 e ARM64.  
**Decision:** entrambi i target cross.  
**Test aggiunti:** TEST-HCD-LAYOUT-001.  
**Metriche prima/dopo:** x86_64-elf PASS; aarch64-none-elf PASS. Nessun ciclo CPU o timing è stato misurato; nessun dato WCET dichiarato.  
**Riferimenti codice:** preboot/common/hcd_wire.h; tests/unit/hcd_layout_test.c.

## ID: PERF-TOOLCHAIN-001
**Titolo:** Aggiornamento toolchain host Intel macOS  
**Stato:** RUNNING  
**Impatto:** BUILD / REPRODUCIBILITY  
**Descrizione:** Homebrew sta compilando dipendenze da sorgente per aggiornare LLVM/QEMU/mtools e successivamente installare GDB.  
**Analisi alternative:** build sorgente Homebrew; toolchain alternativa verificata; ambiente Linux isolato.  
**Decision:** completare il percorso Homebrew e verificare le versioni; registrare eventuali blocchi reali.  
**Test aggiunti:** tools/setup-toolchain.sh --check e manifest.  
**Metriche prima/dopo:** processo avviato; durata e spazio finale non ancora disponibili.  
**Riferimenti codice:** tools/setup-macos.sh; tools/setup-toolchain.sh.

## ID: PERF-HCD-CRC-001
**Titolo:** Correttezza CRC-64/ECMA-182  
**Stato:** PASS — performance non misurata  
**Impatto:** CORRECTNESS / PERFORMANCE  
**Descrizione:** Implementazione tabellare freestanding con tabella statica di 256 elementi U64.  
**Analisi alternative:** bitwise, tabella lookup, istruzioni hardware specifiche ISA.  
**Decision:** tabella statica per baseline; benchmark comparativo ancora richiesto.  
**Test aggiunti:** TEST-HCD-CRC-001.  
**Metriche prima/dopo:** test vector 123456789 = 0x6C40DF5F0B497347; fixture HCD = 0xC29F5C4AC075B014; HOST PASS; AMD64/ARM64 object compile PASS. Nessun ciclo o timing misurato.  
**Riferimenti codice:** preboot/common/hcd_crc64.c; preboot/common/hcd_crc64_table.inc.

## ID: PERF-HCD-PARSER-001
**Titolo:** Validazione HCD iniziale  
**Stato:** CORRECTNESS PASS — timing non misurato  
**Impatto:** SAFETY / PERFORMANCE  
**Descrizione:** Parser senza allocazione dinamica, con workspace caller-owned e cicli limitati dai massimali HCD.  
**Analisi alternative:** workspace globale; workspace caller-owned; allocazione temporanea.  
**Decision:** workspace preallocato caller-owned.  
**Test aggiunti:** TEST-HCD-PARSER-001.  
**Metriche prima/dopo:** host test PASS; cross compile AMD64/ARM64 PASS. Nessun ciclo CPU, WCET o percentile misurato.  
**Riferimenti codice:** preboot/common/hcd_parser.c; preboot/common/hcd_semantics.c.

## ID: PERF-HCD-BOUNDS-001
**Titolo:** Lookup e scansioni HCD bounded  
**Stato:** STATIC BOUNDS DOCUMENTED — WCET NOT MEASURED  
**Impatto:** DETERMINISM / LATENCY  
**Descrizione:** Ricerca binaria su memory map e intervalli interrupt; limiti espliciti per sezioni e CPU.  
**Analisi alternative:** scansione lineare; hash table; binary search su record ordinati.  
**Decision:** binary search con 64 iterazioni massime per memory map e 32 per interrupt ID; directory overlap 64×64.  
**Test aggiunti:** TEST-HCD-PARSER-001; TEST-HCD-CORE-001.  
**Metriche prima/dopo:** limiti algoritmici fissati; nessun ciclo CPU, WCET o percentile misurato.  
**Riferimenti codice:** preboot/common/hcd_semantics.c; preboot/common/hcd_parser.c.

## ID: PERF-HCD-INTERRUPTS-001
**Titolo:** Grafo controller e lookup interrupt bounded  
**Stato:** STATIC BOUNDS DOCUMENTED — WCET NOT MEASURED  
**Impatto:** INTERRUPTS / LATENCY  
**Descrizione:** Controller limitati a 256, parent cycle detection Floyd, interrupt lookup binary.  
**Analisi alternative:** DFS ricorsiva; allocazione dinamica; Floyd con limite fisso.  
**Decision:** Floyd massimo 256 passi per controller; lookup interrupt massimo 32 iterazioni.  
**Test aggiunti:** TEST-HCD-INTERRUPTS-001.  
**Metriche prima/dopo:** limiti algoritmici verificati; nessun WCET o ciclo CPU misurato.  
**Riferimenti codice:** preboot/common/hcd_semantics.c.

## ID: PERF-HCD-BUILDER-001
**Titolo:** Produzione HCD con scrittura diretta  
**Stato:** COMPLEXITY BOUNDED — TIMING NOT MEASURED  
**Impatto:** MEMORY / BOOT LATENCY  
**Descrizione:** Il builder azzera solo la directory massima e le sezioni effettivamente riservate; non usa copie temporanee.  
**Analisi alternative:** staging per sezione; copia finale; scrittura diretta nel buffer finale.  
**Decision:** scrittura diretta e finalizzazione singola.  
**Test aggiunti:** TEST-HCD-BUILDER-001.  
**Metriche prima/dopo:** zero allocazioni; numero byte azzerati = directory massima + payload riservato; nessun WCET misurato.  
**Riferimenti codice:** preboot/common/hcd_builder.c.


## ID: PERF-KERNEL-HCD-GATE-001
**Titolo:** Kernel HCD admission gate  
**Stato:** CORRECTNESS PASS — TIMING NOT MEASURED  
**Impatto:** BOOT / SECURITY / MEMORY  
**Descrizione:** Il gate riusa il parser bounded e mantiene workspace caller-owned.  
**Analisi alternative:** parser diretto; wrapper gate; copia completa del descrittore.  
**Decision:** gate dedicato, zero allocazioni, view pubblicata solo dopo HCD_OK.  
**Test aggiunti:** TEST-KERNEL-HCD-GATE-001.  
**Metriche prima/dopo:** host PASS, cross compile AMD64/ARM64 PASS; nessun ciclo CPU, latency, MAX_OBSERVED o WCET misurato.  
**Riferimenti codice:** kernel/core/hcd_gate.c; kernel/Makefile.

## ID: PERF-GRUB-SOURCE-001
**Titolo:** Acquisizione sorgente GRUB 2.14  
**Stato:** SOURCE CHECKOUT PASS — BUILD NOT RUN  
**Impatto:** BOOT / BUILD  
**Descrizione:** Checkout indipendente tag grub-2.14.  
**Analisi alternative:** package prebuilt; build source separata; bootloader alternativo.  
**Decision:** source project isolato, build multi-target da completare.  
**Test aggiunti:** tag e commit verificati.  
**Metriche prima/dopo:** commit d38d6a1a9b79427848976f53d474392cd29c2a71; build non misurata.  
**Riferimenti codice:** boot/grub/UPSTREAM_REVISION; boot/grub/build.sh.


## ID: PERF-FREESTANDING-LINK-001
**Titolo:** Assenza dipendenze implicite libc/runtime nel gate HCD  
**Stato:** STATIC LINK CHECK PASS  
**Impatto:** BOOT / DETERMINISM / PORTABILITY  
**Descrizione:** I primi oggetti freestanding generavano riferimenti impliciti a memset e stack protector. Sono stati rimossi inizializzazioni/assegnamenti aggregate nei componenti runtime e i flag freestanding ora disabilitano stack protector e builtins.  
**Analisi alternative:** implementare libc kernel; introdurre runtime memory primitives; rimuovere le dipendenze implicite.  
**Decision:** nessuna dipendenza libc/runtime nel sottosistema HCD; inizializzazione e copie sono esplicite e bounded.  
**Test aggiunti:** TEST-FREESTANDING-LINK-001.  
**Metriche prima/dopo:** prima: undefined memcpy/memset/stack_chk; dopo: link relocatable AMD64/ARM64 senza undefined. Nessuna misura temporale.  
**Riferimenti codice:** preboot/common/hcd_parser.c; preboot/common/hcd_semantics.c; preboot/common/hcd_builder.c; kernel/core/hcd_gate.c.


## ID: PERF-PREBOOT-MEMORY-PLAN-001
**Titolo:** Workspace e bound algoritmici extent planner  
**Stato:** STRUCTURAL BOUNDS VERIFIED — TIMING NOT MEASURED  
**Impatto:** MEMORY / BOOT / PERFORMANCE  
**Descrizione:** 4096 HCD memory records, 64 allocation requests e 4160 extent slots; ogni allocazione scansiona al massimo il numero corrente di extent e split/shift è bounded dalla stessa capacità.  
**Analisi alternative:** bitmap proporzionale alla RAM; buddy; extent list; firmware allocator.  
**Decision:** extent workspace <=144 KiB, caller-owned in memoria bootstrap; no heap; first-fit.  
**Test aggiunti:** TEST-PREBOOT-MEMORY-PLAN-001.  
**Metriche prima/dopo:** limite workspace compile-time PASS; host/cross PASS; cicli e latenza MAX_OBSERVED/WCET non misurati.  
**Riferimenti codice:** preboot/common/memory_plan.h; memory_plan.c.


## ID: PERF-SANITIZER-ANALYZER-001
**Titolo:** Sanitizer dinamici e Clang Static Analyzer per HCD/Memory Planner/Gate  
**Stato:** PASS  
**Impatto:** MEMORY SAFETY / UNDEFINED BEHAVIOR  
**Descrizione:** ASan+UBSan eseguiti sui test HCD builder, physical memory planner e kernel admission gate; Clang Static Analyzer eseguito sui sorgenti production HCD, memory planner e gate.  
**Analisi alternative:** solo warning compiler; static analyzer; runtime sanitizer; fuzzing.  
**Decision:** sanitizer e analyzer diventano target ripetibili dei singoli progetti; fuzzing strutturato resta da aggiungere.  
**Test aggiunti:** target test-sanitize in preboot e kernel.  
**Metriche prima/dopo:** ASan/UBSan PASS; Clang Static Analyzer PASS; nessun benchmark temporale.  
**Riferimenti codice:** preboot/Makefile; kernel/Makefile; tools/test-runner.sh.


## ID: PERF-GRUB-MULTITARGET-001
**Titolo:** Build completa GRUB per BIOS, AMD64 EFI e ARM64 EFI
**Stato:** PASS — tutti i target completi
**Impatto:** BOOT / BUILD / PORTABILITY
**Descrizione:** `make grub` ha applicato l'intera patch series al worktree detached del commit GRUB 2.14 pinned e completato build e install per i target i386-PC, x86_64-EFI e arm64-EFI. Nessun target o modulo è stato disabilitato. Il log completo non rediretto contiene 16.388 righe (10.035.949 byte); la ripartizione concorrente è JOBS=16, cinque job per target.
**Analisi alternative:** ridurre i target/moduli; mantenere build separate concorrenti; aggiornare i flag per eludere le relocation prodotte dal compilatore; aggiungere supporto corretto alle relocation nella patch downstream.
**Decision:** conservare tutti i target e correggere downstream il supporto ARM64 alle relocation LDST low-12 a tutte le larghezze, in verifier, image builder e loader. La sorgente pinned è rimasta pulita.
**Test aggiunti:** applicazione della patch series verificata con `git apply --check`; build/install completa di tutti e tre i target; `make toolchain`; `make test`.
**Metriche prima/dopo:** prima, ARM64 EFI falliva su relocation LDST non supportate (`0x11d`, poi `0x116`); dopo, `make grub` PASS per i386-PC, x86_64-EFI e arm64-EFI. Nessun benchmark comparativo o tempo totale di build registrato.
**Riferimenti codice:** patch/grub/series; patch/grub/0002-arm64-efi-support-aarch64-ldst-relocations.patch; boot/grub/build.sh.


## ID: PERF-UEFI-MEMORY-MAP-001
**Titolo:** Normalizzatore UEFI memory map in-place
**Stato:** STRUCTURAL BOUNDS AND CORRECTNESS PASS — TIMING NOT MEASURED
**Impatto:** BOOT / MEMORY / PERFORMANCE
**Descrizione:** Conversione descriptor v1 in HCD memory records nel buffer caller-owned. Il percorso ordinato usa una scansione lineare; mappe non ordinate usano heapsort O(n log n), poi validazione overlap e coalescing lineari.
**Analisi alternative:** richiedere input ordinato; sort quadratic; heapsort in-place senza workspace aggiuntivo.
**Decision:** non assumere l'ordinamento UEFI e non introdurre heap/workspace; limite input 64 MiB, descriptor minimi 40 byte, output massimo 4096 record dopo coalescing. Una mappa che supera il limite finale è rifiutata, non troncata.
**Test aggiunti:** TEST-UEFI-MEMORY-MAP-NORMALIZER-001; incluso caso con 4097 descriptor contigui coalesced in un record.
**Metriche prima/dopo:** host/cross-link/sanitizer/static analysis PASS; workspace aggiuntivo 0 byte; MAX_OBSERVED e WCET non misurati.
**Riferimenti codice:** preboot/hal/uefi_memory_map.c; preboot/tests/unit/uefi_memory_map_test.c.
