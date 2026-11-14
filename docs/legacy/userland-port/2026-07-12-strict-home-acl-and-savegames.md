# 2026-07-12 — strict tree ACL, runtime file creation, doom savegames in /home

Increment record (documentation is append-only). Corrects and completes the
previous ACL increment per maintainer direction: EVERY tree outside /home
stays closed to ordinary users; only /home has expanded authority.

## Final write-authority table (vfs_write_allowed, single seam)

| Path                       | machine | root | user | guest |
|----------------------------|---------|------|------|-------|
| `/sys/bin`                 | ✓       | ✗    | ✗    | ✗     |
| `/bin`, `/sys` (explicit branch) | ✓ | ✓    | ✗    | ✗     |
| every other non-home tree (`/etc`, `/fonts`, `/`, …) | ✓ | ✓ | ✗ | ✗ |
| `/home/...`                | ✓       | ✓    | ✓    | ✗     |
| `/home/shared/...`         | ✓       | ✓    | ✓    | ✓     |

The `/bin`+`/sys` branch is kept explicit (maintainer request) even though
the final catch-all enforces the same root-only rule for all remaining
trees; the two emit distinct log messages ("root-only path" vs "outside
/home").

## Runtime file creation unblocked (kilo, doom, everything)

SYS_FILE_WRITE's extra machine-only creation gate predated /home and made
runtime file creation impossible for every real app. Creation authority now
IS write authority: vfs_write_allowed (the tree ACL above) is the single
predicate, then vfs_create brings the file into existence. Consequences:

- **kilo** needs NO source change: its editorSave() already goes through
  file_write ("create-or-overwrite"), which now truly creates — inside
  /home for users, anywhere for root/machine.
- **doom** saves work at runtime instead of writing into pre-created stubs.

## Doom: config + savegames → /home/Documents/doom (fork commits)

- `M_SetConfigDir("/home/Documents/doom/")` replaces the auto-detected "."
  (d_main.c); savegames sit DIRECTLY in configdir (m_config.c) because the
  VFS has no mkdir yet; the `savedir` config default follows
  (doomgeneric_os1.c). Commits on the doom fork's `nexsos-port` branch.
- The hardcoded zero-byte `/bin/doomsav*.dsg` copies are removed from the
  rootfs build; `/home/Documents/doom` and `/home/shared` are pre-created
  there (mkdisk recurses into empty directories, verified).

## Follow-ups queued

- `vfs_mkdir` (ext4 directory creation) — removes the pre-created-directory
  constraint and lets doom/others manage their own subtrees (already on the
  VFS close-out list with rename/truncate).
- Per-user homes (`/home/<user>`) once an identity model beyond PLVL
  exists; `/home/shared` is the guest floor already.

## Validation

Builds green on both architectures; headless boots 8/8, no panic.
Interactive check for the maintainer's next `make run`: in doom, save a
game (files appear as `/home/Documents/doom/doomsav*.dsg` at runtime) and
reload it; with kilo, create a NEW file under /home and save it.
