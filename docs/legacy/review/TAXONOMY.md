# Code Review Taxonomy — OS1 / NEXS

This document defines the classification scheme used across every analysis
document under `docs/review/` and in the master `REVIEW.md` findings index.

Every finding is tagged on **two independent axes**: a **severity** (W0–W5,
"how much it matters") and one or more **kinds** (what category of problem it
is). A finding's ID encodes its subsystem, e.g. `MM-VMM-01`.

---

## Axis 1 — Severity (Warning Level)

| Level | Name | Meaning | Typical action |
|-------|------|---------|----------------|
| **W0** | Informational | Cosmetic only: style, naming, formatting, a clarifying comment. No functional or design impact. | Optional, batch later. |
| **W1** | Minor | Dead/redundant code, stale or misleading comments, micro-inefficiency, inflated claims in comments/docs. No runtime risk. | Clean up opportunistically. |
| **W2** | Moderate | A real limitation: missing edge-case handling, silent/partial behavior, small/bounded leak, contention, "logic to refine". Works today but constrains correctness or scale. | Schedule into a phase. |
| **W3** | Significant | A genuine bug under plausible conditions, an SMP/cache-coherency hazard, a security weakness, a wrong abstraction/design, or a **stub on a path that is actually used**. | Fix before relying on the path. |
| **W4** | Severe | Broken or missing **core** functionality, data-corruption risk, an exploitable security hole, or a crash on a common path. | Fix soon; blocks maturity. |
| **W5** | Critical | Build/boot blocker, guaranteed corruption/crash, or a fundamental design defect that blocks the project's stated goals. | Fix first. |

> Severity is about **impact given the project's goals**, not how hard it is to
> fix. A one-line omission can be W4 if it corrupts data.

---

## Axis 2 — Kind (Category)

These map directly onto the categories requested for this review.

| Tag | Italian (brief) | Definition |
|-----|-----------------|------------|
| **MISSING** | implementazioni mancanti | Functionality that is referenced, expected, or required but absent. |
| **BUG** | errori | Code that is incorrect for inputs/conditions it will actually meet. |
| **BAD-IMPL** | cattive implementazioni | Works, but implemented in a fragile, convoluted, or non-idiomatic way. |
| **WRONG-DESIGN** | design errato | The abstraction/interface/ownership model itself is wrong for the goal. |
| **STUB** | logica stub | Placeholder that pretends to do work (prints/returns success) but does not. |
| **REFINE** | logica da rifinire | Correct skeleton that needs hardening, completion, or generalization. |
| **PERF** | cali prestazionali | Avoidable algorithmic or structural performance cost. |
| **SECURITY** | sicurezza | Isolation, privilege, memory-safety, or input-validation weakness. |
| **DOC** | (tutto il resto) | Documentation/comment is wrong, stale, or makes unsupported claims. |

A single finding may carry more than one kind (e.g. `STUB + SECURITY`).

---

## Evidence discipline

To keep this review trustworthy for the code owner, every finding states its
**evidence basis**:

- **[verified]** — confirmed by building and/or running (serial log, compiler
  output, disassembly).
- **[static]** — confirmed by reading the source; not exercised at runtime.
- **[inferred]** — a reasoned conclusion that depends on an assumption stated
  inline; flagged so it can be challenged.

No finding enters the master index without a concrete `file:line` citation.

---

## Finding ID scheme

`<SUBSYS>[-<UNIT>]-<NN>` — e.g. `MM-PMM-02`, `DRV-VIRTIO-05`, `BOOT-03`.
Subsystem prefixes: `BOOT`, `ARCH`, `HAL`, `MM`, `SCHED`, `FS`, `DRV`, `IRQ`,
`GFX`, `LIB`, `ABI`, `USR`, `BUILD`, `DOC`.
