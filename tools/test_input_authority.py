#!/usr/bin/env python3
"""Compile the real input resource implementation with bounded HAL mocks."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools/power12"))
from common import clang_ast, source_definitions

def main() -> None:
    path = ROOT / "kernel/resources/input.c"
    ast, error = clang_ast(path, "amd64")
    if ast is None:
        raise RuntimeError(error)
    names = {"input_read", "nx_input_resource_acquire"}
    nodes = [n for n in source_definitions(ast, path) if n["name"] in names]
    if {n["name"] for n in nodes} != names:
        raise RuntimeError("input access source functions not found")
    original = path.read_bytes()
    fragments = {}
    for n in nodes:
        rng = n["range"]
        fragments[n["name"]] = original[
            rng["begin"]["offset"]:
            rng["end"]["offset"] + rng["end"]["tokLen"]
        ].decode()
    full = fragments["input_read"] + "\n\n" + (
        "static const struct nx_resource_ops input_ops = "
        "{.read = input_read};\n\n") + fragments["nx_input_resource_acquire"]
    scheduler = ROOT / "kernel/sched/process.c"
    sched_ast, error = clang_ast(scheduler, "amd64")
    if sched_ast is None:
        raise RuntimeError(error)
    policy, = (n for n in source_definitions(sched_ast, scheduler)
               if n["name"] == "ipc_input_allowed")
    span = policy["range"]
    full += "\n" + scheduler.read_bytes()[
        span["begin"]["offset"]:span["end"]["offset"] + span["end"]["tokLen"]
    ].decode()
    types = (ROOT / "kernel/include/abi/posix_types.h").read_text()
    events = re.findall(r"^#define IPC_TYPE_(?:INPUT|MOUSE|NOTIFY) [^\n]+", types, re.M)
    if len(events) != 3:
        raise RuntimeError("missing input IPC ABI declarations")
    full = "\n".join(events) + "\n" + full
    with tempfile.TemporaryDirectory(prefix="nx-input-auth-") as temp:
        work = Path(temp)
        (work / "real_input_access.inc").write_text(full)
        command = [
            "clang", "-x", "c", "-std=c11", "-O1", "-g", "-Wall",
            "-Wextra", "-Werror", "-fsanitize=address,undefined",
            "-I" + str(ROOT / "kernel/include/abi"),
            "-I" + str(work),
            str(ROOT / "kernel/test/input_authority_fixture.inc"),
            "-o", str(work / "test")]
        subprocess.run(command, check=True)
        subprocess.run([str(work / "test")], check=True)
    print("PASS: real input provider, raw-service authorization and app denial")

if __name__ == "__main__":
    main()
