#!/usr/bin/env python3
"""Test real NX-CORE capability descriptor duplication with controlled mocks."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools/power12"))
from common import clang_ast, source_definitions
NAMES = {"obj_unref", "endpoint_release", "object_duplicate_min", "object_duplicate_to", "sys_handle_dup",
         "process_redirect_child_fd_from", "process_redirect_child_fd"}

def main() -> None:
    source = ROOT / "kernel/core/object.c"
    ast, error = clang_ast(source, "amd64")
    if ast is None:
        raise RuntimeError(error)
    nodes = [n for n in source_definitions(ast, source)
             if n["name"] in NAMES]
    if {n["name"] for n in nodes} != NAMES:
        raise RuntimeError("missing real object duplicate implementation")
    original = source.read_bytes()
    included = "\n\n".join(original[
            n["range"]["begin"]["offset"]:
            n["range"]["end"]["offset"]+n["range"]["end"]["tokLen"]
        ].decode() for n in sorted(nodes,
            key=lambda n: n["range"]["begin"]["offset"]))
    header = ROOT / "kernel/include/kernel/object.h"
    match = re.search(r"^#define NPROC_HANDLES\s+(\d+)$",
                      header.read_text(), re.M)
    if not match:
        raise RuntimeError("real process handle bound unavailable")
    with tempfile.TemporaryDirectory(prefix="nx-object-dup-") as temp:
        work = Path(temp)
        (work / "actual_handle_dup.inc").write_text(included)
        cmd = ["clang", "-x", "c", "-std=c11", "-O1", "-g", "-Wall",
               "-Wextra", "-Werror", "-fsanitize=address,undefined",
               "-DOS1_TEST_NPROC_HANDLES=" + match.group(1),
               "-I" + str(ROOT / "kernel/include/abi"), "-I" + str(work),
               str(ROOT / "kernel/test/handle_dup_fixture.inc"),
               "-o", str(work / "test")]
        subprocess.run(cmd, check=True)
        subprocess.run([str(work / "test")], check=True)
    print("PASS: real capability dup min/exact; revocation/rights/denials")

if __name__ == "__main__":
    main()
