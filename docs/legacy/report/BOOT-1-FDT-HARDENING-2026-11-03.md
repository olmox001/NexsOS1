# BOOT-1 — DTB/FDT bounds, handoff e ownership memoria

**Data:** 2026-11-03
**Stato:** BOOT-1 IMPLEMENTATO E VERIFICATO su host + QEMU AMD64/AArch64; i gate di clean build, boot EFI AArch64, KTEST, shell, nxwins, soak e fault scan sono PASS. Fuzzing continuo e certificazione hardware restano fuori scope.
**Ambito:** kernel legacy attuale, prima di proseguire con le fasi NEXS-NG. Non rimuove né sostituisce il percorso di boot legacy.

## 1. Contratto di handoff verificato nel sorgente

Il percorso EFI AArch64 conosce già la dimensione FDT: `validate_fdt()` restituisce il campo `totalsize` in `DtbSpan`; `efi_main()` lo passa a `nexs_efi_handoff(KernelPA, DtbPA, KernelSpan, DtbSpan)`.

BOOT-1 conserva entrambi gli argomenti attraverso l'entry:
- `x0`: indirizzo fisico DTB;
- `x1`: span disponibile/validato ricevuto dal loader;
- il record AArch64 `struct boot_info` è ABI v1, 32 byte, con offset verificati da `_Static_assert`;
- dopo la validazione kernel-side, `boot_info.fdt_size` viene normalizzato al `totalsize` effettivo;
- le flag distinguono EFI con lunghezza esplicita, loader legacy con puntatore fidato e ricerca legacy.

I loader legacy che consegnano il puntatore in `x0` e lasciano `x1=0` restano supportati da `fdt_init_legacy()`. Il ramo con puntatore non nullo legge il minimo header, limita il totalsize e verifica la leggibilità delle pagine prima di invocare il parser bounded. Non si rimuove questo adapter.

AMD64 continua a usare Multiboot2/PVH e non usa FDT: le implementazioni FDT AMD64 restano stub espliciti.

## 2. Parser bounded

Il parser puro è separato in `kernel/lib/fdt_parser.c` e `kernel/include/kernel/fdt_parser.h`; l'adapter che legge il DTB fisico resta in `kernel/lib/fdt.c`.

Controlli implementati:
- magic, totalsize, versione, last-compatible-version e dimensione header;
- limite massimo del blob e controlli di overflow;
- allineamento e contenimento di structure, strings e reservation map nel totalsize;
- token, profondità, bilanciamento dei nodi, terminatore FDT_END e padding;
- nomi nodo e stringhe proprietà terminati entro il blocco corretto;
- name offset e payload di ogni proprietà entro i rispettivi bounds;
- tuple `reg` con celle supportate e lunghezze divisibili;
- nodi `/memory`, stato `status`, `#address-cells`/ `#size-cells`;
- `/reserved-memory`: celle esplicite e coerenti con root, `ranges` obbligatorio e vuoto, `reg` statico; allocazioni dinamiche `size`/ `alignment` non supportate e rifiutate, non ignorate;
- reservation map terminata da coppia (0,0), riserve statiche e merge delle regioni dello stesso tipo;
- conteggio CPU basato sullo stesso walker validato, senza duplicare il parser.

Il parser generico ammette `#address-cells=3` nei nodi come PCIe (presente nel DTB reale QEMU); la decodifica delle tuple RAM resta limitata ai formati effettivamente supportati. Le proprietà dopo i subnodi, i duplicati dei campi strutturali e gli stati non abilitati non vengono promossi a RAM.

## 3. Ricerca legacy page-by-page

La ricerca resta un fallback, non sostituisce il puntatore fornito dai loader. Il range legacy già presente nel progetto viene preservato: `[ARCH_RAM_START, ARCH_RAM_START + 1 GiB)`, che su QEMU virt AArch64 è `[0x40000000, 0x80000000)`.

La scansione:
1. visita una pagina da 4 KiB alla volta;
2. verifica la leggibilità della pagina tramite `arch_platform_probe_read32()`;
3. cerca solo candidati allineati a 8 byte all'interno della pagina;
4. valida il candidato con il parser bounded e verifica il blob completo prima di accettarlo.

Il probe AArch64 usa il PC esatto dell'istruzione LDR come fixup nella gestione Data Abort; non converte ogni Data Abort generico in una lettura riuscita. Il range di ricerca non è stato esteso arbitrariamente a 128 GiB.

## 4. Mappa memoria e PMM

Il parser produce:
- regioni RAM abilitate;
- reservation-map entries;
- `/reserved-memory` statico;
- intervallo del DTB stesso.

La piattaforma verifica che il DTB sia contenuto in una regione RAM dichiarata; quando il nodo RAM manca, il fallback QEMU-virt verifica che il DTB ricada nell'intervallo RAM effettivamente sondato. I descrittori RESERVED vengono mantenuti anche quando il parser non trova un nodo RAM.

Il PMM:
- conta l'unione delle pagine complete delle regioni USABLE, senza sommare due volte regioni sovrapposte;
- evita gli overlay RESERVED durante il posizionamento dei metadati;
- riserva kernel, metadata, firmware, DTB, boot modules e ogni buco non coperto dall'unione USABLE;
- limita le regioni allocabili alla finestra fisica mappata dall'architettura e fallisce chiuso su intervalli RAM fuori finestra.

Il VMM costruisce il direct map soltanto dalle regioni USABLE, lasciando fuori buchi e intervalli RESERVED, e verifica la stessa finestra fisica prima di mappare.

Limiti correnti:
- AArch64: `ARCH_PHYS_MAP_LIMIT = 0x2000000000` (128 GiB, esclusivo);
- AMD64: `ARCH_PHYS_MAP_LIMIT = 256 GiB` (esclusivo).

Questi limiti descrivono la finestra supportata dal direct map, non la quantità di RAM installata.

## 5. Test e collocazione

Il test host risiede in `kernel/test/fdt_parser_test.c`; non viene introdotta una directory test parallela alla root. Il target `make test-fdt` genera esplicitamente `build/aarch64/virt.dtb`, compila il parser host con ASan+UBSan e richiede il DTB reale: l'assenza del DTB è un errore, non uno skip.

**Ultimo risultato host osservato:**
- `make test-fdt`: exit code 0;
- fixture sintetici validi/malformati, v16/v17, celle, capacity, reservation map, reserved-memory statico/dinamico, parent disabled, proprietà dopo i subnodi e `no-map`;
- DTB QEMU: 1 MiB, 2 regioni risultanti (RAM + DTB RESERVED), 4 CPU;
- **61 controlli PASS** con AddressSanitizer e UndefinedBehaviorSanitizer.

Sono inoltre stati aggiunti KTEST runtime:
- unione di regioni PMM non ordinate/sovrapposte (entrambe le architetture);
- verifica di ogni PFN del DTB come RESERVED e campionamento allocatore che non deve restituire PFN DTB (AArch64).

**Verifica runtime completata il 2026-11-03 (stato attuale del sorgente):**
- clean build da build tree pulito: `make clean`, `make ARCH=amd64 all`, quindi `make ARCH=aarch64 all`; entrambe le build complete sono terminate senza errori;
- `make test-all`: exit code 0; il target ha eseguito build/ISO/check, `make test-fdt` e i test `nxadb` su entrambe le architetture;
- host parser: 61 controlli PASS con ASan/UBSan; DTB QEMU reale da 1 MiB, 2 regioni risultanti, 4 CPU;
- AMD64: GRUB BIOS ISO costruita e avviata; NXShell pronta, `nxwins` focus window 104 PASS, soak 30 s, screenshot finale 1280×800 verificato, 10 KTEST PASS, 0 FAIL, fault scan 0;
- AArch64: GRUB arm64-efi e ISO UEFI costruiti; boot EFI completato con `BootInfo source=1`, x0=DTB e x1=1 MiB; SMP 4/4; NXShell pronta; `test_fdt_page_by_page_search` ha trovato il DTB QEMU reale a `0x40000000` dopo aver rimosso il fixture sintetico dalla finestra di ricerca; 11 KTEST PASS, 0 FAIL; `nxwins` focus window 104 PASS, soak 30 s, screenshot finale 1280×800 verificato, fault scan 0;
- QEMU dei test fermati con `nxadb stop`; nessun processo QEMU di test resta attivo.

Log completi persistenti:
- `docs/report/logs/BOOT-1-clean-aarch64-2026-11-03.log`
- `docs/report/logs/BOOT-1-host-fdt-2026-11-03.log`
- `docs/report/logs/BOOT-1-test-all-2026-11-03.log`

La prima clean build AMD64 era stata eseguita correttamente ma il suo tee era stato collocato in `build/runs`, che `make clean` elimina; per non perdere i log successivi, tutti i nuovi log sono stati spostati in `docs/report/logs`, fuori dall'albero generato. Il test-all persistente conserva il rebuild/boot/test dual-arch completo.

## 6. Gate prima di procedere ad altre fasi

<badge label="BOOT-1 runtime gate: PASS" color="green" />

1. Revisione del diff e riferimenti documentali: completata per il perimetro BOOT-1; i due gitlink submodule estranei (`user/bin/coreutils`, `user/bin/doom`) restano esclusi dallo stage.
2. Clean build AMD64 e AArch64, in sequenza: PASS.
3. Parser host ASan/UBSan con DTB QEMU obbligatorio: 61/61 PASS.
4. Boot QEMU AMD64 via GRUB BIOS e AArch64 via GRUB arm64-efi/UEFI: PASS.
5. KTEST: AMD64 10/10, AArch64 11/11; shell/NXShell, nxwins focus, soak 30 s, screenshot 1280×800 e fault scan 0: PASS.
6. Handoff EFI AArch64 x0/x1 e percorso legacy x1=0/page-by-page: PASS; log AArch64 mostra `BootInfo source=1`, mentre KTEST esercita `fdt_init_legacy(0)` e ritrova il DTB QEMU reale.
7. Report, PENDING-WORK e log persistenti aggiornati. BOOT-1 può essere chiuso; le attività successive DEV-1/DEV-2 restano fasi separate e non sono incluse in questa verifica.

## 7. Fuori scope / limiti espliciti

- Non è implementato un BootInfo comune AMD64+AArch64 con EFI memory map, ACPI, framebuffer e moduli: questo record è l'adapter AArch64 DTB v1.
- La discovery generica di device/interrupti/compatible e il grafo devmgr non sono implementati da BOOT-1.
- Le riserve dinamiche `/reserved-memory` che usano `size`/allocazione firmware sono rifiutate; richiedono un incremento separato e testato.
- Il parser host non è ancora sottoposto a fuzzing continuo/libFuzzer; 61 controlli deterministici non equivalgono a fuzz coverage.
- Nessuna certificazione su hardware fisico è implicata dai test QEMU.
