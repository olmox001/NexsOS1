#!/usr/bin/env python3
"""Print a compact warning census for successful external/vendor compilations."""

from __future__ import annotations

import re
import sys
from collections import Counter
from pathlib import Path

WARNING_RE = re.compile(r"^(.*?):(?:[0-9]+)(?::[0-9]+)?: warning:")


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} BUILD_DIR", file=sys.stderr)
        return 2

    root = Path(sys.argv[1])
    logs = sorted(root.rglob("*.external-warnings")) if root.exists() else []
    files: Counter[str] = Counter()
    warnings = 0

    for log in logs:
        for raw in log.read_text(errors="replace").splitlines():
            if "warning:" not in raw:
                continue
            warnings += 1
            match = WARNING_RE.match(raw)
            if match:
                name = match.group(1)
            else:
                name = log.name.removesuffix(".external-warnings")
            files[name] += 1

    print(f"[external] warnings={warnings} files={len(files)}")
    for name, count in sorted(files.items(), key=lambda item: (-item[1], item[0]))[:12]:
        print(f"[external] {count:4d} {name}")
    if len(files) > 12:
        print(f"[external] ... {len(files) - 12} additional files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
