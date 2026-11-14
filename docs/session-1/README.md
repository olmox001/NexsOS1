# NexsOS1 Session 1

This directory is the active operational documentation for Session 1.
The authoritative owner directive is preserved separately in
`/README-LLM.md`. Nothing here overrides that directive.

Documents:

- `METHODOLOGY.md` — mandatory bounded-task, no-assumption and verification protocol.
- `ARCHITECTURE.md` — current target ownership boundaries.
- `GPU-DEVICE-CENSUS.md` — live GPU/display/device ownership, defects and safety evidence.
- `REFERENCES.md` — approved reference families and optional-submodule policy.
- `PLAN.md` — ordered task sequence: documentation, GPU/device safety, Power12, then migrations.
- `OBJECTIVES.md` — stable objective IDs, priorities, status and explicit acceptance gates.
- `STATE.md` — verified current repository/runtime state.
- `ISSUES.md` — open defects and architectural debt, including unrelated findings.
- `VERIFICATION.md` — commands/results already executed and evidence locations.
- `LEGACY-MIGRATION.md` — preservation and path-migration record for pre-Session-1 docs.

Mandatory loop: plan one bounded task, read the relevant files and documentation
in full, verify current interfaces/callers/tests, implement through the canonical
mechanism, add a test for any observed behavior the suite did not previously
detect, verify on the required architecture/runtime matrix, update documentation,
then stage only that verified block.
