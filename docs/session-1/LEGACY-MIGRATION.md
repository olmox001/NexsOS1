# Session 1 documentation legacy migration

## Objective

Make Session 1 the only active development documentation namespace while
preserving all earlier documentation as historical evidence.

## Transformation

All pre-Session-1 content previously located directly under `docs/` is moved
under `docs/legacy/` with its relative directory structure retained.

The active root is limited to:

- `docs/README.md`
- `docs/session-1/`
- `docs/legacy/`

Historical files under `docs/legacy/` are preserved byte-for-byte from their
pre-migration Git `HEAD` blobs. Their internal prose is not rewritten, because
old path names, commands and evidence are part of the historical record.

Current references outside the legacy archive (root README, indexes, ignore
rules and current source/document comments) are updated to the new namespace.
The PressToAI index is updated only in its path fields; it is not regenerated or
reformatted.

## Safety rule

No historical document is deleted or semantically rewritten by this migration.
Binary assets, reports, tracked logs, spreadsheets, screenshots and previous
session records retain their original bytes.

Legacy documents can therefore contain historical `docs/...` strings that no
longer name the active path. Active documentation must use
`docs/legacy/...` when linking to them.

## Verification gate

The documentation-layout test verifies:

- active root contains only the approved namespaces/files;
- every pre-Session-1 tracked documentation blob exists under the mapped legacy
  path with an identical SHA/content;
- every indexed project documentation path in `.presstoai.json` exists;
- current tracked project files outside the archive do not retain references to
  the moved historical paths;
- Session 1 active Markdown links resolve;
- all required Session 1 documents exist.

Pre-existing broken references that were already invalid before the migration
are catalogued rather than replaced with invented targets.
