---
description: "Analizza stato progetto OS e crea piano dettagliato per refactor, modulizzazione, driver PCI, e astrazione HAL in fasi verificate con 'make run'"
name: "Analizza & Pianifica OS Refactor"
agent: "agent"
---

# Analisi Completa OS1 + Piano Refactor Fase per Fase

**Obiettivo**: Scansionare lo stato attuale del progetto OS (codebase, build system, componenti architettura), identificare problemi strutturali, testate entrambe le architetture (aarch64/amd64), e creare un piano dettagliato suddiviso in **micro-fasi verificabili**.

---

## 1. ANALISI STATO CORRENTE

Esegui una scansione completa **senza supposizioni**. Leggi e analizza:

### A. Struttura Build System
- [ ] [Makefile](./Makefile) — configurazione per aarch64/amd64, target di build
- [ ] [boot/aarch64/linker.ld](./boot/aarch64/linker.ld), [boot/amd64/linker.ld](./boot/amd64/linker.ld) — memory layout
- [ ] [kernel/arch/aarch64/kernel.ld](./kernel/arch/aarch64/kernel.ld), [kernel/arch/amd64/kernel.ld](./kernel/arch/amd64/kernel.ld) — kernel layout

**Documen**: Tabella layout memoria per architettura, entry point, offset bootstap

### B. Boot Chain
- [ ] [boot/aarch64/stage1.S](./boot/aarch64/stage1.S), [boot/aarch64/stage2.S](./boot/aarch64/stage2.S)
- [ ] [boot/amd64/stage1.S](./boot/amd64/stage1.S), [boot/amd64/stage2.S](./boot/amd64/stage2.S)
- [ ] [kernel/main.c](./kernel/main.c) — entry point C

**Documenti**: Differenze stage1/2 tra architetture, cosa manca di astrazione

### C. HAL Attuale (Architecture Abstraction)
- [ ] [kernel/include/kernel/arch.h](./kernel/include/kernel/arch.h) — interfacce HAL
- [ ] [kernel/arch/aarch64/](./kernel/arch/aarch64/) e [kernel/arch/amd64/](./kernel/arch/amd64/) — implementazioni arch-specific
- [ ] **Identificare**: Funzioni assembly NON astratte, code duplication, componenti non modularizzate

**Documenti**: Audit HAL (cosa è astratto, cosa no), liste di funzioni assembly giganti, code duplication %

### D. Drivers & Device Support
- [ ] [kernel/drivers/](./kernel/drivers/) — virtio, uart, console, pci, gpu, timer, etc.
- [ ] **Per ogni driver**: architettura supportata (aarch64 only? amd64?), dipendenze PCI vs MMIO, portabilità
- [ ] [scratch/base-nexs-main/](./scratch/base-nexs-main/) — analizzare implementazione HAL superior (registry, syscall dispatch)

**Documenti**: Matrice driver × architettura, identificare quelli che mancano PCI

### E. Process Management & Syscalls
- [ ] [kernel/sched/process.c](./kernel/sched/process.c) — scheduler, context switch
- [ ] [kernel/core/syscall_dispatch.c](./kernel/core/syscall_dispatch.c) — syscall routing
- [ ] [include/api/os1.h](./include/api/os1.h) — ABI POSIX/System V
- [ ] Bug attivo: ELR=0 panic (vedi [STATUS.md](./STATUS.md#bug-attivo-pre-esistente))

**Documenti**: Elenco syscall implementati, architettura dispatcher, bug list

### F. Filesystem & Memory
- [ ] [kernel/fs/](./kernel/fs/) — ext4, gpt support
- [ ] [kernel/mm/](./kernel/mm/) — pmm.c, vmm.c, paging
- [ ] Device tree support (FDT)

**Documenti**: FS capabilities, PMM/VMM status, device tree integration status

---

## 2. TESTING BASELINE

Testa entrambe le architetture per stabilire baseline:

```bash
# AArch64
make clean
make run ARCH=aarch64

# Cattura output: boot success, crash logs, performance baseline
# Durata stabile? Errori? Quale è lo stato?
```

```bash
# AMD64
make clean
make run ARCH=amd64

# Stessa analisi
```

**Documenti**: Tabella test results (stability, errors, what works/fails per arch)

---

## 3. PROBLEMI STRUTTURALI IDENTIFICATI

Sintetizza i problemi trovati in categorie:

1. **Codice Assembly Gigante** — file asm enormi che dovrebbero essere in C
2. **HAL Incompleto** — funzioni arch-specific non astratte
3. **Duplication** — codice duplicato aarch64 vs amd64
4. **Driver Non Portabili** — hardcoded MMIO/PCI, non funzionano su entrambe le arch
5. **Build System Fragile** — mkdisk.c, ISO generation, bootloader selection
6. **Registry/Syscall Dispatch** — meno maturo vs scratch/base-nexs-main
7. **Modulizzazione** — driver hardcoded, non modularizzabili via device tree + ELF load

---

## 4. PIANO DETTAGLIATO (Fasi Micro-Verificabili)

Per ogni fase:
- **Obiettivo**: cosa fattibile entro X commit
- **Input**: file da modificare
- **Output**: test verificabile (`make run` success/fail)
- **Dipendenze**: da quali fasi precedenti dipende
- **Effort**: stima complessità

### FASE 1: Code Cleanup & Assembly Audit
**Obiettivo**: Identificare asm file candidati per migrazione a C, riduplicare codice boot

- **1.1** Audit stage1/2 assembly — quali funzioni sono presenti in entrambe le arch, quali no
- **1.2** Misurare % code duplication boot stage
- **1.3** Selezionare 1 funzione asm semplice (es. spinlock primitives) da migrare a C con HAL inline
- **1.4** Test: `make run ARCH=aarch64 && make run ARCH=amd64` — deve boot comunque
- **Effort**: 2-3 hours | **Risk**: medio (asm è delicato)

### FASE 2: HAL Unificazione Layer 1
**Obiettivo**: Estrarre interfacce HAL mancanti, documentare astrazione target

- **2.1** Creare [kernel/include/kernel/hal_unified.h](./kernel/include/kernel/hal_unified.h) con typedef per: memory access, CPU control, IRQ management, cache
- **2.2** Implementare versioni aarch64 + amd64 dietro interfaccia
- **2.3** Migrare prime 3 funzioni asm semplici (es. `halt()`, `cli()`, `sti()`)
- **2.4** Test: `make run ARCH=aarch64 && make run ARCH=amd64`
- **Effort**: 3-4 hours | **Risk**: medio-alto (scheduler-sensitive)

### FASE 3: Driver PCI/MMIO Abstraction
**Obiettivo**: Unificare accesso device register via HAL, aggiungere PCI su amd64

- **3.1** Creare [kernel/include/kernel/hal_device.h](./kernel/include/kernel/hal_device.h) con device discovery trait
- **3.2** Implementare funzioni: `platform_enumerate_devices()`, `device_read_mmio()`, `device_write_mmio()`
- **3.3** Per aarch64: MMIO via device tree
- **3.4** Per amd64: PCI enumeration via BIOS
- **3.5** Migrare 1 driver semplice (es. console/UART) dietro HAL_DEVICE
- **3.6** Test: `make run ARCH=aarch64 && make run ARCH=amd64`
- **Effort**: 4-5 hours | **Risk**: alto (PCI è complesso)

### FASE 4: Device Tree Modularizzazione
**Obiettivo**: Caricare driver via ELF + device tree descriptor, non hardcoded

- **4.1** Creare [kernel/lib/dt_loader.c](./kernel/lib/dt_loader.c) che parsa device tree e istanzia driver struct
- **4.2** Definire formato descriptor driver (compatible string, probe function ptr, flags)
- **4.3** Migrare 2-3 driver verso device tree binding (uart, timer, console)
- **4.4** Test: `make run ARCH=aarch64 && make run ARCH=amd64`
- **Effort**: 3-4 hours | **Risk**: medio

### FASE 5: Build System Revision
**Obiettivo**: Standardizzare build, semplificare bootloader, unificare ISO generation

- **5.1** Analizzare [tools/mkdisk.c](./tools/mkdisk.c) vs [scratch/base-nexs-main scripts/](./scratch/base-nexs-main/scripts/)
- **5.2** Creare nuovo [scripts/build-image.sh](./scripts/build-image.sh) che genera image per entrambe arch
- **5.3** Consolidare [boot/](./boot/) e [kernel/arch/*/boot/](./kernel/arch/*/boot/) in unica struttura
- **5.4** Test: `make clean && make run ARCH=aarch64` e `make run ARCH=amd64` da zero
- **Effort**: 2-3 hours | **Risk**: medio (build è fragile)

### FASE 6: Registry & Syscall Dispatch Upgrade
**Obiettivo**: Portare registry + syscall dispatcher maturo da scratch/base-nexs-main

- **6.1** Analizzare [scratch/base-nexs-main/registry/](./scratch/base-nexs-main/registry/) e [scratch/base-nexs-main/core/syscall_dispatch.c](./scratch/base-nexs-main/core/syscall_dispatch.c)
- **6.2** Selezionare design pattern (hash table? tree?) e estrarre core
- **6.3** Adattare a current os1 codebase — tipo conversion, ABI preservation
- **6.4** Migrare prime 10-15 syscall su nuovo dispatcher
- **6.5** Test: `make run ARCH=aarch64 && make run ARCH=amd64`
- **Effort**: 4-5 hours | **Risk**: alto (syscall è critico)

### FASE 7: VFS Layer Implementation
**Obiettivo**: Astrarre filesystem in VFS, supportare multiple fs (ext4, vfat, etc.)

- **7.1** Creare [kernel/fs/vfs.c](./kernel/fs/vfs.c) con inode/dentry abstraction
- **7.2** Implementare file ops (open, read, write, seek)
- **7.3** Migrare ext4 driver a VFS backend
- **7.4** Test: `make run ARCH=aarch64 && make run ARCH=amd64` con VFS mount
- **Effort**: 5-6 hours | **Risk**: alto (fs è delicato)

### FASE 8: Composer & Graphics Stabilization
**Obiettivo**: Stablizzare damage rect compositor, aggiungere render backends

- **8.1** Fix ELR=0 bug (vedi STATUS.md)
- **8.2** Test 120s stability: `timeout 120 make run ARCH=aarch64 2>&1 | tee logs/stability.txt`
- **8.3** Profiling damage rect efficiency
- **8.4** Add OpenGL stub backend
- **Effort**: 3-4 hours | **Risk**: basso

---

## 5. DIPENDENZE & SEQUENZA CONSIGLIATA

```
FASE 1 (Audit)
    ↓
FASE 2 (HAL)  ← prerequisite per FASE 3, 4, 5
    ↓
[FASE 3, 4, 5 parallelo]  ← orthogonal work
    ↓
FASE 6 (Syscall)
    ↓
FASE 7 (VFS)
    ↓
FASE 8 (Stabilizzazione)
```

**Suggerito**: Fare FASE 1 subito, poi chiedere conferma prima di FASE 2.

---

## 6. PROSSIMI STEP

1. **Confermi il piano?** Vuoi aggiustamenti?
2. **Priorita**: Quale fase per primo?
3. **Tempo**: Quante fasi per session?
4. **Tool**: Vuoi un analyzer tool che automatizza i passi audit di FASE 1?

Pronto ad iniziare FASE 1 (Code Cleanup & Assembly Audit) non appena confermi.
