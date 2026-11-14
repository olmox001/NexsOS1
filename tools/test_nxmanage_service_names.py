#!/usr/bin/env python3
"""Exercise service-name, executable-path and control-message constraints."""
from pathlib import Path
import re

BASE = Path(__file__).resolve().parents[1]
CONT = (BASE / "user/NexsOS1-main/sys/boot/services.conf").read_text().splitlines()
NAME = re.compile(r"[a-z][a-z0-9_-]{0,22}\Z")
EXECUTABLE = re.compile(r"/sys/(?:bin|services)/[A-Za-z0-9_-]+\Z")


def validate(lines: list[str]) -> bool:
    names: set[str] = set()
    for raw in lines:
        if not raw or raw.startswith("#"):
            continue
        fields = raw.split("|")
        if len(fields) not in (5, 6):
            return False
        name, mode, executable, args, key = fields[:5]
        authority = fields[5] if len(fields) == 6 else ""
        if not NAME.fullmatch(name) or name in names:
            return False
        names.add(name)
        if mode not in ("lock", "once", "start", "async", "debug"):
            return False
        if not EXECUTABLE.fullmatch(executable):
            return False
        if authority not in ("", "window-manage"):
            return False
        if len(args.split()) > 2 or len(executable) + len(args) + len(key) + 3 > 64:
            return False
        if key and not re.fullmatch(r"srv\.[A-Za-z0-9_.-]+", key):
            return False
    return bool(names)


def main() -> int:
    baseline = [i for i, line in enumerate(CONT) if line.startswith("notify|")]
    if not validate(CONT) or len(baseline) != 1:
        return 1
    idx = baseline[0]
    samples = (
        CONT[idx],
        "forged|lock|../evil||",
        "forged|lock|/tmp/evil||",
        "a" * 40 + "|lock|/sys/bin/nxshell||",
        "forged|lock|/sys/bin/nxshell|a b c|",
        "forged|lock|/sys/bin/nxshell|" + "a" * 65 + "|",
        "forged|lock|/sys/bin/nxshell||||",
    )
    if any(validate(CONT + [malformed]) for malformed in samples):
        return 1
    print(f"PASS: {len(samples)} hostile service-name/executable framing mutations rejected")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
