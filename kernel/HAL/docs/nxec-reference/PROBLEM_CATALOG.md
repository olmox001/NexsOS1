# nxec — Problem Catalog

## ID: PROBLEM-HCD-001
**Titolo:** Protocollo Multiboot2 non definito per AArch64  
**Stato:** RESOLVED_BY_DESIGN  
**Impatto:** COMPATIBILITY  
**Descrizione:** Multiboot2 2.0 enumera i386 e MIPS32, non AArch64.  
**Analisi alternative:** inventare un architecture ID (vietato); protocollo custom; EFI PE/COFF AArch64 tramite GRUB arm64-efi.  
**Decision:** mantenere GRUB; AMD64 usa Multiboot2 nei percorsi supportati; ARM64 UEFI usa EFI chainloader.  
**Test aggiunti:** boot test separati, da implementare.  
**Metriche prima/dopo:** N/A.  
**Riferimenti codice:** docs/HCD_SPEC.md, sezione 3.

## ID: PROBLEM-HCD-002
**Titolo:** Confusione tra feature dichiarata e feature realmente utilizzabile  
**Stato:** DESIGNED  
**Impatto:** SAFETY / COMPATIBILITY  
**Descrizione:** CPUID/ID registers non dimostrano da soli che il sistema operativo abbia configurato tutti i prerequisiti.  
**Analisi alternative:** un bitmap; tre bitmap; catalogo di eventi di abilitazione.  
**Decision:** tre insiemi SUPPORTED/ENABLED/USABLE per CPU e riepilogo globale.  
**Test aggiunti:** subset invariants, CPU eterogenee e prerequisiti OS state.  
**Metriche prima/dopo:** N/A.  
**Riferimenti codice:** docs/HCD_SPEC.md, sezioni 9-10.

## ID: PROBLEM-TOOLCHAIN-001
**Titolo:** Toolchain Homebrew su host macOS Intel senza bottiglie aggiornate  
**Stato:** OPEN  
**Impatto:** BUILD / REPRODUCIBILITY  
**Descrizione:** Homebrew segnala che la configurazione Intel x86_64 non riceve più bottiglie aggiornate; i pacchetti richiesti possono dover essere compilati da sorgente. Il disco host mostra circa 30 GiB liberi durante il tentativo.  
**Analisi alternative:** Homebrew source build; toolchain LLVM precompilata compatibile; toolchain Linux isolata/QEMU; MacPorts o build riproducibile da sorgente.  
**Decision:** tentare aggiornamento/installazione dei pacchetti richiesti tramite Homebrew; monitorare spazio, dipendenze e risultati; non dichiarare toolchain pronta finché check e build target non passano.  
**Test aggiunti:** toolchain --check, version manifest, compile target AMD64/ARM64.  
**Metriche prima/dopo:** installazione in corso; tempi e spazio finali da registrare.  
**Riferimenti codice:** tools/setup-toolchain.sh; tools/setup-macos.sh.


## ID: PROBLEM-HCD-FUZZ-001
**Titolo:** Robustezza parser non ancora sottoposta a fuzzing  
**Stato:** OPEN  
**Impatto:** SECURITY / ROBUSTNESS  
**Descrizione:** Le fixture deterministiche coprono mutazioni note, ma non esplorano lo spazio completo di header, directory, record e combinazioni cross-section.  
**Analisi alternative:** random mutation non guidata; libFuzzer; AFL++; corpus strutturato con mutatori per sezione.  
**Decision:** aggiungere harness fuzzing host con corpus minimo e invarianti di no-crash/no-OOB/no-publication-on-error; scegliere il motore dopo verifica toolchain.  
**Test aggiunti:** test unitari recovery/core PASS; fuzzing non ancora eseguito.  
**Metriche prima/dopo:** nessun numero di input fuzzati.  
**Riferimenti codice:** hcd_parser.c; hcd_semantics.c; tests/unit/hcd_parser_test.c; tests/unit/hcd_core_profile_test.c.

## ID: PROBLEM-HCD-FW-CHECKSUM-001
**Titolo:** Checksum delle tabelle firmware esterne  
**Stato:** DEFERRED_TO_AUTHORIZED_CONSUMER  
**Impatto:** FIRMWARE / MEMORY SAFETY  
**Descrizione:** Il parser HCD non dereferenzia indirizzi fisici esterni; non può verificare il contenuto di ACPI/DTB/SMBIOS senza mappatura e autorizzazione.  
**Analisi alternative:** dereferenziare nel parser; copiare tabelle; validare dopo mapping autorizzato.  
**Decision:** il parser valida range, overflow, tipo e lifetime dichiarato; il sottosistema firmware verifica checksum e contenuto dopo mapping autorizzato e capability appropriata.  
**Test aggiunti:** range memory map e overflow; checksum contenuto esterno ancora da testare nel consumer.  
**Metriche prima/dopo:** N/A.  
**Riferimenti codice:** hcd_semantics.c; docs/HCD_SPEC.md, sezioni 22-25.

## ID: PROBLEM-HCD-WCET-001
**Titolo:** WCET del parser e delle primitive HCD non misurato  
**Stato:** OPEN  
**Impatto:** REALTIME / DETERMINISM  
**Descrizione:** I limiti di iterazioni sono statici e documentati, ma non sono una prova di WCET e non comprendono cache miss, contesa, firmware o variabilità hardware.  
**Analisi alternative:** misure empiriche; analisi statica; bound architetturale; combinazione di analisi e misure.  
**Decision:** mantenere separati WCET provato, bound analitico, massimo osservato, percentile e media; non sostituire uno con l'altro.  
**Test aggiunti:** bound algoritmici verificati; misure per CPU/firmware ancora da implementare.  
**Metriche prima/dopo:** nessuna misura temporale.  
**Riferimenti codice:** hcd_parser.c; hcd_semantics.c; docs/PERFORMANCE_LOG.md.

## ID: PROBLEM-HCD-QEMU-001
**Titolo:** Boot HCD AMD64/ARM64 non ancora verificato in QEMU  
**Stato:** OPEN  
**Impatto:** BOOT / COMPATIBILITY  
**Descrizione:** Wire format, parser e fixture sono verificati, ma non esistono ancora immagini di boot e test del passaggio reale pre-kernel → Core su QEMU.  
**Analisi alternative:** test host soltanto; QEMU AMD64; QEMU AMD64 e ARM64 con firmware appropriato.  
**Decision:** non dichiarare boot multiplatform verificato finché entrambi i target non completano boot e validazione HCD in QEMU.  
**Test aggiunti:** solo cross-compile; boot test da implementare.  
**Metriche prima/dopo:** N/A.  
**Riferimenti codice:** tools/setup-toolchain.sh; docs/HCD_SPEC.md.


## ID: PROBLEM-BOOT-IMAGE-001
**Titolo:** Bundle kernel precompilato, firma e decompressione non implementati  
**Stato:** OPEN / DESIGN  
**Impatto:** BOOT / SECURITY / MEMORY  
**Descrizione:** Serve un formato container versionato con manifest, firma, componenti, compressione, ELF e BHD.  
**Analisi alternative:** ELF diretto, cpio/tar, NXB custom; ET_EXEC vs ET_DYN; none/LZ4/Zstd.  
**Decision:** NXB è direzione architetturale, layout e profili restano da confrontare.  
**Test aggiunti:** catalogati in BOOT_IMAGE_SPEC.md.  
**Metriche prima/dopo:** nessuna.  
**Riferimenti codice:** docs/BOOT_IMAGE_SPEC.md.

## ID: PROBLEM-REG-001
**Titolo:** Protocollo REG asincrono e capability-safe non formalizzato a livello wire  
**Stato:** OPEN / DESIGN  
**Impatto:** IPC / SECURITY / PERFORMANCE  
**Descrizione:** Richieste push, completion, cancellation, code per-core e registry distribuito richiedono wire schema e memory model.  
**Analisi alternative:** RPC, 9P-like, rings condivisi e ibrido.  
**Decision:** protocollo ibrido bounded con authority capability separata.  
**Test aggiunti:** cancellation race, replay, overflow, fairness, revocation e zero-copy.  
**Metriche prima/dopo:** nessuna.  
**Riferimenti codice:** docs/REG_PROTOCOL.md.

## ID: PROBLEM-SEC-DOMAIN-001
**Titolo:** Mapping MACHINE/SYSTEM/ROOT/USER/SHARED su AMD64 e ARM64  
**Stato:** OPEN / DESIGN  
**Impatto:** SECURITY / PRIVILEGE  
**Descrizione:** I domini logici non coincidono con CPL, VMX root/non-root o ARM exception levels.  
**Analisi alternative:** mapping rigido ring-based; principal indipendenti; privilege mapping per-ISA.  
**Decision:** domini astratti e mapping ISA-specifico documentato; MACHINE non accessibile da userland.  
**Test aggiunti:** escalation, domain confusion, delegation e isolation.  
**Metriche prima/dopo:** nessuna.  
**Riferimenti codice:** docs/SECURITY_DOMAINS.md.

## ID: PROBLEM-VFS-IMAGE-001
**Titolo:** Formato immagine userland/root e mount registry da scegliere  
**Stato:** OPEN / STUDY_REQUIRED  
**Impatto:** STORAGE / SECURITY / BOOT  
**Descrizione:** cpio newc, tar/pax, SquashFS e formato NXFS hanno costi e licenze differenti.  
**Analisi alternative:** confronto parser footprint, metadata, random access, compression, integrity, tooling e license.  
**Decision:** NXUI come container concettuale, filesystem interno OPEN.  
**Test aggiunti:** traversal, sdN isolation, mount revoke, recovery e corrupt metadata.  
**Metriche prima/dopo:** nessuna.  
**Riferimenti codice:** docs/VFS_USERLAND_SPEC.md.

## ID: PROBLEM-GRUB-LICENSE-001
**Titolo:** GRUB GPL separato dalla policy permissiva del codice nxec  
**Stato:** DESIGNED / AUDIT_REQUIRED  
**Impatto:** LEGAL / DISTRIBUTION  
**Descrizione:** GRUB upstream è GPL-3.0-or-later, mentre la regola fondativa limita il codice incorporato a MIT/BSD/ISC/Apache-2.0.  
**Analisi alternative:** esclusione; incorporazione; progetto upstream isolato con licenza propria.  
**Decision:** repository upstream separato, nessun linking nel kernel e obblighi GPL rispettati.  
**Test aggiunti:** verifica revision/COPYING e audit release.  
**Metriche prima/dopo:** source clone verificato; build non verificata.  
**Riferimenti codice:** boot/grub/README.md.

## ID: PROBLEM-KERNEL-HCD-GATE-001
**Titolo:** Gate kernel iniziale per validazione HCD  
**Stato:** IMPLEMENTED_TESTED  
**Impatto:** BOOT / SECURITY  
**Descrizione:** Il kernel deve validare HCD prima di attivare feature e pubblicare viste.  
**Analisi alternative:** fidarsi del preboot; parser indipendente; riuso del parser condiviso con gate kernel.  
**Decision:** gate kernel dedicato, workspace preallocato, nessuna vista pubblicata in caso di errore. Il buffer deve essere immutabile durante la validazione.  
**Test aggiunti:** fixture Core valida, checksum corrotto e profilo sconosciuto.  
**Metriche prima/dopo:** host e cross compile AMD64/ARM64 PASS; timing non misurato.  
**Riferimenti codice:** kernel/core/hcd_gate.c; kernel/tests/hcd_gate_test.c.


## ID: PROBLEM-PREBOOT-MEMORY-001
**Titolo:** Qualifica dei limiti e politica allocator su firmware reali  
**Stato:** OPEN / HARDWARE_VALIDATION_REQUIRED  
**Impatto:** MEMORY / BOOT / NUMA / PERFORMANCE  
**Descrizione:** Limite 4096 memory records, 64 allocation requests, 4160 extents e first-fit sono limiti/policy iniziali; gli standard firmware non garantiscono un massimo di 4096.  
**Analisi alternative:** coalescing semantico, best-fit, top-down, NUMA pools, extent compaction e recovery.  
**Decision:** nessuna truncation; superamento non riducibile porta a recovery/rifiuto. Valori da qualificare su UEFI/ACPI/E820/DTB reali.  
**Test aggiunti:** host synthetic maps e boundary/NUMA/fragmentation; QEMU/hardware da fare.  
**Metriche prima/dopo:** nessuna misura timing.  
**Riferimenti codice:** docs/MEMORY_BOOTSTRAP_SPEC.md; preboot/common/memory_plan.c.


## ID: PROBLEM-UEFI-EXIT-BOOT-SERVICES-001
**Titolo:** Implementazione e qualificazione retry ExitBootServices  
**Stato:** OPEN / PREBOOT_UEFI_NOT_IMPLEMENTED  
**Impatto:** BOOT / MEMORY OWNERSHIP / FIRMWARE  
**Descrizione:** Il MapKey può diventare stale tra GetMemoryMap ed ExitBootServices; dopo il primo tentativo fallito l'uso dei Boot Services è limitato alle Memory Allocation Services.  
**Analisi alternative:** retry con buffer preallocato; map buffer con slack; recovery dopo tentativi finiti; reset controllato.  
**Decision:** acquisire in anticipo tutti i buffer, ripetere GetMemoryMap/ExitBootServices senza servizi non consentiti e definire un attempt budget finito da qualificare. Nessun numero massimo è ancora congelato.  
**Test aggiunti:** da implementare con mock EFI e QEMU OVMF/firmware reale.  
**Metriche prima/dopo:** non implementato.  
**Riferimenti codice:** docs/MEMORY_BOOTSTRAP_SPEC.md; UEFI Specification 2.11 §7.4.6.

## ID: PROBLEM-UEFI-ENTRY-OWNERSHIP-001

**Titolo:** Proprietario di ExitBootServices per i percorsi GRUB/UEFI

**Stato:** RESOLVED_BY_DESIGN / PINNED_GRUB_SOURCE_VERIFIED

**Impatto:** BOOT / MEMORY OWNERSHIP / FIRMWARE

**Descrizione:** Owner e fonte della memory map dipendono dal boot transport; la documentazione precedente non distingueva la consegna Multiboot2 AMD64 dal chainload EFI ARM64.

**Analisi alternative:** un adapter EFI comune; delegare EBS a GRUB su tutti i target; affidare EBS all'applicazione EFI soltanto quando il firmware la avvia direttamente.

**Decision:** AMD64 BIOS/Multiboot2 usa la mappa Multiboot2 BIOS/E820 senza EBS. AMD64 UEFI/Multiboot2 delega EBS a GRUB e consuma il tag EFI memory map post-EBS; nxec non richiede il tag che mantiene attivi i Boot Services. ARM64 UEFI viene chainloaded come applicazione EFI e il preboot nxec chiama EBS usando il retry helper prima di normalizzare la mappa. Le scelte sono supportate dall'ispezione del sorgente GRUB 2.14 pinned, non da una prova runtime firmware.

**Test aggiunti:** ispezione di `multiboot_mbi2.c`, `kern/efi/mm.c` e `loader/efi/chainloader.c`; test boot firmware/QEMU richiesti prima di dichiarare la validazione runtime.

**Metriche prima/dopo:** N/A.

**Riferimenti codice:** boot/grub/upstream/grub-core/loader/multiboot_mbi2.c; boot/grub/upstream/grub-core/kern/efi/mm.c; boot/grub/upstream/grub-core/loader/efi/chainloader.c; preboot/hal/uefi_exit.c; preboot/hal/uefi_memory_map.c.
