#!/usr/bin/env python3
"""Exercise service-name, namespace and control-message size constraints."""
from pathlib import Path
import re

BASE = Path(__file__).resolve().parents[1]
CONT = (BASE / "user/sys/boot/services.conf").read_text().splitlines()
NAME = re.compile(r"[a-z][a-z0-9_-]{0,22}\Z")
EXEC = re.compile(r"/sys/(?:bin|services)/[A-Za-z0-9_-]+\Z")


def validate(lines: list[str]) -> bool:
    names: set[str] = set()
    for raw in lines:
        if not raw or raw.startswith("#"):
            continue
        fields = raw.split("|")
        if len(fields) != 5:
            return False
        name, mode, path, args, key = fields
        if not NAME.fullmatch(name) or name in names:
            return False
        names.add(name)
        if mode not in ("lock", "once", "debug") or not EXEC.fullmatch(path):
            return False
        if len(args.split()) > 2 or len(path) + len(args) + len(key) + 3 > 64:
            return False
        if key and not re.fullmatch(r"srv\.[A-Za-z0-9_.-]+", key):
            return False
    return bool(names)


def main() -> int:
    valid = validate(CONT)
    baseline = [i for i, line in enumerate(CONT) if line.startswith("notify|")]
    if not valid or len(baseline) != 1:
        return 1
    idx = baseline[0]
    samples = (
        ("duplicate name", CONT[idx]),
        ("parent traversal", "forged|lock|/sys/services/../evil||"),
        ("user writable", "forged|lock|/home/evil||"),
        ("long name", "a" * 40 + "|lock|/sys/bin/nxshell||"),
        ("extra parameter", "forged|lock|/sys/bin/nxshell|a b c|"),
        ("oversized PORT", "forged|lock|/sys/bin/nxshell|" + "a" * 65 + "|"),
        ("new line field", "forged|lock|/sys/bin/nxshell|||"),
    )
    if any(validate(CONT + [malformed]) for _, malformed in samples):
        return 1
    print(f"PASS: {len(samples)} hostile service-name and PORT framing mutations rejected")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
