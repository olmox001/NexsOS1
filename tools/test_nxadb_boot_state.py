#!/usr/bin/env python3
"""Actual acceptance gate semantics, including negative/legacy fixtures."""
from nxadb import (
    SHELL_PROMPT_SETTLE_SECONDS,
    _boot_nxtest_state,
    _nxwins_focused_tty_window,
    _prompt_settle_state,
    boot_has_live_userland,
)


def check_prompt_settle() -> int:
    ready, since = _prompt_settle_state("no prompt", None, 10.0)
    if ready or since is not None:
        print("FAIL prompt-absent state")
        return 1

    prompt = "NXShell:/home> "
    ready, since = _prompt_settle_state(prompt, None, 20.0)
    if ready or since != 20.0:
        print("FAIL prompt-first-observation state")
        return 1

    ready, same_since = _prompt_settle_state(
        prompt, since, 20.0 + SHELL_PROMPT_SETTLE_SECONDS / 2.0
    )
    if ready or same_since != since:
        print("FAIL prompt-early-settle state")
        return 1

    ready, same_since = _prompt_settle_state(
        prompt, since, 20.0 + SHELL_PROMPT_SETTLE_SECONDS
    )
    if not ready or same_since != since:
        print("FAIL prompt-stable state")
        return 1
    return 0


def check_boot_nxtest_state() -> int:
    cases = (
        ("absent", "NXShell: Alive", "absent"),
        (
            "running",
            "[NXTEST] real userland suite started in isolated boot TTY",
            "running",
        ),
        (
            "done",
            "[NXTEST] real userland suite started in isolated boot TTY\n"
            "[NXTEST] done: 18/19 passed, 1 failure(s)",
            "done",
        ),
    )
    for name, log, expected in cases:
        actual = _boot_nxtest_state(log)
        if actual != expected:
            print(f"FAIL NXTEST {name}: {actual}, expected {expected}")
            return 1
    return 0


def check_nxwins_tty_owner() -> int:
    current = (
        "ID  PID  STATE   GEOMETRY NAME\n"
        "104  14  focus   pos=434,256 surface=640x432 "
        "draw=640x432 /sys/services/nxttyd\n"
    )
    if _nxwins_focused_tty_window(current) != "104":
        print("FAIL nxwins canonical nxttyd owner")
        return 1

    legacy = (
        "ID  PID  STATE   GEOMETRY NAME\n"
        "104  14  focus   pos=434,256 surface=640x432 "
        "draw=640x432 /sys/bin/nxshell\n"
    )
    if _nxwins_focused_tty_window(legacy) is not None:
        print("FAIL nxwins accepted legacy nxshell WINDOW owner")
        return 1

    not_focused = (
        "ID  PID  STATE   GEOMETRY NAME\n"
        "104  14  shown   pos=434,256 surface=640x432 "
        "draw=640x432 /sys/services/nxttyd\n"
    )
    if _nxwins_focused_tty_window(not_focused) is not None:
        print("FAIL nxwins accepted non-focused TTY")
        return 1

    if _nxwins_focused_tty_window(
        "104  14  focus /sys/services/nxttyd\n"
    ) is not None:
        print("FAIL nxwins accepted output without structured header")
        return 1
    return 0


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
    if check_prompt_settle() != 0:
        return 1
    if check_boot_nxtest_state() != 0:
        return 1
    if check_nxwins_tty_owner() != 0:
        return 1
    print(
        f"PASS: {len(cases)} boot readiness cases + "
        "4 prompt-settle cases + 3 boot-NXTEST states + "
        "4 nxwins TTY-owner cases"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
