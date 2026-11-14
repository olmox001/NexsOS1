#!/usr/bin/env python3
from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOCS = ROOT / "docs"
LEGACY = DOCS / "legacy"
SESSION1 = DOCS / "session-1"
ACTIVE = DOCS / "session-2"
LEGACY_SOURCE_COMMIT = "5a1264c310b034e3fa574301731b0f933361aceb"
ALLOWED_ROOT = {"README.md", "legacy", "session-1", "session-2"}
MD_LINK_RE = re.compile(r"\[[^\]]*\]\(([^)]+)\)")


def git_bytes(*args: str) -> bytes:
    return subprocess.check_output(["git", "-C", str(ROOT), *args])


def old_doc_paths() -> list[str]:
    return git_bytes("ls-tree", "-r", "--name-only", LEGACY_SOURCE_COMMIT, "docs").decode().splitlines()


def check_root_layout(errors: list[str]) -> None:
    actual = {p.name for p in DOCS.iterdir()}
    if actual != ALLOWED_ROOT:
        errors.append(
            f"docs root mismatch: expected {sorted(ALLOWED_ROOT)}, got {sorted(actual)}"
        )


def check_legacy_preservation(errors: list[str]) -> None:
    for old in old_doc_paths():
        if not old.startswith("docs/"):
            continue
        rel = old[len("docs/"):]
        mapped = LEGACY / rel
        if not mapped.is_file():
            errors.append(f"missing migrated legacy file: {old}")
            continue
        if git_bytes("show", f"{LEGACY_SOURCE_COMMIT}:{old}") != mapped.read_bytes():
            errors.append(f"legacy content changed: {old}")


def check_index_migration(errors: list[str]) -> None:
    indexed = set(git_bytes("ls-files", "--cached").decode().splitlines())
    for old in old_doc_paths():
        if not old.startswith("docs/"):
            continue
        rel = old[len("docs/"):]
        mapped = f"docs/legacy/{rel}"
        if mapped not in indexed:
            errors.append(f"legacy file missing from Git index: {mapped}")


def walk_paths(value):
    if isinstance(value, dict):
        for key, item in value.items():
            if key == "path" and isinstance(item, str):
                yield item
            yield from walk_paths(item)
    elif isinstance(value, list):
        for item in value:
            yield from walk_paths(item)


def check_presstoai(errors: list[str]) -> None:
    index = json.loads((ROOT / ".presstoai.json").read_text())
    docs_paths = [p for p in walk_paths(index) if p.startswith("docs/")]
    if not docs_paths:
        errors.append(".presstoai.json contains no project docs paths")
        return
    for path in docs_paths:
        if not path.startswith(("docs/legacy/", "docs/session-1/", "docs/session-2/")):
            errors.append(f"stale PressToAI docs path: {path}")
        elif not (ROOT / path).exists():
            errors.append(f"missing PressToAI indexed path: {path}")


def check_current_references(errors: list[str]) -> None:
    old = set(old_doc_paths())
    try:
        output = git_bytes(
            "grep", "-n", "-I", "docs/", "--", ".", ":(exclude)docs/**"
        ).decode("utf-8", "replace")
    except subprocess.CalledProcessError as exc:
        output = exc.output.decode("utf-8", "replace")
    for line in output.splitlines():
        path, _, text = line.partition(":")
        if not text:
            continue
        for token in old:
            if token in text:
                errors.append(f"stale moved-doc reference: {path}: {token}")


def local_link_target(source: Path, raw: str) -> Path | None:
    raw = raw.strip()
    if not raw or raw.startswith(("#", "http://", "https://", "mailto:")):
        return None
    raw = raw.split("#", 1)[0].split("?", 1)[0]
    if not raw:
        return None
    target = Path(raw)
    if target.is_absolute():
        return ROOT / raw.lstrip("/")
    return source.parent / target


def check_active_markdown_links(errors: list[str]) -> None:
    sources = [
        ROOT / "README.md", DOCS / "README.md",
        *sorted(SESSION1.glob("*.md")), *sorted(ACTIVE.glob("*.md"))
    ]
    for source in sources:
        text = source.read_text()
        for raw in MD_LINK_RE.findall(text):
            target = local_link_target(source, raw)
            if target is not None and not target.exists():
                errors.append(
                    f"broken active markdown link: {source.relative_to(ROOT)} -> {raw}"
                )


def check_required_active_docs(errors: list[str]) -> None:
    required_session1 = {
        "README.md", "METHODOLOGY.md", "ARCHITECTURE.md",
        "GPU-DEVICE-CENSUS.md", "REFERENCES.md", "PLAN.md", "OBJECTIVES.md",
        "STATE.md", "ISSUES.md", "VERIFICATION.md", "LEGACY-MIGRATION.md",
        "P2P-TRANSCRIPT.md",
    }
    session1_actual = {p.name for p in SESSION1.glob("*.md")}
    missing_session1 = sorted(required_session1 - session1_actual)
    if missing_session1:
        errors.append(f"missing Session 1 docs: {missing_session1}")

    required_session2 = {
        "README.md", "OBJECTIVES.md", "STATE.md", "ISSUES.md", "VERIFICATION.md",
    }
    session2_actual = {p.name for p in ACTIVE.glob("*.md")}
    missing_session2 = sorted(required_session2 - session2_actual)
    if missing_session2:
        errors.append(f"missing active Session 2 docs: {missing_session2}")


def main() -> int:
    errors: list[str] = []
    check_root_layout(errors)
    check_legacy_preservation(errors)
    if "--require-index" in sys.argv[1:]:
        check_index_migration(errors)
    check_presstoai(errors)
    check_current_references(errors)
    check_active_markdown_links(errors)
    check_required_active_docs(errors)
    if errors:
        for error in errors:
            print(f"docs-layout FAIL: {error}", file=sys.stderr)
        print(f"docs-layout: {len(errors)} failure(s)", file=sys.stderr)
        return 1
    print("docs-layout PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
