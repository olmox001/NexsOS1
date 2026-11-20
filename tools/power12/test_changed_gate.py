#!/usr/bin/env python3
"""Exercise real staged/unstaged/untracked Git scopes; no fake passes."""
from __future__ import annotations

import subprocess
import tempfile
from pathlib import Path

from changed_gate import (audit_delta, audited_path, build_findings,
                          changed_files, exact_rename_destinations,
                          is_blocking_finding, workspace_changes)
from common import load_policy


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

        # Project userland remains audited/reporting, but its delta findings
        # are warning-only. Kernel/SDK/USERKIT-owned findings remain blocking.
        if is_blocking_finding({"file": "user/NexsOS1-main/sys/bin/test.c"}):
            print("FAIL NexsOS1-main delta unexpectedly blocks")
            return 7
        for owned_path in ("kernel/core/test.c", "system/lib/test.c",
                           "user/USERKIT/services/test.c"):
            if not is_blocking_finding({"file": owned_path}):
                print(f"FAIL blocking scope demoted: {owned_path}")
                return 8

        # External Linux and GRUB files must be excluded even when named
        # Makefile or *.mk. First-party kernel/HAL files stay in scope.
        policy = load_policy()
        external_paths = (
            "kernel/external/linux/Makefile",
            "kernel/external/linux/drivers/net/Makefile",
            "kernel/external/linux/kernel/Kbuild.mk",
            "kernel/external/linux/kernel/sched/core.c",
            "kernel/external/grub/Makefile",
        )
        for external in external_paths:
            if audited_path(external, policy):
                print(f"FAIL external source included in Power12: {external}")
                return 12
        if audit_delta(policy, {path: {1} for path in external_paths}):
            print("FAIL external source produced Power12 findings")
            return 13
        for first_party in ("kernel/Makefile", "kernel/HAL/core/nxpax.c",
                            "kernel/core/object.c", "system/Makefile"):
            if not audited_path(first_party, policy):
                print(f"FAIL first-party Power12 scope lost: {first_party}")
                return 14

        # Exact path-only renames do not create new Power12 debt.
        git(root, "reset", "--hard", "HEAD")
        git(root, "clean", "-fd")
        git(root, "mv", "kernel/timer.c", "kernel/timer_renamed.c")
        renamed = exact_rename_destinations(root)
        lines, _ = changed_files(root)
        if "kernel/timer_renamed.c" not in renamed or "kernel/timer_renamed.c" in lines:
            print(f"FAIL exact rename treated as content edit: renamed={renamed} lines={lines}")
            return 9

        # Any content edit after the rename must restore delta coverage.
        moved = root / "kernel" / "timer_renamed.c"
        moved.write_text("int a = 9;\nint b = 2;\nint c = 3;\n")
        lines, _ = changed_files(root)
        if lines.get("kernel/timer_renamed.c") != {1}:
            print(f"FAIL edited rename did not preserve line delta: {lines}")
            return 10

        # A gitlink may advance, then receive staged and untracked edits.
        child = root / "system"
        child.mkdir()
        git(child, "init", "-q")
        git(child, "config", "user.name", "SDK fixture")
        git(child, "config", "user.email", "sdk@invalid.example")
        header = child / "api.h"
        header.write_text("int one;\nint two;\n")
        git(child, "add", ".")
        git(child, "commit", "-qm", "SDK baseline")
        git(root, "add", "system")
        git(root, "commit", "-qm", "pin SDK")
        header.write_text("int three;\nint two;\n")
        git(child, "commit", "-qam", "SDK advance")
        header.write_text("int three;\nint four;\n")
        git(child, "add", "api.h")
        (child / "new.c").write_text("int five;\n")
        lines, _ = workspace_changes(root, {"source_roots": ["system"]})
        if lines.get("system/api.h") != {1, 2} or lines.get("system/new.c") != {1}:
            print(f"FAIL nested SDK changes relative to pinned revision: {lines}")
            return 11

    print("PASS: Power12 delta scope, report-only userland policy, deletions and R10 blockers")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
