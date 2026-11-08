# nxec — Assembly Modules, ABI e verifica automatizzata

**ID:** SPEC-ASM-MODULE-001  
**Versione:** 0.1.0-DESIGN  
**Stato:** DESIGN / TOOLING TO IMPLEMENT

## 1. Obiettivo

Ogni componente assembly ISA-specific è progetto autonomo con toolchain, ABI, manifest, build, disassembly, test e risultati. Assembly entra nel Core solo se necessaria e dimostra correttezza, compatibilità e sicurezza.

## 2. Organizzazione progressiva

- preboot/arch/amd64: bootstrap e primitive AMD64.
- preboot/arch/arm64: bootstrap AArch64.
- kernel/hal/isa/amd64 e arm64: backend HAL.
- modules/<id>/arch/<isa>: routine specializzate.

Directory create al primo sorgente/test reale. Codice comune C17; assembly per transizioni, registri, atomiche, context switch e primitive senza equivalente C portabile.

## 3. Manifest

Ogni assembly module dichiara module ID/version/source revision, ABI, ISA/subtarget, feature K-class, privilege/domain, entry symbols, calling convention, register clobber/preserve, stack alignment/red-zone, memory ordering/barriers, relocation types, section permissions, worst-case stack usage, bounded loops, dependencies, capabilities, test vectors e hardware/QEMU profiles.

Manifest è input non attendibile e viene validato.

## 4. Build

AMD64: Clang integrated assembler o NASM quando richiesto. ARM64: LLVM/Clang AArch64 assembler. ABI C interoperabile fissata per target. Build fissa march/mcpu/mattr e non eredita feature host. K2 solo su CPU USABLE con affinity/migration policy verificata.

## 5. Pipeline

1. Parse manifest/schema.
2. Verifica ISA/subtarget/feature.
3. Compila warning-as-error.
4. Verifica ELF header, sections, symbols, relocation e visibility.
5. Disassembla con llvm-objdump e archivia.
6. Verifica istruzioni presenti contro manifest.
7. Verifica W^X, stack, clobber ABI e relocation whitelist.
8. Unit test host.
9. QEMU per target e feature flags.
10. Registra hash, toolchain, CPU model, flags, output e metriche.

Analisi statica non prova da sola assenza side effects o correttezza concorrente.

## 6. K-class e fallback

K0 arch-independent; K1 ISA implementation; K2 CPU feature; K3 accelerator; K4 software fallback; K5 optional/resource conditional; K6 privileged/capability-bound. Ogni modulo dichiara classi applicabili. K4 deve preservare semantica, autorizzazioni e precisione contrattuale. K2/K3 richiedono CPU target, pinning/migration, state save/restore e revocation.

## 7. NASA Rule of 10

Loop assembly con bound statico; nessun salto non controllato, self-modifying code o MMIO senza capability/memory-order contract. Stack/clobber espliciti, zero warning e test limiti. Funzioni assembly piccole e con contratto leggibile da C.

## 8. QEMU e hardware

Testare CPU base e feature on/off, smp 1 e multiprocessore, TCG e accelerazione host, feature eterogenee quando supportate, fault injection, interrupt e preemption. Classificare QEMU_TCG, QEMU_ACCELERATED o PHYSICAL_HARDWARE. QEMU non prova equivalenza del silicio reale.

## 9. Output e gate

Conservare manifest normalizzato, command line, compiler version, object hash, ELF metadata, disassembly, static analysis, QEMU log, test e performance. Prima del primo modulo production: ABI boot/kernel, validator manifest, ELF metadata parser, disassembly checks e QEMU matrix.
