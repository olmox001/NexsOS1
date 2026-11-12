#!/usr/bin/env python3
"""Exercise real staged/unstaged/untracked Git scopes; no fake passes."""
from __future__ import annotations

import subprocess
import tempfile
from pathlib import Path

from changed_gate import build_findings, changed_files, exact_rename_destinations


def git(root: Path, *args: str) -> None:
    subprocess.run(["git", *args], cwd=root, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="nx-power12-changed-") as tmp:
        root = Path(tmp)
        git(root, "init", "-q")
        git(root, "config", "user.name", "Power12 Change Scope Test")
        git(root, "config", "user.email", "power12-test@invalid.example")
        (root / "kernel").mkdir()
        file = root / "kernel" / "timer.c"
        file.write_text("int a = 1;\nint b = 2;\nint c = 3;\n")
        git(root, "add", ".")
        git(root, "commit", "-qm", "baseline")

        # Stage one edit, then make a second edit on the same source file.
        file.write_text("int a = 5;\nint b = 2;\nint c = 3;\n")
        git(root, "add", "kernel/timer.c")
        file.write_text("int a = 5;\nint b = 7;\nint c = 3;\n")
        (root / "kernel" / "new.c").write_text(
            "int d = 4;\nint e = 5;\n")
        lines, removed = changed_files(root)
        if lines.get("kernel/timer.c") != {1, 2}:
            print(f"FAIL combined staged+unstaged: {lines}")
            return 1
        if lines.get("kernel/new.c") != {1, 2}:
            print(f"FAIL untracked source: {lines}")
            return 2
        if removed.get("kernel/timer.c") != 2:
            print(f"FAIL deletion accounting: {removed}")
            return 3

        # A true deletion-only change must remain visible in the scope.
        file.write_text("int a = 1;\nint c = 3;\n")
        lines, removed = changed_files(root)
        if "kernel/timer.c" not in lines or removed["kernel/timer.c"] == 0:
            print("FAIL removal-only change disappears")
            return 4

        makefile = root / "Makefile"
        makefile.write_text("flags += -Wno-error\n")
        findings = build_findings(makefile, root)
        if not any(item["rule"] == "R10" for item in findings):
            print("FAIL warning demotion bypassed")
            return 5
        makefile.write_text("mode := BYPASS_USERLAND_ERRORS\n")
        if not any(item["rule"] == "R10" for item in build_findings(makefile, root)):
            print("FAIL bypass flag ignored")
            return 6

        # Exact path-only renames do not create new Power12 debt.
        git(root, "reset", "--hard", "HEAD")
        git(root, "clean", "-fd")
        git(root, "mv", "kernel/timer.c", "kernel/timer_renamed.c")
        renamed = exact_rename_destinations(root)
        lines, _ = changed_files(root)
        if "kernel/timer_renamed.c" not in renamed or "kernel/timer_renamed.c" in lines:
            print(f"FAIL exact rename treated as content edit: renamed={renamed} lines={lines}")
            return 7

        # Any content edit after the rename must restore delta coverage.
        moved = root / "kernel" / "timer_renamed.c"
        moved.write_text("int a = 9;\nint b = 2;\nint c = 3;\n")
        lines, _ = changed_files(root)
        if lines.get("kernel/timer_renamed.c") != {1}:
            print(f"FAIL edited rename did not preserve line delta: {lines}")
            return 8

    print("PASS: Power12 staged+unstaged+untracked delta, deletions and R10 blockers")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
