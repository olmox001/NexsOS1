#!/usr/bin/env python3
"""Generate independent NX-CORE and NX-SDK TIME headers from the wire spec."""
import argparse
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = ROOT / "kernel/contracts/time-v1.json"
TARGETS = (
    ROOT / "kernel/include/abi/os1_time.h",
    ROOT / "system/include/contract/os1_time.h",
)

def render(spec):
    lines = [
        "/* Generated from kernel/contracts/time-v1.json. Do not edit. */",
        "#ifndef OS1_TIME_ABI_H",
        "#define OS1_TIME_ABI_H",
        "",
        "#include <stdint.h>",
        "",
    ]
    for key, value in spec["constants"].items():
        lines.append(f"#define {key} {value}")
    lines.append("")
    for item in spec["structs"]:
        lines.append(f"struct {item['name']} {{")
        for field in item["fields"]:
            lines.append(f"  {field['decl']};")
        lines.append("};")
        lines.append("")
    lines.extend(["#endif /* OS1_TIME_ABI_H */", ""])
    return "\n".join(lines)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    content = render(json.loads(SPEC.read_text()))
    stale = []
    for path in TARGETS:
        if args.check:
            if not path.exists() or path.read_text() != content:
                stale.append(str(path.relative_to(ROOT)))
        else:
            path.write_text(content)
    if stale:
        raise SystemExit("stale generated TIME contract: " + ", ".join(stale))

if __name__ == "__main__":
    main()
