#!/usr/bin/env python3
"""Machine-readable provenance audit for NEXS Power12 R11/R12."""
from __future__ import annotations
import argparse
import json
import re
from pathlib import Path
from common import finding, load_policy, owned_files, relpath

LIMIT_NAME = re.compile(
    r"(?:^|_)(?:MAX|MIN|LIMIT|CAPACITY|RETRIES|RETRY|SLOTS|POOL|BUDGET|"
    r"QUANTUM|DEPTH|TIMEOUT|INTERVAL|HZ|PAGES|BYTES)(?:_|$)"
)
OP_NAME = re.compile(
    r"(?:^|_)(?:OP|CTL|REQUEST|EVENT|IPC_TYPE|OBJ_TYPE|RIGHT|NS)(?:_|$)"
)
DEFINE = re.compile(r"^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)\s+(.+?)\s*$")

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--json", type=Path)
    parser.add_argument("--max-print", type=int, default=120)
    args = parser.parse_args()
    policy = load_policy()
    declared_limits = policy.get("limits", {})
    declared_ops = policy.get("operations", {})
    findings = []
    for path in owned_files(policy, (".c", ".h")):
        rel = relpath(path)
        for number, raw in enumerate(path.read_text(errors="replace").splitlines(), 1):
            match = DEFINE.match(raw)
            if not match:
                continue
            name, value = match.groups()
            key = f"{rel}::{name}"
            if LIMIT_NAME.search(name):
                entry = declared_limits.get(key)
                if not isinstance(entry, dict):
                    findings.append(finding("R12", rel, number, name,
                                            f"finite/material limit lacks policy provenance: {value}"))
                else:
                    required = ("kind", "provenance", "rationale")
                    missing = [field for field in required if not entry.get(field)]
                    if missing:
                        findings.append(finding("R12", rel, number, name,
                                                "limit provenance missing fields: "+",".join(missing)))
            if OP_NAME.search(name):
                entry = declared_ops.get(key)
                if not isinstance(entry, dict):
                    findings.append(finding("R11", rel, number, name,
                                            f"material operation/authority ID is not catalogued: {value}"))
                else:
                    required = ("id", "owner", "work_bound")
                    missing = [field for field in required if entry.get(field) in (None, "")]
                    if missing:
                        findings.append(finding("R11", rel, number, name,
                                                "operation catalogue missing fields: "+",".join(missing)))
    findings.sort(key=lambda x:(x["rule"],x["file"],x["line"],x["symbol"]))
    report={"profile":policy["profile"],"findings":findings}
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(report,indent=2)+"\n")
    print(f"Power12 ID/limit provenance: findings={len(findings)}")
    for item in findings[:args.max_print]:
        print(f"FAIL {item['rule']} {item['file']}:{item['line']} {item['symbol']} — {item['detail']}")
    if len(findings)>args.max_print:
        print(f"... {len(findings)-args.max_print} additional findings in JSON report")
    return 1 if findings else 0

if __name__ == "__main__":
    raise SystemExit(main())
