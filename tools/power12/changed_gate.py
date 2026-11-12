#!/usr/bin/env python3
"""Strict Power12 gate for actual staged, unstaged and untracked edits.

This is an additional *delta* gate. It neither changes nor replaces run.py,
which remains the complete project-wide qualification gate.
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path

from audit_ast import (analyze_translation_unit, clang_ast, preprocess_findings,
                       recursion_findings, source_definitions, source_span)
from audit_limits import (DEFINE, LIMIT_NAME, is_material_operation_id)
from common import ROOT, excluded, finding, load_policy, relpath, source_arches

HUNK = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@")
SOURCES = {".c", ".h"}
BUILD_SUFFIXES = {".mk"}
BUILD_NAMES = {"Makefile"}


def git(root: Path, *args: str) -> bytes:
    result = subprocess.run(["git", *args], cwd=root, capture_output=True,
                            check=False)
    if result.returncode != 0:
        raise RuntimeError(result.stderr.decode(errors="replace"))
    return result.stdout



def exact_rename_destinations(root: Path) -> set[str]:
    """Return only worktree destinations whose content is byte-identical to HEAD.

    R100 is a path-only move. Any edited rename is intentionally absent and
    remains in the delta audit.
    """
    raw = git(root, "diff", "HEAD", "--name-status", "-z", "-M100%",
              "--diff-filter=R")
    fields = raw.decode(errors="replace").split("\0")
    result: set[str] = set()
    index = 0
    while index + 2 < len(fields):
        status = fields[index]
        old = fields[index + 1]
        new = fields[index + 2]
        if status == "R100" and old and new:
            result.add(new)
        index += 3
    return result

def changed_files(root: Path) -> tuple[dict[str, set[int]], dict[str, int]]:
    """Compare HEAD to worktree while preserving rename-aware line deltas."""
    raw = git(root, "diff", "HEAD", "--name-status", "-z", "-M",
              "--diff-filter=ACMRT")
    fields = raw.decode(errors="replace").split("\0")
    tracked: set[str] = set()
    rename_sources: dict[str, str] = {}
    pure_renames: set[str] = set()
    index = 0
    while index < len(fields) and fields[index]:
        status = fields[index]
        if status.startswith("R"):
            if index + 2 >= len(fields):
                break
            old = fields[index + 1]
            new = fields[index + 2]
            tracked.add(new)
            rename_sources[new] = old
            if status == "R100":
                pure_renames.add(new)
            index += 3
            continue
        if index + 1 >= len(fields):
            break
        tracked.add(fields[index + 1])
        index += 2
    untracked = {name for name in git(root, "ls-files", "--others",
                                      "--exclude-standard", "-z").decode().split("\0")
                 if name}
    paths = (tracked | untracked) - pure_renames
    lines: dict[str, set[int]] = {}
    deletions: dict[str, int] = {}
    for name in sorted(paths):
        path = root / name
        if not path.is_file() or not path.resolve().is_relative_to(root.resolve()):
            continue
        if name in untracked:
            lines[name] = set(range(1, len(path.read_bytes().splitlines()) + 1))
            deletions[name] = 0
            continue
        old = rename_sources.get(name)
        args = ["diff", "HEAD", "--unified=0", "--no-ext-diff", "-M", "--"]
        if old is not None:
            args.extend([old, name])
        else:
            args.append(name)
        patch = git(root, *args).decode(errors="replace")
        modified: set[int] = set()
        removed = 0
        for row in patch.splitlines():
            match = HUNK.match(row)
            if match:
                new_start = int(match.group(1))
                size = int(match.group(2) or 1)
                modified.update(range(new_start, new_start + size))
                if size == 0:
                    modified.add(max(1, new_start))
            elif row.startswith("-") and not row.startswith("---"):
                removed += 1
        lines[name] = modified
        deletions[name] = removed
    return lines, deletions


def owned(name: str, policy: dict) -> bool:
    return (not excluded(name, policy) and
            (any(name.startswith(x.rstrip("/") + "/")
                 for x in policy["source_roots"]) or
             (name.startswith("user/NexsOS1-main/bin/") and name.count("/") == 3)))


def limit_findings(path: Path, policy: dict) -> list[dict]:
    result = []
    rel = relpath(path)
    for line_no, raw in enumerate(path.read_text(errors="replace").splitlines(), 1):
        match = DEFINE.match(raw)
        if not match:
            continue
        name, value = match.groups()
        key = f"{rel}::{name}"
        if LIMIT_NAME.search(name):
            entry = policy.get("limits", {}).get(key)
            if not isinstance(entry, dict) or any(
                    not entry.get(field) for field in ("kind", "provenance", "rationale")):
                result.append(finding("R12", rel, line_no, name,
                                      f"limit lacks proven policy provenance: {value}"))
        if is_material_operation_id(name):
            entry = policy.get("operations", {}).get(key)
            if not isinstance(entry, dict) or any(
                    entry.get(field) in (None, "") for field in ("id", "owner", "work_bound")):
                result.append(finding("R11", rel, line_no, name,
                                      f"operation/authority ID lacks provenance: {value}"))
    return result


def build_findings(path: Path, root: Path = ROOT) -> list[dict]:
    rel = path.resolve().relative_to(root.resolve()).as_posix()
    result = []
    for no, row in enumerate(path.read_text(errors="replace").splitlines(), 1):
        if "BYPASS_USERLAND_ERRORS" in row or "BYPASS_SEC" in row:
            result.append(finding("R10", rel, no, "", "build bypass is forbidden"))
        if "-Wno-error" in row:
            result.append(finding("R10", rel, no, "", "warning demotion is forbidden"))
        if "NX_DISCARD(" in row:
            result.append(finding("R7", rel, no, "", "discarded fallible result"))
    return result


def touched_functions(path: Path, arch: str, changed: set[int]) -> set[str]:
    ast, _ = clang_ast(path, arch)
    if ast is None:
        return set()
    data = path.read_bytes()
    touched = set()
    for function in source_definitions(ast, path):
        start, end = source_span(function, data)
        if start and end and any(start <= number <= end for number in changed):
            touched.add(function.get("name", "<unnamed>"))
    return touched


def audit_delta(policy: dict, changed: dict[str, set[int]]) -> list[dict]:
    report = []
    edges = []
    definitions = []
    modified_functions: dict[tuple[str, str], set[str]] = {}
    for rel, changed_lines in sorted(changed.items()):
        path = ROOT / rel
        if path.suffix in SOURCES and owned(rel, policy):
            candidates = preprocess_findings(path, policy) + limit_findings(path, policy)
            for item in candidates:
                if item["line"] in changed_lines:
                    report.append(item)
            if path.suffix == ".c":
                for arch in source_arches(rel, policy):
                    current, local_edges, local_defs = analyze_translation_unit(
                        path, arch, policy)
                    touched = touched_functions(path, arch, changed_lines)
                    modified_functions[(rel, arch)] = touched
                    for item in current:
                        # The full gate keeps existing file/function debt.
                        # The changed-only gate reports only findings at lines
                        # actually added/changed (plus unparseable units).
                        # Otherwise one edit to a 600-line legacy test would
                        # misreport its original 100+ findings as regressions.
                        if item["line"] == 0 or item["line"] in changed_lines:
                            report.append(item)
                    edges.extend((rel, arch, a, b, line)
                                 for a, b, line in local_edges)
                    definitions.extend((rel, arch, name, line)
                                       for name, line in local_defs)
        elif rel in BUILD_NAMES or path.suffix in BUILD_SUFFIXES:
            for item in build_findings(path):
                if item["line"] in changed_lines:
                    report.append(item)
    for item in recursion_findings(edges, definitions):
        if (item["line"] in changed.get(item["file"], set()) or
                item["symbol"] in modified_functions.get(
                    (item["file"], item["arch"]), set())):
            report.append(item)
    return sorted({(x["rule"], x["file"], x["line"], x["symbol"],
                    x["arch"], x["detail"]): x for x in report}.values(),
                  key=lambda x: (x["file"], x["line"], x["rule"], x["detail"]))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--report", type=Path,
                        default=ROOT / "build/reports/power12/changed.json")
    parser.add_argument("--max-print", type=int, default=80)
    args = parser.parse_args()
    policy = load_policy()
    changed, deletions = changed_files(ROOT)
    audited = {name: lines for name, lines in changed.items()
               if owned(name, policy) or name in BUILD_NAMES or
               Path(name).suffix in BUILD_SUFFIXES}
    results = audit_delta(policy, audited)
    summary: dict[str, int] = defaultdict(int)
    for item in results:
        summary[item["rule"]] += 1
    output = {
        "profile": policy["profile"],
        "scope": "HEAD-to-working-tree (staged + unstaged + untracked)",
        "head": git(ROOT, "rev-parse", "HEAD").decode().strip(),
        "generated_utc": datetime.now(timezone.utc).isoformat(),
        "changed_files": sorted(changed),
        "audited_files": sorted(audited),
        "changed_line_counts": {name: len(lines) for name, lines in audited.items()},
        "deleted_line_counts": {name: deletions[name] for name in audited},
        "findings": results,
        "summary": dict(summary),
        "verified": not results,
        "full_project_qualified": False,
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(output, indent=2) + "\n")
    print(f"Power12 changed gate: {len(audited)} scoped files, "
          f"{len(results)} findings; full-project qualification remains separate")
    for item in results[:args.max_print]:
        print(f"FAIL {item['rule']} {item['file']}:{item['line']} "
              f"{item['symbol']} ({item['arch']}) {item['detail']}")
    if len(results) > args.max_print:
        print(f"... {len(results)-args.max_print} additional findings in {args.report}")
    return 1 if results else 0


if __name__ == "__main__":
    sys.exit(main())
