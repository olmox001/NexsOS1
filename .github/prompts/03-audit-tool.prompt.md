---
description: "Analyzer tool automatizzato che scansiona codebase, misura assembly duplication, identifica funzioni candidabili per migrazione asm→C, crea report audit"
name: "Audit Tool — Code Analysis Automatizzato"
agent: "agent"
---

# Audit Tool: Automated Code Analysis for Phase 1

**Scopo**: Generare report completo di audit automatizzato per **FASE 1 (Code Cleanup & Assembly Audit)** senza fare supposizioni. Questo tool:

- Scansiona struttura boot stage1/stage2 per aarch64 + amd64
- Misura code duplication tra architetture
- Identifica funzioni assembly candidate per migrazione a C
- Analizza dipendenze cross-file
- Produce JSON report + Markdown summary

---

## CONFIGURAZIONE

Specifica cosa vuoi auditare:

```
AUDIT_SCOPE: "full" | "boot-only" | "hal-only" | "drivers-only"
OUTPUT_FORMAT: "json" | "markdown" | "both"
INCLUDE_SIZE_ANALYSIS: true | false
```

Suggerito: **full, markdown+json, true**

---

## ANALISI AUTOMATIZZATA

### 1. BOOT STAGE AUDIT

Per ogni file stage1/stage2 (aarch64 + amd64):

#### 1.1 Parsa File ASM
```c
struct asm_function {
    char name[256];
    int line_start, line_end;
    int byte_size;
    char* body[1000];  // lines
    char* dependencies[100];  // symbols referenced
    bool is_public;
};
```

**Leggi file**:
- [boot/aarch64/stage1.S](./boot/aarch64/stage1.S)
- [boot/aarch64/stage2.S](./boot/aarch64/stage2.S)
- [boot/amd64/stage1.S](./boot/amd64/stage1.S)
- [boot/amd64/stage2.S](./boot/amd64/stage2.S)

**Parse funzioni**: Estrai ogni `.globl function_name` con inizio/fine, byte count, dipendenze

**Output Intermediate**: struct array di funzioni per arch

#### 1.2 Similarity Analysis
Per ogni coppia (aarch64_stage1, amd64_stage1):

```
sim_score = |asm_code_A ∩ asm_code_B| / max(len_A, len_B)

Se sim_score > 70%: flag as DUPLICATE
Se sim_score 40-70%: SIMILAR (same structure, different asm)
Se sim_score < 40%: ARCH_SPECIFIC (ok to separate)
```

**Algoritmo**: Line-by-line comparison dopo normalization (remove comments, normalize spacing)

**Output**: Duplication matrix
```
stage1_aarch64 vs stage1_amd64: 65% duplicate
stage2_aarch64 vs stage2_amd64: 42% duplicate
```

#### 1.3 Size Analysis
```
stage1.S (aarch64): 245 lines, ~3.2 KB
stage1.S (amd64):   198 lines, ~2.8 KB
—————————————
Total: 443 lines, ~6.0 KB
```

Compare con C equivalent estimate (se fossero in C con inline asm mini-sections):
```
Estimated C: ~100 lines, ~1.5 KB
Savings potential: ~4.5 KB, -75% lines
```

---

### 2. FUNCTION MIGRATION CANDIDACY

Per ogni funzione ASM identificata:

#### 2.1 Complexity Scoring
```
score = (lines × 2) + (branches × 3) + (special_regs × 5) + (memory_access × 2)

score < 50: SIMPLE (easily portable to C)
50-150: MODERATE (possible with HAL helpers)
> 150: COMPLEX (keep as asm, wrap with HAL)
```

**Fattori**:
- `lines`: number of asm lines
- `branches`: jne, je, jmp, etc. (control flow complexity)
- `special_regs`: MSR writes, system registers (arch-specific registers)
- `memory_access`: mov/ldr patterns (pointer arithmetic complexity)

#### 2.2 Per-Function Report

```markdown
### Function: memcpy_early (aarch64)
- Lines: 18
- Branches: 2 (loop control)
- Special registers: 0
- Memory accesses: 3
- **Complexity Score: 42** → SIMPLE CANDIDATE
- Dependencies: None
- Duplicated in amd64? NO (uses SSE, too different)
- Migration effort: LOW
- Recommendation: **MIGRATE TO C with inline for performance**
```

```markdown
### Function: spinlock_acquire (aarch64)
- Lines: 12
- Branches: 1 (loop)
- Special registers: 1 (writes SCTLR for atomic)
- Memory accesses: 2
- **Complexity Score: 31** → SIMPLE CANDIDATE
- Dependencies: None
- Duplicated in amd64? YES (almost identical logic)
- Migration effort: LOW
- Recommendation: **EXTRACT TO HAL, implement both arch versions in C**
```

```markdown
### Function: mmu_setup_page_tables (aarch64)
- Lines: 156
- Branches: 18 (nested loops)
- Special registers: 5 (TTBRx, MAIR, TCR)
- Memory accesses: 21
- **Complexity Score: 287** → COMPLEX
- Dependencies: ASID manager, barrier() macro
- Duplicated in amd64? SIMILAR (different register names but same pattern)
- Migration effort: HIGH (requires significant HAL abstraction)
- Recommendation: **KEEP AS ASM, WRAP WITH hal_mmu_setup_pages() interface**
```

---

### 3. HAL ABSTRACTION AUDIT

#### 3.1 Current HAL Coverage

Scan [kernel/include/kernel/arch.h](./kernel/include/kernel/arch.h):

```c
// Find all function declarations with #ifdef ARCH_AARCH64 / #ifdef ARCH_AMD64
// Extract: function name, arch coverage, implementation file
```

Generate matrix:

| Function | aarch64 | amd64 | Type | Status |
|----------|---------|-------|------|--------|
| `halt()` | yes | yes | control | ✓ ABSTRACTED |
| `cli()` | yes | yes | IRQ | ✓ ABSTRACTED |
| `memory_barrier()` | yes | yes | memory | ✓ ABSTRACTED |
| `mmu_enable()` | yes | no | MMU | ✗ INCOMPLETE (amd64 missing) |
| `spinlock_acquire()` | yes | partially | sync | ✗ PARTIAL (no on amd64) |

**Output**: Coverage % per function category

#### 3.2 Missing Abstractions

Grep per `#ifdef ARCH_` in kernel files che DOVREBBERO avere astrazione:

```bash
grep -r "#ifdef ARCH_AARCH64" kernel --include="*.c" --include="*.h" | grep -v "arch/" | head -20
```

**Output**: List di codice arch-specific scattered outside HAL

---

### 4. DRIVER PORTABILITY AUDIT

#### 4.1 Per-Driver Analysis

For each driver in [kernel/drivers/](./kernel/drivers/):

```
Driver: virtio_gpu
- Files: kernel/drivers/gpu/virtio_gpu.c, .h
- Arch support: aarch64 only (hardcoded MMIO base)
- Dependencies: MMIO primitive (gpio_read32, gpio_write32)
- Device discovery: None (hardcoded address 0x100000000)
- Duplicated code: N/A (only one copy)
- PCI support: NO
- MMIO support: YES (hardcoded)
- Recommendation: NEEDS HAL (separate discovery + register access)

Migration steps:
1. Create virtio_device struct (platform_find_device → hal_enumerate_devices)
2. Replace hardcoded addresses with device.base_addr
3. Test on aarch64, add amd64 PCI version
Effort: MEDIUM (3-4 hours)
```

| Driver | Arch | Discovery | MMIO/PCI | Abstraction |
|--------|------|-----------|----------|-------------|
| console/uart | aarch64 | hardcoded | MMIO | ✗ NONE |
| timer | aarch64 | hardcoded | MMIO | ✗ NONE |
| virtio_gpu | aarch64 | hardcoded | MMIO | ✗ NONE |
| virtio_input | aarch64 | hardcoded | MMIO | ✗ NONE |
| irq_ctrl (GIC) | aarch64 | hardcoded | MMIO | ✗ NONE |

---

### 5. CODE DUPLICATION HEATMAP

Generate visual representation:

```
File Duplication Matrix:
                aarch64/  amd64/    Shared   Dedupable
boot/stage1       100%     60%     60%      40% ← focus here
boot/stage2       100%     45%     45%      55% ← high impact
kernel/main       100%    100%    100%       0% ← already unified
kernel/arch/*     100%      0%      0%     100% ← inherently separate

Total boot: 443 lines, ~100 lines duplicated (22%)
Consolidation potential: ~45 lines of unified C helpers
```

---

### 6. GENERATED REPORT

#### 6.1 Executive Summary

```markdown
# OS1 Code Audit Report — Phase 1 Analysis

**Date**: [auto-generated]
**Scope**: Full codebase audit for assembly consolidation

## Key Findings

1. **Boot Consolidation Opportunity**: 443 lines boot asm, 22% duplication detected
   - Stage1: 65% similar → HIGH consolidation potential
   - Stage2: 42% similar → MEDIUM consolidation potential

2. **Simple Migration Candidates**: 7 functions flagged as SIMPLE (score <50)
   - Examples: memcpy_early, spinlock_acquire, halt sequence
   - Total lines: 142 asm lines → ~50 C lines (65% reduction)
   - Estimated effort: 6-8 hours

3. **HAL Abstraction Gaps**: 5 arch-specific code sections found outside HAL
   - Missing: proper device discovery, MMIO/PCI abstraction
   - Impact: 3 drivers cannot work on amd64

4. **Driver Portability**: 5/7 drivers are aarch64-only
   - Reason: hardcoded MMIO addresses, no device discovery
   - Fix: Implement hal_enumerate_devices() + hal_device_read_mmio()

## Recommendations (Priority Order)

1. **IMMEDIATE**: Extract spinlock_acquire to HAL (used everywhere)
2. **IMMEDIATE**: Add device discovery abstraction (hal_enumerate_devices)
3. **HIGH**: Consolidate boot stage1 (duplicate detection + unified C wrapper)
4. **HIGH**: Wrap MMIOr/PCI register access (hal_device_read/write)
5. **MEDIUM**: Migrate simple functions (memcpy, cache flush)
6. **DEFERRED**: MMU setup (keep as asm, wrap interface)

## Metrics

| Metric | Value |
|--------|-------|
| Total ASM lines | 1243 |
| Duplicated lines | 223 (18%) |
| Simple migration candidates | 142 lines |
| Estimated consolidation | ~55 lines of HAL |
| Phase 1 estimated effort | 10-12 hours |
```

#### 6.2 JSON Output

```json
{
  "timestamp": "2026-05-12T10:30:00Z",
  "scope": "full",
  "summary": {
    "total_asm_lines": 1243,
    "duplication_percent": 18,
    "simple_candidates": 7,
    "simple_candidates_lines": 142,
    "hal_gaps": 5,
    "driver_portability_issues": 5
  },
  "boot_analysis": {
    "stage1": {
      "aarch64_lines": 245,
      "amd64_lines": 198,
      "similarity_percent": 65,
      "dedup_potential": 40
    }
  },
  "function_candidates": [
    {
      "name": "spinlock_acquire",
      "arch": "aarch64",
      "lines": 12,
      "complexity_score": 31,
      "recommendation": "EXTRACT_TO_HAL",
      "effort": "LOW"
    }
  ],
  "drivers": [
    {
      "name": "console_uart",
      "arch_support": ["aarch64"],
      "discovery": "hardcoded",
      "abstraction_needed": true,
      "effort": "MEDIUM"
    }
  ]
}
```

---

## COME USARE QUESTO TOOL

1. **Esegui audit completo**:
```
/Audit Tool — Code Analysis Automatizzato
```

2. **Specifica scope** (se richiesto):
   - Includi solo boot? → "boot-only"
   - Solo driver? → "drivers-only"

3. **Ricevi report** in Markdown + JSON

4. **Usa report per pianificare FASE 1**:
   - Top candidates da migrare
   - Interdipendenze identificate
   - Effort stima più accurata

---

## TOOL FEATURES

- ✅ Automatic asm parsing (comments removed, normalized)
- ✅ Cross-arch duplication detection
- ✅ Complexity scoring per function
- ✅ HAL coverage matrix
- ✅ Driver portability analysis
- ✅ Code size estimation
- ✅ Migration effort estimation
- ✅ JSON + Markdown output

---

## NEXT STEP

Esegui questo tool, ottieni il report, poi usa **02-implement-phase.prompt.md** per FASE 1 con dati concreti.
