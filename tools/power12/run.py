#!/usr/bin/env python3
"""Run the complete NEXS Power12 gate and retain machine-readable evidence."""
from __future__ import annotations
import argparse
import json
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from common import ROOT, load_policy

AUDITS = ("audit_build.py", "audit_limits.py", "audit_ast.py")

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--max-print", type=int, default=80)
    parser.add_argument("--report-dir", type=Path,
                        default=ROOT / "build" / "reports" / "power12")
    args = parser.parse_args()
    policy = load_policy()
    args.report_dir.mkdir(parents=True, exist_ok=True)
    tests = subprocess.run(
        [sys.executable, str(Path(__file__).with_name("test_auditors.py"))],
        cwd=ROOT, text=True, capture_output=True, check=False)
    sys.stdout.write(tests.stdout)
    sys.stderr.write(tests.stderr)
    statuses = {"mutation_tests": tests.returncode}
    for script in AUDITS:
        report = args.report_dir / script.replace(".py", ".json")
        run = subprocess.run(
            [sys.executable, str(Path(__file__).with_name(script)),
             "--json", str(report), "--max-print", str(args.max_print)],
            cwd=ROOT, text=True, capture_output=True, check=False)
        statuses[script] = run.returncode
        sys.stdout.write(run.stdout)
        sys.stderr.write(run.stderr)
    manifest = {
        "profile": policy["profile"],
        "generated_utc": datetime.now(timezone.utc).isoformat(),
        "statuses": statuses,
    }
    (args.report_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n")
    failed = [name for name, code in statuses.items() if code != 0]
    if failed:
        print("Power12 gate: FAIL — " + ", ".join(failed))
        return 1
    print("Power12 gate: PASS")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
