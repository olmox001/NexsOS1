#!/usr/bin/env python3
"""Run exact NX-CORE NULL provider methods under AddressSanitizer and UBSan."""
from pathlib import Path
import subprocess
import sys
import tempfile
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools/power12"))
from common import clang_ast, source_definitions

def main():
    p = ROOT / "kernel/resources/null.c"
    ast, error = clang_ast(p, "amd64")
    if ast is None:
        raise RuntimeError(error)
    names = {"null_read", "null_write", "nx_null_resource_acquire"}
    defs = [d for d in source_definitions(ast, p) if d["name"] in names]
    if {d["name"] for d in defs} != names:
        raise RuntimeError("real NULL resource source is incomplete")
    original = p.read_bytes()
    source = "\n\n".join(original[
        d["range"]["begin"]["offset"]:
        d["range"]["end"]["offset"] + d["range"]["end"]["tokLen"]
    ].decode() for d in sorted(defs,
         key=lambda d: d["range"]["begin"]["offset"]))
    # Preserve original immutable ops descriptor, never fabricate provider.
    init = "static const struct nx_resource_ops null_ops ="
    start = p.read_text().find(init)
    end = p.read_text().find("};", start)
    if start < 0 or end < start:
        raise RuntimeError("missing real provider immutable table")
    ops = p.read_text()[start:end+2]
    function_tail = source.find("int nx_null_resource_acquire")
    source = source[:function_tail] + ops + "\n" + source[function_tail:]
    with tempfile.TemporaryDirectory(prefix="nx-null-provider-") as dir:
        work = Path(dir)
        (work / "actual_null_resource.inc").write_text(source)
        cmd = ["clang", "-x", "c", "-std=c11", "-O1", "-g", "-Wall",
               "-Wextra", "-Werror", "-fsanitize=address,undefined",
               "-I" + str(ROOT / "kernel/include/abi"),
               "-I" + str(work),
               str(ROOT / "kernel/test/null_resource_fixture.inc"),
               "-o", str(work / "test")]
        subprocess.run(cmd, check=True)
        subprocess.run([str(work / "test")], check=True)
    print("PASS: actual NULL provider 3 functions x 2 non-fabricated tests")

if __name__ == "__main__":
    main()
