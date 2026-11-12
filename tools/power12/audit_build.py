#!/usr/bin/env python3
"""Build-policy audit for NEXS Power12 R7/R10 and source hygiene."""
from __future__ import annotations
import argparse
import json
import re
from pathlib import Path
from common import ROOT, finding, load_policy, owned_files, owned_tree_files, relpath

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--json", type=Path)
    parser.add_argument("--max-print", type=int, default=120)
    args = parser.parse_args()
    policy = load_policy()
    findings = []
    makefile = ROOT / "Makefile"
    lines = makefile.read_text(errors="replace").splitlines()

    for number, line in enumerate(lines, 1):
        if "BYPASS_USERLAND_ERRORS" in line or "BYPASS_SEC" in line:
            findings.append(finding("R10", "Makefile", number, "",
                                    "first-party warning bypass switch is forbidden"))
        if "NX_DIAG_WARNING_MODE" in line and "-Wno-error" in line:
            findings.append(finding("R10", "Makefile", number, "",
                                    "conversion diagnostics may not be demoted"))
        if "-Wno-error" in line:
            first_party_compile = any(token in line for token in (
                "$(USER_CFLAGS) -Wno-error",
                "BYPASS_USERLAND_ERRORS",
                "NX_DIAG_WARNING_MODE",
            ))
            if first_party_compile:
                findings.append(finding("R10", "Makefile", number, "",
                                        "first-party compilation may not use -Wno-error"))
        if "power12-nxpax" in line:
            findings.append(finding("R10", "Makefile", number, "",
                                    "component-only Power12 gate is not the project gate"))

    common_text = "\n".join(lines[:170])
    for flag in ("-Wconversion", "-Wsign-conversion"):
        if flag not in common_text:
            findings.append(finding("R10", "Makefile", 0, "",
                                    f"mandatory first-party warning flag missing from common profile: {flag}"))

    for path in owned_files(policy, (".c", ".h", ".S", ".s")):
        rel = relpath(path)
        text = path.read_text(errors="replace")
        for number, line in enumerate(text.splitlines(), 1):
            if "NX_DISCARD(" in line:
                findings.append(finding("R7", rel, number, "NX_DISCARD",
                                        "fallible-result discard mechanism is forbidden"))
    owned_tree = owned_tree_files(policy)
    for path in owned_tree:
        if path.suffix != ".mk":
            continue
        rel = relpath(path)
        for number, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
            if "-Wno-error" in line:
                findings.append(finding("R10", rel, number, "",
                                        "first-party build fragment demotes warnings"))

    for path in owned_tree:
        rel = relpath(path)
        if any(path.name.endswith(suffix) for suffix in policy["stale_suffixes"]):
            findings.append(finding("R10", rel, 0, "",
                                    "stale alternate source artifact is forbidden"))

    unique = {(x["rule"], x["file"], x["line"], x["detail"]): x for x in findings}
    findings = sorted(unique.values(), key=lambda x:(x["rule"],x["file"],x["line"],x["detail"]))
    report = {"profile": policy["profile"], "findings": findings}
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(report, indent=2)+"\n")
    print(f"Power12 build policy: findings={len(findings)}")
    for item in findings[:args.max_print]:
        print(f"FAIL {item['rule']} {item['file']}:{item['line']} — {item['detail']}")
    if len(findings) > args.max_print:
        print(f"... {len(findings)-args.max_print} additional findings in JSON report")
    return 1 if findings else 0

if __name__ == "__main__":
    raise SystemExit(main())
