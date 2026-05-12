# OS1 Project Prompts — Quick Start Guide

Custom prompts per il refactor + modernizzazione del progetto OS1. **Usa sequenzialmente** per risultati ottimali.

---

## 📋 I Tuoi 4 Prompt

### 1️⃣ **Analizza & Pianifica OS Refactor**
📄 [`01-analyze-and-plan.prompt.md`](01-analyze-and-plan.prompt.md)

**Quando**: All'inizio, oppure per planning generale
**Output**: 
- Scansione completa codebase (senza supposizioni)
- Identificazione 7 problemi strutturali
- Piano con 8 fasi micro-verificabili
- Sequenza dipendenze + stima effort

**Usage**:
```
Digita: /
Seleziona: "Analizza & Pianifica OS Refactor"
Specifica nulla (prende input dal codebase)
```

**Tempo**: ~30 min (lettura + output planning)

---

### 2️⃣ **Implementa Fase Refactor**
📄 [`02-implement-phase.prompt.md`](02-implement-phase.prompt.md)

**Quando**: Durante implementazione di una fase specifica
**Output**:
- Workflow standardizzato per modifica → test → checkpoint
- Test automatico dopo ogni step (`make run`)
- Report di progresso per ogni step
- Rollback strategy se qualcosa falsa

**Usage**:
```
Digita: /
Seleziona: "Implementa Fase Refactor"
Specifica: "FASE 2" (o quale fase vuoi implementare)
```

**Tempo**: Varia per fase (2-8 hours dipende da FASE)

---

### 3️⃣ **Audit Tool — Code Analysis Automatizzato**
📄 [`03-audit-tool.prompt.md`](03-audit-tool.prompt.md)

**Quando**: Prima di FASE 1, oppure per analysis approfondito
**Output**:
- Report automatizzato: duplication detection, complexity scoring
- Function candidacy per migrazione asm→C
- HAL coverage matrix
- Driver portability audit
- JSON + Markdown report

**Usage**:
```
Digita: /
Seleziona: "Audit Tool — Code Analysis Automatizzato"
Specifica (opzionale): "full", "boot-only", "drivers-only"
```

**Tempo**: ~20 min (scanning + report generation)

---

### 4️⃣ **Architecture Review — Post-Refactor**
📄 [`04-architecture-review.prompt.md`](04-architecture-review.prompt.md)

**Quando**: Dopo completamento di una fase o checkpoint intermedi
**Output**:
- Design principle validation (5 aspetti: microkernel, portability, etc.)
- Code quality metrics (abstraction ratio, duplication, complexity)
- Subsystem health checks
- Technical debt scorecard
- Critical issues prioritized
- Go/No-go decision per next phase

**Usage**:
```
Digita: /
Seleziona: "Architecture Review — Post-Refactor"
Specifica: "POST-PHASE FASE 2" (o "MID-PROJECT" o "PRE-FREEZE")
```

**Tempo**: ~25 min (analysis + review)

---

## 🎯 WORKFLOW CONSIGLIATO

### Opzione A: Step-by-Step Completo (Raccomandato)

```
1. [Oggi] Esegui: 01-analyze-and-plan
   ↓ Ricevi: Piano completo + stima fasi
   
2. [Domani] Opzionale: 03-audit-tool (FASE 1 analysis dettagliato)
   ↓ Ricevi: Report audit assembly, duplication, candidates
   
3. [Domani+1] Esegui: 02-implement-phase FASE 1
   ↓ Ricevi: Step-by-step implementation, logs
   
4. [Domani+1 fine] Esegui: 04-architecture-review POST-PHASE FASE 1
   ↓ Ricevi: Design validation, tech debt scorecard, next phase clear
   
5. Loop per FASE 2, 3, 4, ... (repeat step 3-4)
```

**Tempo totale**: ~2-3 settimane per tutte 8 fasi

### Opzione B: Fast Track (Solo Essenziale)

```
1. Esegui: 01-analyze-and-plan
2. Esegui: 02-implement-phase per FASE di scelta
3. Repeat 2 per altre fasi
4. [Fine progetto] Esegui: 04-architecture-review PRE-FREEZE
```

**Tempo totale**: ~1.5-2 settimane (meno review, più veloce)

---

## 📊 COSA OGNI PROMPT PRODUCE

| Prompt | Output Type | Formato | Use For |
|--------|-------------|---------|---------|
| 01-analyze | Text report | Markdown | Planning, stakeholder communication |
| 02-implement | Implementation guide | Step-by-step checklist | Active development, testing |
| 03-audit | Data + report | JSON + Markdown | Deep analysis, making data-driven decisions |
| 04-review | Scorecard | Markdown + metrics | Quality gates, sign-off, roadmap |

---

## ⚡ QUICK REFERENCE

### Ho una domanda specifica?

| Domanda | Prompt da usare |
|---------|-----------------|
| "Qual è il piano per il refactor?" | 01 |
| "Come faccio FASE 2?" | 02 + specifica FASE 2 |
| "Quali funzioni assembly dovrei migrare a C?" | 03 |
| "È il codice pronto per il prossimo step?" | 04 |
| "Quante linee di assembly duplication?" | 03 |
| "Quali sono i critical bugs nel design?" | 04 |
| "Ho broken qualcosa in FASE X?" | 04 (POST-PHASE) |

---

## 🔧 MANUTENZIONE PROMPTS

Se il progetto cambia significativamente:

1. **Aggiungi una nuova FASE**: Edita `01-analyze-and-plan.prompt.md` sezione "PIANO DETTAGLIATO"
2. **Aggiungi metriche di audit**: Edita `03-audit-tool.prompt.md` sezione "ANALISI AUTOMATIZZATA"
3. **Aggiungi subsystem review**: Edita `04-architecture-review.prompt.md` sezione "SUBSYSTEM REVIEWS"

---

## 📝 NOTE IMPORTANTI

- **Test dopo OGNI modifica**: `make run ARCH=aarch64` poi `make run ARCH=amd64`
- **Zero assunzioni**: Se non sai se qualcosa funziona, testa
- **Documenta tutto**: Logs = source of truth per fasi future
- **Rollback friendly**: Ogni step deve essere revertible via `git`

---

## 🚀 PROSSIMO STEP

**Consigliato**: Esegui adesso il prompt `01-analyze-and-plan` per ottenere il piano dettagliato. 

Comanda:
```
/Analizza & Pianifica OS Refactor
```

Poi conferma con me il piano prima di procedere a implementazioni vere.
