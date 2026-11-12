#!/usr/bin/env python3
"""Clang-AST structural audit for the NEXS Power12 profile.

This intentionally fails closed: unsupported loop/control forms and unparsed
translation units are findings, never silent passes.
"""
from __future__ import annotations
import argparse
import json
import re
from collections import defaultdict
from pathlib import Path

from common import (
    ROOT, clang_ast, finding, line_for, load_policy, node_children,
    owned_files, relpath, source_arches, source_definitions, walk,
)

LOOPS = {"ForStmt", "WhileStmt", "DoStmt"}
CAST_KINDS = {
    "ImplicitCastExpr", "CStyleCastExpr", "ParenExpr", "ConstantExpr",
    "ExprWithCleanups", "MaterializeTemporaryExpr",
}
COMPARE = {"<", "<=", ">", ">="}
ARITH = {"+", "-", "*", "/", "%", "<<", ">>", "&", "|", "^"}

def clean_source(text: str) -> str:
    out = []
    i = 0
    state = "code"
    while i < len(text):
        c = text[i]
        n = text[i + 1] if i + 1 < len(text) else ""
        if state == "code":
            if c == "/" and n == "/":
                out.extend((" ", " "))
                i += 2
                state = "line"
                continue
            if c == "/" and n == "*":
                out.extend((" ", " "))
                i += 2
                state = "block"
                continue
            if c == '"':
                out.append(" ")
                i += 1
                state = "string"
                continue
            if c == "'":
                out.append(" ")
                i += 1
                state = "char"
                continue
            out.append(c)
        elif state == "line":
            if c == "\n":
                out.append("\n")
                state = "code"
            else:
                out.append(" ")
        elif state == "block":
            if c == "*" and n == "/":
                out.extend((" ", " "))
                i += 2
                state = "code"
                continue
            out.append("\n" if c == "\n" else " ")
        elif state in ("string", "char"):
            quote = '"' if state == "string" else "'"
            if c == "\\":
                out.append(" ")
                if i + 1 < len(text):
                    out.append("\n" if text[i + 1] == "\n" else " ")
                    i += 2
                    continue
            if c == quote:
                out.append(" ")
                state = "code"
            else:
                out.append("\n" if c == "\n" else " ")
        i += 1
    return "".join(out)

def unwrap(node: dict) -> dict:
    current = node
    while current.get("kind") in CAST_KINDS and node_children(current):
        current = node_children(current)[0]
    return current

def refs(node: dict) -> set[str]:
    names = set()
    for part in walk(node):
        if part.get("kind") != "DeclRefExpr":
            continue
        decl = part.get("referencedDecl", {})
        name = decl.get("name")
        if name:
            names.add(name)
    return names

def call_name(node: dict) -> str | None:
    for part in walk(node):
        if part.get("kind") != "DeclRefExpr":
            continue
        decl = part.get("referencedDecl", {})
        if decl.get("kind") == "FunctionDecl":
            return decl.get("name")
    return None

def static_expr(node: dict) -> bool:
    node = unwrap(node)
    kind = node.get("kind")
    if kind in ("IntegerLiteral", "CharacterLiteral", "GNUNullExpr"):
        return True
    if kind == "DeclRefExpr":
        decl = node.get("referencedDecl", {})
        return decl.get("kind") in ("EnumConstantDecl",)
    if kind == "UnaryExprOrTypeTraitExpr":
        return node.get("name") == "sizeof"
    if kind == "UnaryOperator" and node.get("opcode") in ("+", "-", "~"):
        parts = node_children(node)
        return len(parts) == 1 and static_expr(parts[0])
    if kind == "BinaryOperator" and node.get("opcode") in ARITH:
        parts = node_children(node)
        return len(parts) == 2 and all(static_expr(x) for x in parts)
    return False

def find_static_comparison(condition: dict) -> tuple[str, str] | None:
    for part in walk(condition):
        if part.get("kind") != "BinaryOperator" or part.get("opcode") not in COMPARE:
            continue
        operands = node_children(part)
        if len(operands) != 2:
            continue
        left_names = refs(operands[0])
        right_names = refs(operands[1])
        if len(left_names) == 1 and static_expr(operands[1]):
            return next(iter(left_names)), part.get("opcode", "")
        if len(right_names) == 1 and static_expr(operands[0]):
            reverse = {"<": ">", "<=": ">=", ">": "<", ">=": "<="}
            return next(iter(right_names)), reverse[part.get("opcode", "")]
    return None

def direct_variable_write_target(node: dict, name: str) -> bool:
    """An indexed array write does not mutate its index variable.

    The prior broad refs() check treated samples[i] = value as if it assigned
    i itself, producing a false R2 finding for a genuinely bounded loop.
    """
    direct = unwrap(node)
    return (direct.get("kind") == "DeclRefExpr" and
            direct.get("referencedDecl", {}).get("name") == name)


def progress_for(body: dict, name: str, direction: str) -> bool:
    wanted = {"<": "++", "<=": "++", ">": "--", ">=": "--"}[direction]
    matches = 0
    for part in walk(body):
        if part.get("kind") == "UnaryOperator" and part.get("opcode") in ("++", "--"):
            operands = node_children(part)
            if operands and direct_variable_write_target(operands[0], name):
                if part.get("opcode") != wanted:
                    return False
                matches += 1
        if part.get("kind") in ("BinaryOperator", "CompoundAssignOperator"):
            op = part.get("opcode")
            operands = node_children(part)
            if operands and direct_variable_write_target(operands[0], name) and op in ("=", "+=", "-=", "*=", "/="):
                return False
    return matches == 1

def loop_proved(node: dict) -> bool:
    kind = node.get("kind")
    raw = [x for x in node.get("inner", ()) if isinstance(x, dict) and x]
    condition = None
    body = raw[-1] if raw else None
    if kind == "ForStmt":
        for child in raw[:-1]:
            if child.get("kind") == "BinaryOperator" and child.get("opcode") in (
                    COMPARE | {"&&", "||"}):
                condition = child
                break
        if condition is None:
            return False
        bound = find_static_comparison(condition)
        if bound is None:
            return False
        name, direction = bound
        # The canonical increment may be outside the body. Prove it and reject
        # any additional writes in the body.
        increments = [x for x in raw[:-1]
                      if x.get("kind") == "UnaryOperator" and
                      x.get("opcode") in ("++", "--") and name in refs(x)]
        expected = {"<": "++", "<=": "++", ">": "--", ">=": "--"}[direction]
        if len(increments) != 1 or increments[0].get("opcode") != expected:
            return False
        for part in walk(body or {}):
            kind = part.get("kind")
            if kind == "UnaryOperator":
                operands = node_children(part)
                if operands and direct_variable_write_target(operands[0], name):
                    return False
            if kind in ("BinaryOperator", "CompoundAssignOperator"):
                operands = node_children(part)
                if operands and direct_variable_write_target(operands[0], name) and part.get("opcode") in (
                        "=", "+=", "-=", "*=", "/=", "%=", "<<=", ">>=", "&=", "|=", "^="):
                    return False
        return True
    if kind in ("WhileStmt", "DoStmt"):
        for child in raw:
            if child.get("kind") == "BinaryOperator" and child.get("opcode") in (
                    COMPARE | {"&&", "||"}):
                condition = child
                break
        if condition is None or body is None:
            return False
        bound = find_static_comparison(condition)
        return bound is not None and progress_for(body, bound[0], bound[1])
    return False

def source_span(function: dict, data: bytes) -> tuple[int, int]:
    begin = function.get("range", {}).get("begin", {})
    end = function.get("range", {}).get("end", {})
    def get(loc: dict) -> int:
        line = loc.get("line")
        if isinstance(line, int):
            return line
        off = loc.get("offset")
        if isinstance(off, int) and 0 <= off <= len(data):
            return data[:off].count(b"\n") + 1
        return 0
    return get(begin), get(end)

def direct_discarded_calls(function: dict):
    for compound in walk(function):
        if compound.get("kind") != "CompoundStmt":
            continue
        for statement in node_children(compound):
            base = unwrap(statement)
            if base.get("kind") == "CallExpr":
                qtype = base.get("type", {}).get("qualType", "")
                if qtype and qtype != "void":
                    yield base
            elif statement.get("kind") == "CStyleCastExpr":
                qtype = statement.get("type", {}).get("qualType", "")
                calls = [x for x in walk(statement) if x.get("kind") == "CallExpr"]
                if qtype == "void":
                    for call in calls:
                        ctype = call.get("type", {}).get("qualType", "")
                        if ctype and ctype != "void":
                            yield call

def pointer_finding(qtype: str) -> bool:
    compact = qtype.replace(" ", "")
    if compact.count("*") >= 2:
        return True
    return bool(re.search(r"\(\*[^)]*\)\(", compact))

def macro_expanded(node: dict, path: Path) -> bool:
    begin = node.get("range", {}).get("begin", {})
    spelling = begin.get("spellingLoc")
    expansion = begin.get("expansionLoc")
    if not isinstance(spelling, dict) or not isinstance(expansion, dict):
        return False
    spell_file = spelling.get("file")
    if not spell_file:
        return True
    try:
        return Path(spell_file).resolve() != path.resolve()
    except OSError:
        return True

def preprocess_findings(path: Path, policy: dict) -> list[dict]:
    rel = relpath(path)
    text = clean_source(path.read_text(errors="replace"))
    lines = text.splitlines()
    findings = []
    guard_name = None
    if path.suffix == ".h":
        for line in lines[:8]:
            m = re.match(r"\s*#\s*ifndef\s+([A-Za-z_][A-Za-z0-9_]*)", line)
            if m:
                guard_name = m.group(1)
                break
    assertion_pattern = re.compile(
        r"\b(assert|KASSERT|KASSERT_EQ|NX_ASSERT|SCHED_ASSERT)\s*\(|"
        r"\b(_Static_assert|static_assert)\b"
    )
    for number, line in enumerate(lines, 1):
        m = assertion_pattern.search(line)
        if m:
            findings.append(finding("R5", rel, number, "", "assertion construct is forbidden"))
        stripped = line.strip()
        if not stripped.startswith("#"):
            continue
        if re.match(r"#\s*(include|pragma)\b", stripped):
            continue
        mdef = re.match(
            r"#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)(\([^)]*\))?\s*(.*)$",
            stripped)
        if mdef:
            name = mdef.group(1)
            function_like = mdef.group(2) is not None
            replacement = mdef.group(3) or ""
            token_transform = "##" in replacement or bool(
                re.search(r"(^|[^#])#\s*[A-Za-z_]", replacement))
            if name == guard_name or (not function_like and not token_transform):
                continue
            findings.append(finding("R8", rel, number, name,
                                    "behavioral/function-like or token-transforming macro"))
            continue
        mifndef = re.match(r"#\s*ifndef\s+([A-Za-z_][A-Za-z0-9_]*)", stripped)
        if mifndef and mifndef.group(1) == guard_name:
            continue
        if re.match(r"#\s*endif\b", stripped):
            continue
        findings.append(finding("R8", rel, number, "",
                                "conditional/complex preprocessing in first-party source"))
    return findings

def analyze_translation_unit(path: Path, arch: str, policy: dict):
    rel = relpath(path)
    data = path.read_bytes()
    findings = []
    edges = []
    definitions = []
    ast, error = clang_ast(path, arch)
    if ast is None:
        findings.append(finding("R10", rel, 0, "", f"clang AST parse failed: {error[:900]}", arch))
        return findings, edges, definitions
    functions = source_definitions(ast, path)
    allowed_fp = any(rel.startswith(prefix) for prefix in policy["allowed_function_pointer_prefixes"])
    for node in ast.get("inner", ()):
        if not isinstance(node, dict) or node.get("kind") != "VarDecl":
            continue
        loc_line = line_for(node, data)
        if loc_line == 0 or node.get("storageClass") == "extern":
            continue
        qtype = node.get("type", {}).get("qualType", "")
        if "const" not in qtype:
            findings.append(finding("R6", rel, loc_line, node.get("name", ""),
                                    "mutable translation-unit/global state requires explicit ownership",
                                    arch))
    for function in functions:
        name = function.get("name", "<unnamed>")
        line = line_for(function, data)
        begin, end = source_span(function, data)
        definitions.append((name, line))
        if begin == 0 or end == 0:
            findings.append(finding("R4", rel, line, name, "function source span is not provable", arch))
        elif end - begin + 1 > int(policy["function_max_lines"]):
            findings.append(finding("R4", rel, begin, name,
                                    f"function has {end-begin+1} physical lines; max={policy['function_max_lines']}",
                                    arch))
        for part in walk(function):
            if macro_expanded(part, path):
                continue
            kind = part.get("kind")
            pline = line_for(part, data) or line
            if kind in ("GotoStmt", "IndirectGotoStmt", "BreakStmt"):
                findings.append(finding("R1", rel, pline, name,
                                        "break/goto/indirect goto is forbidden", arch))
            if kind in LOOPS and not loop_proved(part):
                findings.append(finding("R2", rel, pline, name,
                                        "loop upper bound/progress is not machine-proved", arch))
            if kind == "StaticAssertDecl":
                findings.append(finding("R5", rel, pline, name, "static assertion is forbidden", arch))
            if kind in ("ParmVarDecl", "VarDecl", "FieldDecl"):
                qtype = part.get("type", {}).get("qualType", "")
                if pointer_finding(qtype) and not allowed_fp:
                    findings.append(finding("R9", rel, pline, name,
                                            f"pointer depth/function pointer outside provider boundary: {qtype}",
                                            arch))
            if kind == "CallExpr":
                called = call_name(part)
                if not called:
                    continue
                edges.append((name, called, line))
                if called in policy["forbidden_jump_calls"]:
                    findings.append(finding("R1", rel, pline, name,
                                            f"forbidden nonlocal jump call: {called}", arch))
                if called in policy["general_allocators"]:
                    findings.append(finding("R3", rel, pline, name,
                                            f"runtime allocation/free requires initialization proof: {called}",
                                            arch))
                if called in policy["assertion_calls"]:
                    findings.append(finding("R5", rel, pline, name,
                                            f"assertion call is forbidden: {called}", arch))
        seen = set()
        for call in direct_discarded_calls(function):
            if macro_expanded(call, path):
                continue
            pline = line_for(call, data) or line
            called = call_name(call) or "<indirect>"
            key = (pline, called)
            if key in seen:
                continue
            seen.add(key)
            findings.append(finding("R7", rel, pline, name,
                                    f"non-void result discarded: {called}", arch))
    return findings, edges, definitions

def recursion_findings(all_edges, all_defs):
    name_sites = defaultdict(list)
    for rel, arch, name, line in all_defs:
        name_sites[name].append((rel, arch, line))
    graph = defaultdict(set)
    for rel, arch, caller, callee, line in all_edges:
        if callee == caller:
            graph[(rel, arch, caller)].add((rel, arch, caller))
            continue
        local = [x for x in all_defs if x[0] == rel and x[1] == arch and x[2] == callee]
        if local:
            graph[(rel, arch, caller)].add((rel, arch, callee))
        elif len(name_sites.get(callee, ())) == 1:
            target = name_sites[callee][0]
            graph[(rel, arch, caller)].add((target[0], target[1], callee))
    findings = []
    visiting, visited = set(), set()
    stack = []
    def visit(node):
        if node in visiting:
            if node in stack:
                cycle = stack[stack.index(node):] + [node]
                rel, arch, name = node
                line = next((x[3] for x in all_defs
                             if x[0] == rel and x[1] == arch and x[2] == name), 0)
                findings.append(finding("R1", rel, line, name,
                                        "recursive call cycle: " +
                                        " -> ".join(x[2] for x in cycle), arch))
            return
        if node in visited:
            return
        visiting.add(node)
        stack.append(node)
        for target in graph.get(node, ()):
            visit(target)
        stack.pop()
        visiting.remove(node)
        visited.add(node)
    for node in list(graph):
        visit(node)
    unique = {(x["rule"], x["file"], x["line"], x["symbol"], x["arch"], x["detail"]): x
              for x in findings}
    return list(unique.values())

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--json", type=Path)
    parser.add_argument("--max-print", type=int, default=120)
    args = parser.parse_args()
    policy = load_policy()
    files = owned_files(policy, (".c", ".h"))
    findings = []
    for path in files:
        findings.extend(preprocess_findings(path, policy))
    all_edges = []
    all_defs = []
    c_files = [x for x in files if x.suffix == ".c"]
    for index, path in enumerate(c_files, 1):
        rel = relpath(path)
        for arch in source_arches(rel, policy):
            current, edges, defs = analyze_translation_unit(path, arch, policy)
            findings.extend(current)
            all_edges.extend((rel, arch, a, b, line) for a, b, line in edges)
            all_defs.extend((rel, arch, name, line) for name, line in defs)
    findings.extend(recursion_findings(all_edges, all_defs))
    unique = {(x["rule"], x["file"], x["line"], x["symbol"], x["arch"], x["detail"]): x
              for x in findings}
    findings = sorted(unique.values(),
                      key=lambda x: (x["rule"], x["file"], x["line"], x["arch"], x["detail"]))
    summary = defaultdict(int)
    for item in findings:
        summary[item["rule"]] += 1
    report = {
        "profile": policy["profile"],
        "owned_files": len(files), "translation_units": len(c_files),
        "findings": findings, "summary": dict(sorted(summary.items())),
    }
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(report, indent=2) + "\n")
    print(f"Power12 AST: files={len(files)} C={len(c_files)} findings={len(findings)}")
    print("summary " + " ".join(f"{k}={v}" for k, v in sorted(summary.items())))
    for item in findings[:args.max_print]:
        print(f"FAIL {item['rule']} {item['arch']} {item['file']}:{item['line']} "
              f"{item['symbol']} — {item['detail']}")
    if len(findings) > args.max_print:
        print(f"... {len(findings)-args.max_print} additional findings in JSON report")
    return 1 if findings else 0

if __name__ == "__main__":
    raise SystemExit(main())
