#!/usr/bin/env python3
"""Test actual NX-CORE PIPE status object implementation with HAL mocks."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools/power12"))
from common import clang_ast, source_definitions

def main() -> None:
    target = ROOT / "kernel/core/object.c"
    ast, error = clang_ast(target, "amd64")
    if ast is None:
        raise RuntimeError(error)
    definitions = [d for d in source_definitions(ast, target)
                   if d["name"] == "object_pipe_status"]
    if len(definitions) != 1:
        raise RuntimeError("actual object_pipe_status function is missing")
    node = definitions[0]
    begin = node["range"]["begin"]["offset"]
    end = node["range"]["end"]["offset"] + node["range"]["end"]["tokLen"]
    actual = target.read_bytes()[begin:end].decode()
    match = re.search(r"^#define PIPE_BUF_SIZE\s+(\d+)u?$",
                      target.read_text(), re.M)
    if not match:
        raise RuntimeError("real PIPE_BUF_SIZE constant was not found")
    with tempfile.TemporaryDirectory(prefix="nx-kernel-pipe-status-") as temp:
        work = Path(temp)
        (work / "actual_pipe_status.inc").write_text(actual)
        cflags = ["-DOS1_TEST_PIPE_BUF_SIZE=" + match.group(1) + "U",
                  "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                  "-fsanitize=address,undefined",
                  "-I" + str(ROOT / "kernel/include/abi"),
                  "-I" + str(work)]
        command = ["clang", *cflags,
                   "-x", "c", str(ROOT / "kernel/test/pipe_status_fixture.inc"),
                   "-o", str(work / "test")]
        subprocess.run(command, check=True)
        subprocess.run([str(work / "test")], check=True)
    print("PASS: real PIPE status query + real rejection paths")

if __name__ == "__main__":
    main()
