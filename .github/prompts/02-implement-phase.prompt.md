---
description: "Esegui una singola fase di implementazione con verifiche stepwise, test automatici dopo ogni modifica, e reportistica completa"
name: "Implementa Fase Refactor"
agent: "agent"
---

# Implementazione Fase Refactor OS1 — Step-by-Step Verified

**Scopo**: Eseguire UNA fase completa del piano refactor, con:
- Modifiche incrementali (1-2 file per step)
- Test automatico dopo ogni change (`make run ARCH=...`)
- Documentazione dei risultati
- Rollback guide se qualcosa falsa

---

## INPUT: Quale Fase?

Specifica quale fase vuoi implementare:

- **FASE 1**: Code Cleanup & Assembly Audit (identificazione target)
- **FASE 2**: HAL Unificazione Layer 1 (astrazione primitives)
- **FASE 3**: Driver PCI/MMIO Abstraction (device access unificato)
- **FASE 4**: Device Tree Modularizzazione (ELF + DT loader)
- **FASE 5**: Build System Revision (mkdisk, ISO, bootloader)
- **FASE 6**: Registry & Syscall Dispatch Upgrade (portare da scratch/base-nexs-main)
- **FASE 7**: VFS Layer Implementation (filesystem abstraction)
- **FASE 8**: Composer & Graphics Stabilization (bug fixes, performance)

---

## WORKFLOW STANDARD

### 1. PRE-PHASE CHECKPOINT

```bash
# Verifica stato baseline
cd "/Users/olmo/iCloud Drive (archivio)/Documents/Antigravity/operate System/gen1/os1test"
git status                    # deve essere pulito
git log --oneline -3          # log recente
make clean                    # pulizia
```

**Test baseline aarch64**:
```bash
timeout 30 make run ARCH=aarch64 2>&1 | tee logs/baseline-aarch64-pre.txt
# Grep per: boot success, panics, errors
```

**Test baseline amd64**:
```bash
timeout 30 make run ARCH=amd64 2>&1 | tee logs/baseline-amd64-pre.txt
```

**Documenta**: Baseline stability (pass/fail), output signature

---

### 2. PHASE IMPLEMENTATION

Per ogni sub-fase:

#### Step 2.A: Analizza File Target
- Leggi file coinvolti completamente (non snippets)
- Identifica interdependenze
- Nota asm vs C code ratio
- Flag complicate parts

**Output Intermediate**: Summary di che cosa vai a modificare

#### Step 2.B: Micro-Modifiche (1-2 file max)
Applica cambio atomico:
- Creazione file header astratto (se HAL)
- Stub implementazione per arch
- Comment detailed design decision

**Output**: Modified files salvate

#### Step 2.C: Compilation Check
```bash
cd "/Users/olmo/iCloud Drive (archivio)/Documents/Antigravity/operate System/gen1/os1test"
make clean
make -B 2>&1 | tee logs/compile-step-X.txt
```

Se errori: analizza, fixa, repeat 2.B

**Output**: Compilation log, status (pass/fail)

#### Step 2.D: Functional Test

**aarch64**:
```bash
timeout 30 make run ARCH=aarch64 2>&1 | tee logs/test-aarch64-step-X.txt
```

Grep per:
- Boot success pattern
- Panics / crashes
- Output changes vs baseline
- Performance impact

**amd64**:
```bash
timeout 30 make run ARCH=amd64 2>&1 | tee logs/test-amd64-step-X.txt
```

**Stessa analisi**

**Output**: Test results, regression detection, pass/fail

#### Step 2.E: Checkpoint Decision

- **PASS**: Stage X done, proceed to X+1
- **FAIL with rollback**: Revert cambio, analyze why, document
- **AMBIGUOUS**: Run longer test (60s), check logs per hidden issues

```bash
# Rollback se serve:
git diff kernel/  # vedi cosa è cambiato
git checkout kernel/  # revert
```

---

### 3. INTRA-PHASE DOCUMENTATION

Dopo ogni step, accumula:

```markdown
## Phase [N] — Implementation Log

### Step 1: [Descrizione]
- Files: kernel/foo.c, kernel/include/bar.h
- Change: moved function X to header, added HAL prefix
- Compile: ✓ PASS
- Test aarch64: ✓ PASS (60s stable)
- Test amd64: ✓ PASS (30s stable)
- Notes: Function signature changed, callers updated in 3 places

### Step 2: [Descrizione]
...
```

Save to: `logs/phase-[N]-implementation.md`

---

### 4. POST-PHASE VALIDATION

Una volta completata la fase:

#### 4.1 Extended Stability Test
```bash
# 120 secondi su entrambe
timeout 120 make run ARCH=aarch64 2>&1 | tee logs/stability-aarch64-final.txt
timeout 120 make run ARCH=amd64 2>&1 | tee logs/stability-amd64-final.txt

# Analysis: zero panics/crashes?
grep -c "ERROR\|PANIC" logs/stability-aarch64-final.txt
grep -c "ERROR\|PANIC" logs/stability-amd64-final.txt
```

#### 4.2 Code Quality Metrics
- [ ] No new compiler warnings
- [ ] No unused variables introduced
- [ ] Code duplication vs baseline (increased or decreased?)
- [ ] Assembly file sizes (asm bytes / total bytes ratio)

```bash
# Count lines assembly vs C
find kernel -name "*.S" -exec wc -l {} + | tail -1  # asm total
find kernel -name "*.c" -exec wc -l {} + | tail -1  # C total
```

#### 4.3 Diff Summary
```bash
git diff --stat HEAD~<N>..HEAD
# Show: files changed, insertions, deletions
```

#### 4.4 Phase Completion Checklist
- [ ] All sub-steps completed
- [ ] Both arch tested stable 60s+
- [ ] No regressions vs baseline
- [ ] Code metrics improved (or documented why they didn't)
- [ ] Implementation log written
- [ ] git commit con messaggio descrittivo

---

### 5. GIT WORKFLOW

Dopo ogni fase:

```bash
git add -A
git commit -m "Phase [N]: [Short description]

- Step X: [what was done]
- Step Y: [what was done]

Test results:
- aarch64: 120s stable, zero panics
- amd64: 120s stable, zero panics

Metrics:
- Asm bytes: -500 (code consolidation)
- C bytes: +1200 (new abstraction)
- Duplication: -15%
"
```

---

## ABORT/ROLLBACK STRATEGY

Se durante fase incontri:

**COMPILATION ERRORS**:
1. Analizza error message dettagliato
2. Fix nel file
3. `make clean && make -B`
4. Se persiste: `git diff kernel/` per vedere cosa è sporco
5. Revert, documenta problema, escalate

**CRASH AT BOOT**:
1. Cattura full log: `timeout 30 make run ARCH=aarch64 2>&1 | tee crash.txt`
2. Cerca panic message, FAR address, context
3. Correlate con modifica appena fatta
4. Se chiaro bug: fixa
5. Se unclear: revert, analizza dal baseline

**PERFORMANCE REGRESSION**:
1. Misura tempo boot (prima vs dopo)
2. Se > 50% slower: investigate perché
3. Se dovuto a nuova astrazione: documenta, procedi (normale)
4. Se dovuto a bug: fixa

---

## TEMPLATE OUTPUT — PHASE COMPLETION REPORT

```markdown
# Phase [N] Completion Report

## Obiettivo
[Una riga summary]

## Modifiche Applicate
- File A: [what changed]
- File B: [what changed]
- New file C: [purpose]

## Test Results

### Compilation
- Status: ✓ PASS / ✗ FAIL
- Warnings: 0 (or list)

### Functional Test
| Arch | Duration | Panics | Status |
|------|----------|--------|--------|
| aarch64 | 120s | 0 | ✓ PASS |
| amd64 | 120s | 0 | ✓ PASS |

### Metrics
- Code duplication: -X%
- Assembly lines: X → Y (change)
- HAL abstraction: M functions protected

## Bugs Found & Fixed (This Phase)
- Bug#1: [description] — FIXED
- Bug#2: [description] — DEFERRED (document why)

## Known Issues (Existing)
- [List any new issues discovered]

## Next Phase Dependencies
- [Prerequisite for phase N+1]

## Sign-off
Tested on: [date/time]
Tested by: [tool-assisted]
Ready for: [next phase or team review]
```

---

## CRITICAL INSTRUCTIONS

1. **OGNI modifica** → test su aarch64 PRIMA di amd64
2. **ZERO assumzioni**: Se non sai se qualcosa funziona, testa
3. **ROLLBACK FRIENDLY**: Ogni step deve essere revertible
4. **DOCUMENT EVERYTHING**: Log = source of truth per future phases
5. **Quando in dubbio**: esegui test 60s, non 30s

---

## PRONTO?

**Dimmi quale FASE vuoi implementare adesso**, e iniziamo dal PRE-PHASE CHECKPOINT.
