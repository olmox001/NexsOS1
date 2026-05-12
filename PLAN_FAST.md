# OS1TEST (NEXS) Stabilization Plan - Phase 3 & 4

## Obiettivo
Unificare le architetture AMD64 e AArch64 tramite una HAL coerente, implementare il rilevamento automatico dell'hardware (RAM/CPU) e astrarre i driver VirtIO per supportare MMIO e PCI in modo trasparente.

## Riepilogo Progetto
- **Core**: Microkernel AArch64/AMD64.
- **Boot**: FDT (AArch64), Multiboot2 (AMD64).
- **HAL**: Primitives unificate in `arch.h`.
- **Driver**: VirtIO (Blk, GPU, Input).
- **FS**: Ext4 (RootFS), GPT.

## Piano a Fasi

### Fase 3: Hardware Discovery (Completamento)
- [x] **Parser FDT**: Implementato parser minimale per Device Tree (AArch64).
- [x] **Rilevamento RAM**: Migrazione da hardcoded a parsing dinamico delle regioni 'memory' nel DTB.
- [/] **Rilevamento CPU**: Utilizzo di `fdt_count_cpus()` per inizializzare i core secondari.
    - *Stato*: Rilevamento 4 core riuscito via fallback, ma il parsing FDT necessita di correzione del puntatore DTB.
- [ ] **Fix Puntatore DTB**: Risolvere il problema del puntatore nullo in `arch_platform_early_init`.
- [ ] **Test**: `make run ARCH=aarch64` per confermare RAM e CPU dinamiche.

### Fase 4: Astrazione HAL e Driver VirtIO
- [ ] **Unificazione Header HAL**: Finalizzare `kernel/include/kernel/arch.h` come unico punto di riferimento dinamico.
- [ ] **Unified VirtIO Handle**: Creare `struct virtio_device` comune con supporto per accesso MMIO e PCI.
- [ ] **Driver Refactoring**: 
    - Aggiornare `virtio_blk`, `virtio_gpu`, `virtio_input` per usare l'astrazione HAL.
    - Implementare scansione PCI per AMD64 e MMIO per AArch64.
- [ ] **Test**: `make run ARCH=aarch64` e `make run ARCH=amd64`.

### Fase 5: ISO Boot & Build System
- [ ] **Supporto ISO AArch64**: Integrare la creazione dell'ISO nel Makefile per AArch64.
- [ ] **Bootloader Update**: Assicurarsi che il caricamento ELF avvenga correttamente da ISO.
- [ ] **Test Finale**: Boot completo da ISO su entrambe le architetture.

---
**Note Tecniche**:
- Il caricamento avviene via ELF. Non cambieremo il formato a BIN.
- Log level momentaneamente impostato a `KERN_INFO` per debug discovery.
