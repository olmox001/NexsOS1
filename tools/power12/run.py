#!/usr/bin/env python3
"""Run the complete NEXS Power12 gate and retain machine-readable evidence."""
from __future__ import annotations
import argparse
import hashlib
import json
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from common import ROOT, load_policy, owned_tree_files

AUDITS = ("audit_build.py", "audit_limits.py", "audit_ast.py")

def source_digest(policy: dict) -> str:
    """Hash actual audited inputs; never trust mtimes alone or ignore staged changes."""
    paths = owned_tree_files(policy)
    paths.extend(ROOT / name for name in (
        "Makefile", "kernel/Makefile", "kernel/HAL/Makefile",
        "tools/power12/policy.json",
    ))
    paths.extend(Path(__file__).parent.glob("*.py"))
    digest = hashlib.sha256()
    for path in sorted(set(paths)):
        if not path.is_file():
            continue
        digest.update(path.relative_to(ROOT).as_posix().encode())
        digest.update(b"\0")
        digest.update(hashlib.sha256(path.read_bytes()).digest())
    return digest.hexdigest()

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--incremental", action="store_true",
                        help="reuse complete audited evidence only when every owned input matches")
    parser.add_argument("--warn-only", action="store_true",
                        help="report real findings without blocking maintenance builds")
    parser.add_argument("--max-print", type=int, default=80)
    parser.add_argument("--report-dir", type=Path,
                        default=ROOT / "build" / "reports" / "power12")
    args = parser.parse_args()
    policy = load_policy()
    args.report_dir.mkdir(parents=True, exist_ok=True)
    digest = source_digest(policy)
    manifest_path = args.report_dir / "manifest.json"
    if args.incremental and manifest_path.is_file():
        try:
            cached = json.loads(manifest_path.read_text())
        except (OSError, ValueError):
            cached = {}
        required_reports = [
            args.report_dir / script.replace(".py", ".json") for script in AUDITS
        ]
        if (cached.get("source_digest") == digest and
                all(path.is_file() for path in required_reports) and
                set(cached.get("statuses", {})) ==
                {"mutation_tests", *AUDITS}):
            findings = [k for k, value in cached["statuses"].items()
                        if value != 0]
            print("Power12 evidence cache: verified input digest; audit results unchanged")
            if findings:
                print("WARNING: Power12 NOT VERIFIED — retained findings: "
                      + ", ".join(findings))
            if findings and not args.warn_only:
                return 1
            return 0
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
        "source_digest": digest,
        "mode": "maintenance-warning" if args.warn_only else "qualification-strict",
        "verified": not any(code != 0 for code in statuses.values()),
        "generated_utc": datetime.now(timezone.utc).isoformat(),
        "statuses": statuses,
    }
    (args.report_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n")
    failed = [name for name, code in statuses.items() if code != 0]
    if failed:
        if args.warn_only:
            print("WARNING: Power12 NOT VERIFIED; non-blocking maintenance findings: "
                  + ", ".join(failed))
            print("WARNING: this development build is NOT Power12-qualified; "
                  "strict qualification still fails.")
            return 0
        print("Power12 gate: FAIL — " + ", ".join(failed))
        return 1
    print("Power12 gate: PASS")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
