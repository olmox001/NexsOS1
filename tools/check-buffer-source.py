#!/usr/bin/env python3
"""Reproducible source checks; this is not a full Power12 certification.

Checks actual Clang ASTs for the buffer provider/client on both target ISAs.
Ownership, transitive work bounds and rules not checked here remain review
obligations in the buffer assurance record.
"""
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ("kernel/resources/buffer.c",
           "system/lib/extensions/services/buffer_client.c")
HEAP = {"malloc", "calloc", "realloc", "free", "kmalloc", "kfree",
        "aligned_alloc", "posix_memalign"}

def nodes(tree):
    pending = [tree]
    while pending:
        item = pending.pop()
        yield item
        pending.extend(item.get("inner", []))

def check_source(source, architecture):
    kernel = source.startswith("kernel/")
    target = "x86_64-elf" if architecture == "amd64" else "aarch64-none-elf"
    flags = ["clang", f"--target={target}", "-std=c11", "-ffreestanding",
             "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-Wconversion",
             "-Wsign-conversion", "-Xclang", "-ast-dump=json", "-fsyntax-only"]
    includes = ["system/include/api", "system/include/contract"]
    if kernel:
        flags += [f"-DARCH_{architecture.upper()}", "-DKERNEL"]
        includes = ["kernel/include", "kernel/include/abi", "kernel/include/api",
                    "kernel/HAL/include", f"kernel/HAL/arch/{architecture}/include"]
    flags += [f"-I{path}" for path in includes]
    run = subprocess.run(flags + [source], cwd=ROOT, capture_output=True, text=True)
    if run.returncode:
        raise SystemExit(run.stderr)
    tree = json.loads(run.stdout)
    text = (ROOT / source).read_text()
    count = 0
    for function in tree.get("inner", []):
        if function.get("kind") != "FunctionDecl":
            continue
        body = next((n for n in function.get("inner", [])
                     if n.get("kind") == "CompoundStmt"), None)
        if body is None or function.get("loc", {}).get("includedFrom"):
            continue
        first = function["range"]["begin"]["offset"]
        last = function["range"]["end"]["offset"]
        lines = text.count("\n", first, last) + 1
        if lines > 50:
            raise SystemExit(f"{source}: {function['name']}: {lines} lines > 50")
        for node in nodes(body):
            if node.get("kind") in {"GotoStmt", "IndirectGotoStmt"}:
                raise SystemExit(f"{source}: goto in {function['name']}")
            ref = node.get("referencedDecl", {})
            if ref.get("kind") == "FunctionDecl":
                name = ref.get("name")
                if name in HEAP or name == function["name"]:
                    raise SystemExit(f"{source}: forbidden call/reference {name}")
        count += 1
    print(f"{architecture} {source}: {count} functions; strict diagnostics, "
          "length, direct recursion and general-heap checks PASS")

def main():
    for architecture in ("amd64", "aarch64"):
        for source in SOURCES:
            check_source(source, architecture)

if __name__ == "__main__":
    main()
