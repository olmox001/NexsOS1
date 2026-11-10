#!/usr/bin/env python3
"""Actual acceptance gate semantics, including negative/legacy fixtures."""
from nxadb import boot_has_live_userland


def main() -> int:
    cases = (
        ("empty", "", False),
        ("K3-only", "K3: Initialized PID 1 (/sbin/extension)", False),
        ("shell-only", "NXShell: Alive", False),
        ("old-supervisor", "[Init] Entering supervisor loop", True),
        ("new-extension", "K3: Initialized PID 1 (boot extension /krnl/extension/nxmanageDD)\nNXShell: Alive", True),
        ("wrong-K3-PID", "K3: Initialized PID 12\nNXShell: Alive", False),
        ("unstarted-shell", "K3: Initialized PID 1\n[Init] failed to spawn shell", False),
    )
    for name, log, expected in cases:
        actual = boot_has_live_userland(log)
        if actual != expected:
            print(f"FAIL {name}: {actual}, expected {expected}")
            return 1
    print(f"PASS: {len(cases)} real boot readiness positive/negative cases")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
