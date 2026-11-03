#!/usr/bin/env python3
"""nxrun: repeatable GRUB/QEMU smoke and test gate, implemented via nxadb."""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NXADB = Path(__file__).with_name("nxadb.py")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Boot NexsOS1 through GRUB and run kernel/userland tests",
        epilog="Example: nxrun.py -a aarch64 -n 2 -c captest -c libctest -e captest=64 -e libctest=20",
    )
    parser.add_argument("-a", "--arch", default=os.environ.get("ARCH", "aarch64"),
                        choices=("amd64", "aarch64"))
    parser.add_argument("-n", "--runs", type=int, default=1)
    parser.add_argument("-c", "--cmd", action="append", default=[], dest="cmds")
    parser.add_argument("-e", "--expect", action="append", default=[])
    parser.add_argument("--keep-running", action="store_true",
                        help="leave the last QEMU guest running after the test gate")
    args = parser.parse_args()

    if args.runs < 1:
        parser.error("--runs must be >= 1")

    test_command = [sys.executable, str(NXADB), "-a", args.arch, "test", "-n", str(args.runs)]
    for command in args.cmds:
        test_command.extend(("-c", command))
    for expected in args.expect:
        test_command.extend(("-e", expected))

    result = subprocess.run(test_command, cwd=ROOT)
    if not args.keep_running:
        subprocess.run([sys.executable, str(NXADB), "-a", args.arch, "stop"], cwd=ROOT)
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
