#!/usr/bin/env python3
"""
Verifica #define duplicati - Versione Personalizzata
"""

import os
import re
import sys
from collections import defaultdict
from datetime import datetime

# ================== CONFIGURAZIONE ==================
EXCLUDE_DIRS = {'scratch', 'tools', 'build', 'CMakeFiles', '.git'}

DEFINE_PATTERN = re.compile(
    r'^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)\s*(.*?)(?:\s*//.*)?$', 
    re.MULTILINE
)

# Pattern per rilevare direttive condizionali
IFDEF_PATTERN = re.compile(r'^\s*#\s*(?:if|ifdef|ifndef|elif|else|endif)', re.MULTILINE)

def should_skip_path(path: str) -> bool:
    return any(ex in path.lower() for ex in EXCLUDE_DIRS)

def normalize_value(value: str) -> str:
    """Normalizzazione aggressiva + rimozione commenti"""
    if not value:
        return ""
    
    # Rimuovi commenti C++ e C
    value = re.sub(r'//.*$', '', value)
    value = re.sub(r'/\*.*?\*/', '', value, flags=re.DOTALL)
    
    # Normalizza spazi e hex
    value = ' '.join(value.strip().split())
    value = re.sub(r'0x([0-9a-fA-F]+)', lambda m: '0x' + m.group(1).lower(), value)
    return value


def is_amd64_related(filepath: str) -> bool:
    return 'amd64' in filepath.lower() or 'x86_64' in filepath.lower()


def find_duplicate_defines(root_dir: str):
    defines = defaultdict(lambda: defaultdict(list))
    report_lines = []

    report_lines.append("=== REPORT #define DUPLICATI (v3 - Personalizzato) ===\n")
    report_lines.append(f"Progetto: {os.path.abspath(root_dir)}")
    report_lines.append(f"Data: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    report_lines.append("Escluse cartelle: scratch, tools, build\n")

    for dirpath, _, filenames in os.walk(root_dir):
        if should_skip_path(dirpath):
            continue

        for filename in filenames:
            if not filename.endswith(('.c', '.h', '.cpp', '.hpp')):
                continue

            filepath = os.path.join(dirpath, filename)
            try:
                with open(filepath, 'r', encoding='utf-8', errors='ignore') as f:
                    content = f.read()

                # Trova tutte le define
                for match in DEFINE_PATTERN.finditer(content):
                    name = match.group(1)
                    value = match.group(2) or ""
                    norm_value = normalize_value(value)
                    line_num = content[:match.start()].count('\n') + 1

                    defines[name][norm_value].append((filepath, line_num, value.strip()))

            except Exception as e:
                print(f"Errore lettura {filepath}: {e}")

    # ===================== REPORT =====================
    conflicts = 0
    duplicates = 0

    for name in sorted(defines.keys()):
        value_dict = defines[name]
        
        if len(value_dict) > 1:
            # === CONFLITTO (valori diversi) ===
            conflicts += 1
            amd64_flag = any(is_amd64_related(f) for locs in value_dict.values() for f,_,_ in locs)
            prefix = "❌ [AMD64] CONFLITTO" if amd64_flag else "❌ CONFLITTO"
            
            report_lines.append(f"{prefix}: {name} ({len(value_dict)} valori diversi)")
            
            for val, locations in value_dict.items():
                report_lines.append(f"   Valore: '{val if val else '<empty>'}'")
                for file, line, _ in locations:
                    report_lines.append(f"      → {file}:{line}")
            report_lines.append("")

        elif len(next(iter(value_dict.values()))) > 1:
            # === DUPLICATO (stesso valore) ===
            locations = next(iter(value_dict.values()))
            if len(locations) > 1:
                duplicates += 1
                report_lines.append(f"⚠️  DUPLICATO: {name} definito {len(locations)} volte")
                for file, line, _ in locations:
                    report_lines.append(f"      → {file}:{line}")
                report_lines.append("")

    report_lines.append("="*70)
    report_lines.append(f"RIEPILOGO: {conflicts} CONFLITTI | {duplicates} DUPLICATI")
    report_lines.append("="*70)

    # Salva report
    report_text = "\n".join(report_lines)
    print(report_text)

    with open("report_define.txt", "w", encoding="utf-8") as f:
        f.write(report_text)

    print(f"\n📄 Report salvato correttamente in: {os.path.abspath('report_define.txt')}")


if __name__ == "__main__":
    project_path = sys.argv[1] if len(sys.argv) > 1 else "."
    if not os.path.isdir(project_path):
        print("❌ Percorso non valido!")
        sys.exit(1)
    
    find_duplicate_defines(project_path)