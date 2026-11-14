#!/usr/bin/env python3
"""Evidence-first Power12 audit. An incomplete proof is NEVER a passing gate.

The AST checks below diagnose actual compiled function bodies (not text/name
occurrence counts). The tool deliberately fails strict mode when whole-program
proof obligations remain open. This first scope is the NXPAX implementation;
it is not a declaration of repository-wide Power12 compliance.
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCES = (
    "kernel/HAL/core/nxpax.c",
    "kernel/core/nxpax.c",
    "kernel/resources/nxpax.c",
    "user/NexsOS1-main/sys/bin/nxpax.c",
)
INCLUDES = (
    "kernel/HAL", "kernel/HAL/include", "kernel/HAL/arch/amd64/include",
    "kernel/include", "kernel/include/abi", "kernel/include/api",
    "system/include/contract", "system/include/api",
)
FORBIDDEN = frozenset(("setjmp", "longjmp", "_setjmp", "_longjmp"))
HEAP = frozenset(("malloc", "calloc", "realloc", "free", "kmalloc",
                  "kfree", "alloca", "__builtin_alloca"))
LOOP_KINDS = frozenset(("ForStmt", "WhileStmt", "DoStmt"))
RUNTIME_ASSERTS = frozenset(("assert", "KASSERT", "NX_ASSERT"))
PHYSICAL_LINE_MAX = 50


def children(node):
    return [child for child in node.get("inner", ()) if child]


def walk(node):
    stack = [node]
    while stack:
        current = stack.pop()
        yield current
        stack.extend(reversed(children(current)))


def call_name(node):
    for part in walk(node):
        if part.get("kind") == "DeclRefExpr":
            declaration = part.get("referencedDecl", {})
            if declaration.get("kind") == "FunctionDecl":
                return declaration.get("name")
    return None


def location_line(loc, data):
    """Clang compresses repeated .line fields; the byte offset is authoritative."""
    if isinstance(loc.get("line"), int):
        return loc["line"]
    offset = loc.get("offset")
    if isinstance(offset, int) and 0 <= offset < len(data):
        return data[:offset].count(b"\n") + 1
    return None


def definitions(ast, lines, source):
    """Never silently discard a real source function due to formatting.

    Header definitions are excluded by explicit AST source location, not by
    matching or counting identifiers in the C text.
    """
    result = []
    full_source = (ROOT / source).resolve()
    data = full_source.read_bytes()
    for node in ast.get("inner", ()):
        if node.get("kind") != "FunctionDecl" or node.get("isImplicit"):
            continue
        if not any(item.get("kind") == "CompoundStmt" for item in children(node)):
            continue
        loc = node.get("loc", {})
        if loc.get("includedFrom"):
            continue
        file_name = loc.get("file")
        if file_name and Path(file_name).resolve() != full_source:
            continue
        line = location_line(loc, data)
        name = node.get("name", "<unnamed>")
        if not isinstance(line, int) or line < 1 or line > len(lines):
            raise ValueError(f"{source}: function {name} has unresolved location")
        if name not in lines[line - 1]:
            raise ValueError(
                f"{source}:{line}: function {name} has nonlocal/macro location")
        result.append(node)
    return result


def ast_for(source):
    cmd = ["clang", "--target=x86_64-unknown-none-elf", "-std=c17",
           "-ffreestanding", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
           "-DARCH_AMD64", "-DKERNEL", "-Xclang", "-ast-dump=json",
           "-fsyntax-only"]
    for include in INCLUDES:
        cmd.extend(("-I", str(ROOT / include)))
    cmd.append(str(ROOT / source))
    run = subprocess.run(cmd, text=True, capture_output=True, check=False)
    if run.returncode:
        raise RuntimeError(run.stderr.strip() or f"clang failed ({run.returncode})")
    return json.loads(run.stdout)


def unwrapped(node):
    while node.get("kind") in ("ImplicitCastExpr", "ParenExpr",
                                "ConstantExpr") and children(node):
        node = children(node)[0]
    return node


def static_bound(node):
    node = unwrapped(node)
    kind = node.get("kind")
    if kind == "IntegerLiteral":
        return True
    if kind == "UnaryExprOrTypeTraitExpr":
        return node.get("name") == "sizeof"
    if kind == "BinaryOperator" and node.get("opcode") in ("+", "-", "*"):
        operands = children(node)
        return len(operands) == 2 and all(static_bound(x) for x in operands)
    return False


def local_references(node):
    return {part.get("referencedDecl", {}).get("name")
            for part in walk(node) if part.get("kind") == "DeclRefExpr"}


def initial_zero(init, name):
    for declaration in walk(init):
        if declaration.get("kind") != "VarDecl":
            continue
        if declaration.get("name") != name:
            continue
        contents = children(declaration)
        return len(contents) == 1 and unwrapped(contents[0]).get(
            "kind") == "IntegerLiteral" and unwrapped(contents[0]).get(
                "value") == "0"
    return False


def loop_has_proven_bound(node):
    """Prove one induction variable with a compile-time bound and progress.

    Unsupported forms fail closed; the checker does not infer 'finite' from a
    renamed call, a token occurrence or the mere presence of a comparison.
    """
    if node.get("kind") != "ForStmt":
        return False
    parts = node.get("inner", ())
    if len(parts) < 5 or not parts[0] or not parts[2] or not parts[3]:
        return False
    increment = unwrapped(parts[3])
    if increment.get("kind") != "UnaryOperator" or increment.get(
            "opcode") != "++":
        return False
    counter_refs = local_references(increment)
    if len(counter_refs) != 1:
        return False
    name = next(iter(counter_refs))
    if not initial_zero(parts[0], name):
        return False
    for candidate in walk(parts[2]):
        if candidate.get("kind") != "BinaryOperator" or candidate.get(
                "opcode") != "<":
            continue
        operands = children(candidate)
        if len(operands) != 2 or name not in local_references(operands[0]):
            continue
        if not static_bound(operands[1]):
            continue
        # A second write in the body can invalidate monotonic progress.
        body = parts[-1]
        for statement in walk(body):
            if statement.get("kind") == "UnaryOperator" and statement.get(
                    "opcode") in ("++", "--"):
                if name in local_references(statement):
                    return False
            if statement.get("kind") in ("BinaryOperator",
                                         "CompoundAssignOperator") and (
                    statement.get("opcode") in ("=", "+=", "-=")):
                body_children = children(statement)
                if body_children and name in local_references(body_children[0]):
                    return False
        return True
    return False


def cycles(edges):
    findings = []
    visiting = set()
    visited = set()

    def visit(name):
        if name in visiting:
            findings.append(name)
            return
        if name in visited:
            return
        visiting.add(name)
        for target in edges.get(name, ()):
            if target in edges:
                visit(target)
        visiting.remove(name)
        visited.add(name)

    for symbol in edges:
        visit(symbol)
    return sorted(set(findings))


def check_macros(source, lines, findings):
    for line_number, raw in enumerate(lines, 1):
        line = raw.strip()
        if not line.startswith("#"):
            continue
        if line.startswith(("#include", "#ifndef", "#define", "#endif")):
            if line.startswith("#define"):
                name = line.removeprefix("#define").strip().split(" ", 1)[0]
                if "##" in line or "..." in line or "(" in name or re.search(
                        r"#\s*[A-Za-z_]", line.removeprefix("#define")):
                    findings.append(("R8", source, line_number,
                                     "complex or token-transforming macro"))
            continue
        findings.append(("R8", source, line_number,
                         "conditional/complex preprocessing requires review"))


def check_source(source, edges, findings, checked):
    path = ROOT / source
    data = path.read_bytes()
    lines = data.decode("utf-8").splitlines()
    check_macros(source, lines, findings)
    try:
        ast = ast_for(source)
    except (RuntimeError, ValueError) as exc:
        findings.append(("R10", source, 0, f"strict clang AST failed: {exc}"))
        return
    try:
        defined = definitions(ast, lines, source)
    except ValueError as exc:
        findings.append(("R10", source, 0, str(exc)))
        return
    for function in defined:
        name = function["name"]
        begin = location_line(function.get("range", {}).get("begin", {}), data)
        end = location_line(function.get("range", {}).get("end", {}), data)
        line = location_line(function.get("loc", {}), data)
        checked.append((source, name, line))
        if begin is None or end is None:
            findings.append(("R4", source, line,
                             f"{name}: function source span unproved"))
        elif end - begin + 1 > PHYSICAL_LINE_MAX:
            findings.append(("R4", source, begin, f"{name}: {end-begin+1} lines"))
        edges.setdefault(name, set())
        for node in walk(function):
            kind = node.get("kind")
            node_line = node.get("range", {}).get("begin", {}).get("line", line)
            if kind in ("GotoStmt", "IndirectGotoStmt"):
                findings.append(("R1", source, node_line, f"{name}: forbidden goto"))
            if kind in LOOP_KINDS and not loop_has_proven_bound(node):
                findings.append(("R2", source, node_line,
                                 f"{name}: loop upper bound is not proved"))
            if kind == "CallExpr":
                called = call_name(node)
                if called:
                    edges[name].add(called)
                    if called in FORBIDDEN:
                        findings.append(("R1", source, node_line,
                                         f"{name}: forbidden nonlocal jump"))
                    if called in HEAP:
                        findings.append(("R3", source, node_line,
                                         f"{name}: dynamic allocation call {called}"))
            if kind in ("ParmVarDecl", "VarDecl", "FieldDecl"):
                typ = node.get("type", {}).get("qualType", "")
                if "**" in typ or re.search(r"\(\s*\*[^)]*\)\s*\(", typ):
                    findings.append(("R9", source, node_line,
                                     f"{name}: pointer-depth/function-pointer review: {typ}"))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--strict", action="store_true",
                        help="fail for findings AND all unproved Power12 obligations")
    args = parser.parse_args()
    findings = []
    edges = {}
    checked = []
    for source in SOURCES:
        check_source(source, edges, findings, checked)
    for name in cycles(edges):
        origins = [item for item in checked if item[1] == name]
        if origins:
            source, _, line = origins[0]
            findings.append(("R1", source, line, f"{name}: recursive call cycle"))
    checked.sort()
    print(f"Power12 source/AST scope: NXPAX, {len(checked)} function definitions")
    for source, name, line in checked:
        print(f"  function {source}:{line} {name}")
    for rule, source, line, note in sorted(findings):
        print(f"FAIL {rule} {source}:{line} {note}")
    # These rules require semantic or cross-boundary evidence. Do not turn
    # the absence of an AST hit into proof of an unanalysed safety property.
    unproved = {
        "R3": "no whole-callgraph/runtime-phase allocation proof",
        "R5": "meaningful side-effect-free assertion density not established",
        "R6": "minimal lifetime and scope not established",
        "R7": "all fallible results and input contracts not established",
        "R9": "indirect pointer authority across ABI not established",
        "R10": "full clang-tidy/cppcheck target matrix not attached",
        "R11": "stable operation IDs and checked work accounting not proved",
        "R12": "all capacity bounds require provenance and acceptance evidence",
    }
    for rule, note in unproved.items():
        print(f"NOT_VERIFIED {rule} {note}")
    print(f"Power12 findings={len(findings)} open_proofs={len(unproved)}")
    if args.strict and (findings or unproved or not checked):
        return 1
    return 1 if findings or unproved or not checked else 0


if __name__ == "__main__":
    sys.exit(main())
