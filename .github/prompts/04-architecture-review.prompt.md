---
description: "Review post-refactor dell'architettura, validazione design patterns, conformità HAL/ABI, identificazione technical debt residuo, genera recommendations"
name: "Architecture Review — Post-Refactor"
agent: "agent"
---

# Architecture Review: Post-Refactor Validation & Technical Debt Assessment

**Scopo**: Dopo completamento di una o più fasi refactor, validare che:

- Architettura rispetta microkernel + ABI POSIX design goals
- HAL è veramente unificato e utilizzabile da entrambe arch
- Code è modulare, portabile, astratto
- Technical debt è stato ridotto (non aumentato)
- Percorso verso stabilizzazione finale è chiaro

---

## TIPO DI REVIEW

Specifica quale:

1. **POST-PHASE**: Dopo completamento di una singola fase (es. FASE 2 — HAL)
   - Focus: Cambio è isolato? Non ha broken other systems?
   - Scope: Files modificati in quella fase

2. **MID-PROJECT**: Checkpoint dopo 2-3 fasi (es. dopo FASE 2+3+4)
   - Focus: Emerge architettura nuova coerente? Direzione è giusta?
   - Scope: Intera codebase

3. **PRE-FREEZE**: Review finale prima di feature-lock
   - Focus: È pronto per produzione? Quali gap rimangono?
   - Scope: Tutte le componenti critiche

Specifica quale tipo vuoi.

---

## ANALISI STRUTTURATA

### 1. DESIGN PRINCIPLE VALIDATION

Per ogni principio chiave, verifica conformità:

#### 1.1 Microkernel Design
```
□ Process scheduler decoupled da driver code?
  → Check: kernel/sched/*.c ha zero device dependencies
  
□ Driver code isolated in kernel/drivers/?
  → Check: No driver code in sched, mm, fs
  
□ IPC mechanism is clean + documented?
  → Check: Syscall interface vs internal messaging
  
□ Privilege levels properly enforced?
  → Check: EL0 (user) vs EL1 (kernel) boundaries
  
□ HAL provides abstraction between arch + core?
  → Check: kernel/core/*.c imports only from HAL, not arch-specific
```

**Scoring**: 0-5 per principle (5 = perfect)

#### 1.2 Portability (Multi-Arch)
```
□ No hardcoded addresses in non-arch code?
  → Grep: "0x" in kernel/*.c (outside arch/)
  
□ All arch-specific code gated by #ifdef ARCH_?
  → Check: config.h consistency
  
□ Device discovery is unified (hal_enumerate_devices)?
  → Check: No two drivers discovery code paths
  
□ Register access is abstracted (hal_read/write)?
  → Check: No raw volatile pointer accesses outside arch/
  
□ Both aarch64 + amd64 boot, run, stability?
  → Test: 60s run on each
```

**Scoring**: 0-5

#### 1.3 Modularity
```
□ Each subsystem has clear interface?
  → Check: fs, mm, sched, graphics have public .h
  
□ Subsystems can be replaced/upgraded independently?
  → Example: Swap ext4 driver → can vfs.c stay same?
  
□ No circular dependencies between subsystems?
  → Tool: Dependency graph generation
  
□ Driver loading is pluggable (device tree based)?
  → Check: Can add new driver without recompiling kernel?
```

**Scoring**: 0-5

---

### 2. CODE QUALITY METRICS

#### 2.1 Abstraction Ratio

```
total_lines = wc -l kernel/**/*.c + kernel/**/*.S

asm_lines = wc -l kernel/**/*.S
c_lines = wc -l kernel/**/*.c
hal_abstraction_lines = grep -c "hal_" kernel/include/kernel/arch.h

ratio = asm_lines / (asm_lines + c_lines)
abstraction_coverage = (functions_behind_hal) / (total_arch_functions)

Target: asm_ratio < 15%, abstraction_coverage > 80%
```

| Metric | Baseline | Post-Phase | Target | Status |
|--------|----------|-----------|--------|--------|
| ASM% | 28% | 22% | <15% | ⚠️ Improved but not there |
| HAL coverage | 45% | 62% | >80% | 🟡 Good progress |
| Duplication | 22% | 14% | <10% | 🟡 Improving |
| Lines/function | 45 | 38 | <30 | 🟡 Acceptable |

#### 2.2 Compiler Warnings

```bash
make clean && make -B 2>&1 | grep -i "warning" | wc -l
```

Target: **Zero warnings**

If warnings > 0:
```
Warning summary:
- unused variable: 3
- implicit declaration: 1
- signed/unsigned mismatch: 2
```

#### 2.3 Code Duplication Analysis

```bash
# Find duplicated 5+ line chunks across files
# Tool: clonedigger, dupfinder, etc.
```

| Chunk | Location A | Location B | Lines | Type | Action |
|-------|-----------|-----------|-------|------|--------|
| memcpy loop | drivers/gpu.c | drivers/console.c | 8 | EXACT | Extract to lib |
| spinlock_acquire | aarch64/boot.S | aarch64/hal.c | 12 | SIMILAR | Unify in HAL |

---

### 3. SUBSYSTEM REVIEWS

#### 3.1 Scheduler (kernel/sched/)

```
Design:
□ Scheduler is CPU-local + per-core balanced?
□ Context switch is atomic (no race conditions)?
□ Idle task handling correct?
□ Priority inversión avoided?

Code Quality:
□ process.c < 800 lines? (modularity check)
□ Per-CPU data structures separated?
□ Test coverage for edge cases (same-cpu return, idle-to-active)?
```

**Risk**: LOW if process.c stable, MEDIUM if new changes

#### 3.2 Memory Management (kernel/mm/)

```
Design:
□ PMM tracks page allocation correctly?
□ VMM paging works on both arch?
□ No memory leak in kernel allocations?

Audit:
□ pmm.c: total allocatable == physical RAM?
□ vmm.c: paging tables consistency?
□ Page table walks don't access NULL?

Test:
□ Heavy allocation stress test (allocate/free loop 10k)?
□ Large allocation test (>512MB if available)?
```

**Risk**: HIGH (memory bugs are subtle)

#### 3.3 Filesystem (kernel/fs/)

```
Design:
□ VFS abstraction separates ext4 from generic ops?
□ inode/dentry lifecycle clear?
□ RO mount supported?

Audit:
□ ext4.c: only touches vfs interface?
□ No hardcoded sector sizes?
□ Device read/write goes through HAL?
```

**Risk**: MEDIUM (fs works, but modularity?)

#### 3.4 Graphics (kernel/graphics/)

```
Design:
□ Compositor damage rect working?
□ FPS target stable (30 Hz)?
□ No GPU hangs?

Audit:
□ compositor.c: damage tracking correct?
□ Double buffering prevents tearing?
□ Font rendering quality acceptable?

Performance:
□ Damage rect reduces transfer % vs full framebuffer?
□ CPU usage for compositor < 5%?
```

**Risk**: LOW (non-critical for boot)

#### 3.5 Drivers (kernel/drivers/)

```
Design:
□ All drivers loaded via device tree?
□ Each driver has probe() + remove() ops?
□ Device discovery unified (no hardcoding)?

Per-Driver:
□ UART: Baud rate configurable? DMA working?
□ Timer: IRQ frequency stable?
□ Virtio: Device negotiation complete?
□ Console: Output is correct (no garbled text)?

Portability:
□ Each driver has both aarch64 + amd64 code paths?
□ Or shared code via HAL?
```

**Risk**: MEDIUM (driver bugs affect stability)

---

### 4. DEPENDENCY GRAPH ANALYSIS

#### 4.1 Build Dependency Tree

```
kernel/main.c
  ├─ kernel/sched/process.c
  │   ├─ kernel/lib/kmalloc.c
  │   └─ kernel/arch/*/context_switch.S
  ├─ kernel/drivers/console.c
  │   └─ kernel/include/kernel/arch.h (HAL)
  └─ kernel/fs/ext4.c
      └─ kernel/drivers/virtio_blk.c
```

**Validate**:
- [ ] No circular deps
- [ ] Each layer depends only on below
- [ ] Drivers don't depend on sched/mm (except kmalloc)

#### 4.2 Header Inclusion Graph

```bash
# Generate include dependencies
grep -r "#include" kernel --include="*.h" --include="*.c" | \
  awk -F: '{print $1}' | sort -u | \
  xargs -I {} sh -c 'echo "=== {} ==="; grep "#include" {}'
```

**Check for**:
- Circular includes (#ifndef guards broken?)
- Deep include chains (>5 levels)
- Non-local includes (outside kernel/)

---

### 5. TECHNICAL DEBT SCORECARD

Per component, valuta quanto "payload" di tech debt rimane:

| Component | Debt | Impact | Priority | Notes |
|-----------|------|--------|----------|-------|
| Boot/HAL | 🔴 HIGH | Blocks driver porting | HIGH | Assembly still 28%, needs consolidation |
| Scheduler | 🟡 MEDIUM | Performance/stability | MEDIUM | ELR=0 bug still pending, context switch solid |
| Memory MM | 🟡 MEDIUM | Correctness | HIGH | No leak detector, need hardening |
| Filesystem | 🟢 LOW | Functionality | LOW | ext4 works, VFS wrapper needed |
| Drivers | 🔴 HIGH | Portability | HIGH | Hardcoded, MMIO not abstracted |
| Graphics | 🟢 LOW | Feature complete | LOW | Compositor stable post-Phase 2 |
| Build System | 🟡 MEDIUM | Maintainability | MEDIUM | mkdisk.c needs revision, ISO generation complex |

**Total Debt Score**: 28/42 = **67%** (1.0 = 100% clean, 0.0 = total mess)

---

### 6. STATIC ANALYSIS

#### 6.1 Compiler Static Checks

```bash
make clean
CFLAGS="-Wall -Wextra -Wpedantic -Wshadow -Wstrict-aliasing" make -B 2>&1 | tee logs/static-analysis.txt

# Count issues per category
grep -c "warning" logs/static-analysis.txt
```

#### 6.2 Manual Code Inspection

For each modified file:

```
File: kernel/arch/aarch64/hal.c
- Lines modified: 145
- Potential issues:
  □ Buffer overflow (bounds checking)?
  □ Use-after-free (lifetime tracking)?
  □ Integer overflow (arithmetic)?
  □ NULL deref (null checks)?
  □ Race conditions (concurrent access)?
  
Result: 0 HIGH, 1 MEDIUM (potential race in spinlock_acquire), 0 LOW
```

---

### 7. GENERATED REVIEW REPORT

#### Executive Summary

```markdown
# Architecture Review Report
**Date**: [auto]
**Reviewer**: Agent (automated + manual inspection)
**Type**: [POST-PHASE | MID-PROJECT | PRE-FREEZE]

## Overall Assessment: ⚠️ GOOD PROGRESS, GAPS REMAIN

### Score Card

| Dimension | Score | Target | Status |
|-----------|-------|--------|--------|
| Design Adherence | 4/5 | 5/5 | 🟡 Very good |
| Code Quality | 3/5 | 5/5 | 🟡 Acceptable |
| Portability | 3/5 | 5/5 | 🟡 Improving |
| Modularity | 3/5 | 5/5 | 🟡 Good foundation |
| **Overall** | **3.2/5** | **5/5** | **🟡 Acceptable, iterate** |

### Key Findings

1. **Positive**: Scheduler refactor decoupled well, no regressions observed
2. **Positive**: HAL coverage increased from 45% → 62%
3. **Concern**: Driver code still has hardcoded addresses (15 instances found)
4. **Concern**: Boot assembly duplication 22% → not consolidated yet
5. **Risk**: Memory management lacks stress tests + leak detector

### Critical Issues (Fix Before Next Phase)

1. **🔴 CRITICAL**: Driver device discovery still hardcoded (blocks amd64 support)
   - Impact: amd64 cannot enumerate virtio devices
   - Fix: Implement hal_enumerate_devices() + update all drivers
   - Effort: 4-6 hours
   - Deadline: Before FASE 4

2. **🔴 CRITICAL**: No VFS abstraction yet (fs is monolithic)
   - Impact: Cannot swap filesystem driver
   - Fix: Create vfs.c interface, wrap ext4 behind it
   - Effort: 6-8 hours
   - Deadline: Before FASE 7

### High Priority (Soon)

3. **🟠 HIGH**: ELR=0 panic bug (STATUS.md noted as deferred)
   - Location: kernel/sched/process.c ~line 609
   - Workaround: None, just documented
   - Fix: Add guard for regs->elr == 0
   - Effort: 1-2 hours
   - Test: 120s stability run

4. **🟠 HIGH**: Compiler warnings (3 active)
   - unused variable in console.c
   - implicit declaration in vmm.c
   - Fix: 30 minutes

### Deferred (Track But Don't Block)

5. 🟡 MEDIUM: MMU setup code still assembly (necessary complexity)
6. 🟡 MEDIUM: mkdisk.c needs revision (complex, low priority)
7. 🟡 MEDIUM: Build system lacks parallelization

## Subsystem Assessment

| Subsystem | Status | Confidence | Action |
|-----------|--------|------------|--------|
| Scheduler | ✅ STABLE | 95% | No changes needed |
| Memory (PMM/VMM) | ⚠️ FUNCTIONAL | 80% | Add stress test |
| Filesystem | 🟡 BASIC | 65% | Wrap in VFS |
| Drivers | ❌ INCOMPLETE | 40% | Device discovery + PCI |
| Graphics | ✅ STABLE | 90% | No changes |
| HAL | 🟡 PARTIAL | 62% | Increase to >80% |

## Recommendations

### Before Next Phase

1. **Implement hal_enumerate_devices()** + update all drivers (CRITICAL)
2. **Fix ELR=0 guard in scheduler** (CRITICAL)
3. **Create VFS abstraction** for filesystem (CRITICAL)
4. **Resolve compiler warnings** (HIGH)
5. **Add PMM stress test** (HIGH)

### Architecture Roadmap

```
Current state: 67% tech debt (scattered code, incomplete abstraction)
                      ↓
FASE 2 (HAL): 50% tech debt (more centralized, less scattered)
                      ↓
FASE 3+4 (Drivers): 35% tech debt (modular driver loading)
                      ↓
FASE 5+6 (Build+Syscall): 20% tech debt (clean build + dispatch)
                      ↓
FASE 7 (VFS): 10% tech debt (modular filesystem)
                      ↓
Target: <10% tech debt (production-ready)
```

### Estimated Timeline

- **Current**: Day 1 (status quo)
- **After FASE 2**: Day 1.5 (+50% effort)
- **After FASE 3+4**: Day 3 (+100% effort)
- **After FASE 5+6**: Day 5 (+200% effort)
- **After FASE 7**: Day 7 (+300% effort)
- **Stabilization**: Day 8-9 (testing, bug fixes)

### Sign-Off

This review validates that:
- ✅ No regressions introduced
- ✅ Architecture direction is sound
- ✅ Next phase is clear
- ⚠️ Critical issues documented + actionable
- ⚠️ Tech debt scorecard established (track progress)

**Recommendation**: Proceed to FASE 3 after CRITICAL items resolved.
```

---

## COME USARE QUESTO TOOL

1. **Dopo completamento fase**:
   - `/Architecture Review — Post-Refactor`
   - Specifica: "POST-PHASE FASE 2"
   - Ricevi: Design validation + technical debt scorecard

2. **A metà progetto** (checkpoint):
   - `/Architecture Review`
   - Specifica: "MID-PROJECT after FASE 2+3+4"
   - Ricevi: Conformità architettura + recommendations

3. **Prima di freezare**:
   - `/Architecture Review`
   - Specifica: "PRE-FREEZE before launch"
   - Ricevi: Completeness check + production readiness

---

## KEY OUTPUTS

- ✅ Design principle validation (5 aspects)
- ✅ Code quality metrics (abstraction, duplication, complexity)
- ✅ Subsystem health checks
- ✅ Technical debt scorecard (track progress)
- ✅ Critical issues prioritized
- ✅ Architectural roadmap to production
- ✅ Effort estimates
- ✅ Go/No-go decision for next phase
