#!/usr/bin/env python3
"""Shared infrastructure for the NEXS Power12 build gate."""
from __future__ import annotations
import json
import os
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
POLICY_PATH = Path(__file__).with_name("policy.json")

def load_policy() -> dict:
    return json.loads(POLICY_PATH.read_text())

def relpath(path: Path) -> str:
    return path.resolve().relative_to(ROOT.resolve()).as_posix()

def excluded(rel: str, policy: dict) -> bool:
    return any(rel == p.rstrip("/") or rel.startswith(p)
               for p in policy["excluded_prefixes"])

def owned_tree_files(policy: dict) -> list[Path]:
    found: dict[str, Path] = {}
    for root_name in policy["source_roots"]:
        base = ROOT / root_name
        if not base.exists():
            continue
        for base_dir, dir_names, file_names in os.walk(base):
            base_path = Path(base_dir)
            rel_dir = relpath(base_path)
            dir_names[:] = [name for name in dir_names
                            if not excluded(f"{rel_dir}/{name}/", policy)]
            for name in file_names:
                path = base_path / name
                rel = relpath(path)
                if not excluded(rel, policy):
                    found[rel] = path
    for pattern in policy["first_party_root_globs"]:
        for path in ROOT.glob(pattern):
            rel = relpath(path)
            if path.is_file() and not excluded(rel, policy):
                found[rel] = path
    return [found[key] for key in sorted(found)]

def owned_files(policy: dict, suffixes: tuple[str, ...] | None = None) -> list[Path]:
    wanted = set(suffixes or policy["extensions"])
    return [path for path in owned_tree_files(policy) if path.suffix in wanted]

def source_arches(rel: str, policy: dict) -> list[str]:
    if rel.startswith("kernel/HAL/tests/"):
        return ["host"]
    if "/arch/amd64/" in rel or "/amd64/" in rel:
        return ["amd64"]
    if "/arch/aarch64/" in rel or "/arm64/" in rel or "/aarch64/" in rel:
        return ["aarch64"]
    # Structural rules are source properties. Common files are parsed once;
    # real cross-builds remain the architecture-specific R10 evidence.
    return ["amd64"]

def clang_args(rel: str, arch: str) -> list[str]:
    if arch == "host":
        target_args = []
        arch_define = "-DARCH_AMD64"
        arch_inc = "kernel/HAL/arch/amd64/include"
    elif arch == "amd64":
        target_args = ["--target=x86_64-unknown-none-elf"]
        arch_define = "-DARCH_AMD64"
        arch_inc = "kernel/HAL/arch/amd64/include"
    else:
        target_args = ["--target=aarch64-unknown-none-elf"]
        arch_define = "-DARCH_AARCH64"
        arch_inc = "kernel/HAL/arch/aarch64/include"
    args = [
        "clang", *target_args, "-std=gnu17", "-ffreestanding",
        "-fno-builtin", "-Wno-unknown-warning-option", "-Wno-unused-command-line-argument",
        arch_define,
    ]
    if rel.startswith("kernel/") and arch != "host":
        args += ["-DKERNEL"]
    if arch == "host":
        include_dirs = ["kernel/HAL", "kernel/HAL/include"]
    else:
        include_dirs = [
            "kernel/HAL", "kernel/HAL/include", arch_inc,
            "kernel/include", "kernel/include/abi", "kernel/include/api",
            "system/include/contract", "system/include/api",
            "user/lib/stb", "user/lib/pl_mpeg", "user/lib/sdl/include",
            "user/lib/lua", "user/lib/portability/lua",
            "user/portability/coreutils", "user/portability/gnulib",
            "user/lib/gnulib/lib", "user/bin/coreutils/gnulib/lib",
        ]
    for item in include_dirs:
        args += ["-I", str(ROOT / item)]
    if rel == "user/bin/gnulibtest.c" or rel.startswith("user/portability/gnulib/"):
        args += [
            "-D__NEXSOS__", "-DFLEXIBLE_ARRAY_MEMBER=",
            "-include", str(ROOT / "user/portability/gnulib/gnulib_config_nexsos.h"),
        ]
    if rel == "user/portability/gnulib/gnulib_os1_glue.c":
        # This translation unit intentionally owns the externally linked
        # memeq; the production Makefile compiles it with the same define.
        # Without it the forced Gnulib config header declares a second
        # static inline memeq and AST analysis reports a false parse failure.
        args += ["-D_GNULIB_OS1_GLUE_IMPL"]
    if rel.startswith("user/portability/coreutils/"):
        args += [
            "-include",
            str(ROOT / "user/portability/coreutils/coreutils_config_nexsos.h"),
        ]
    return args

def clang_ast(path: Path, arch: str) -> tuple[dict | None, str]:
    rel = relpath(path)
    cmd = clang_args(rel, arch) + [
        "-Xclang", "-ast-dump=json", "-fsyntax-only", str(path)
    ]
    run = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True,
                         check=False, timeout=45)
    if run.returncode != 0:
        return None, run.stderr.strip()
    try:
        return json.loads(run.stdout), ""
    except json.JSONDecodeError as exc:
        return None, f"invalid clang AST JSON: {exc}"

def node_children(node: dict) -> list[dict]:
    return [x for x in node.get("inner", ()) if isinstance(x, dict)]

def walk(node: dict):
    stack = [node]
    while stack:
        current = stack.pop()
        yield current
        stack.extend(reversed(node_children(current)))

def line_for(node: dict, data: bytes) -> int:
    loc = node.get("loc", {})
    if not loc:
        loc = node.get("range", {}).get("begin", {})
    line = loc.get("line")
    if isinstance(line, int):
        return line
    offset = loc.get("offset")
    if isinstance(offset, int) and 0 <= offset <= len(data):
        return data[:offset].count(b"\n") + 1
    return 0

def source_definitions(ast: dict, path: Path) -> list[dict]:
    data = path.read_bytes()
    resolved = path.resolve()
    result = []
    for node in ast.get("inner", ()):
        if not isinstance(node, dict) or node.get("kind") != "FunctionDecl":
            continue
        if node.get("isImplicit"):
            continue
        if not any(x.get("kind") == "CompoundStmt" for x in node_children(node)):
            continue
        loc = node.get("loc", {})
        if loc.get("includedFrom"):
            continue
        file_name = loc.get("file")
        if file_name and Path(file_name).resolve() != resolved:
            continue
        if line_for(node, data) == 0:
            continue
        result.append(node)
    return result

def finding(rule: str, rel: str, line: int, symbol: str, detail: str,
            arch: str = "all") -> dict:
    return {
        "rule": rule, "file": rel, "line": int(line or 0),
        "symbol": symbol, "arch": arch, "detail": detail,
    }
