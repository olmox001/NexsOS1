#!/usr/bin/env python3
# ─────────────────────────────────────────────────────────────────────────────
# kernel_doctor.py — Analisi e refactor automatico di kernel C
# ─────────────────────────────────────────────────────────────────────────────
# Fasi:
#   1. ANALYZE   — grafo include + ownership #define
#   2. RESOLVE   — elimina duplicati / wrappa conflitti arch
#   3. TIDY      — clang-tidy (cleanup, bugprone, readability)
#   4. COCCI     — Coccinelle semantic patches (MMIO, lock, null-check…)
#   5. REPORT    — report testuale finale
#
# Uso:
#   python3 kernel_doctor.py --all    /path/to/kernel   # tutto
#   python3 kernel_doctor.py --analyze /path/to/kernel
#   python3 kernel_doctor.py --resolve /path/to/kernel [--dry-run]
#   python3 kernel_doctor.py --tidy   /path/to/kernel
#   python3 kernel_doctor.py --cocci  /path/to/kernel
#   python3 kernel_doctor.py --report /path/to/kernel
#
# Dipendenze (installa con install_deps.sh):
#   brew install llvm coccinelle bear
# ─────────────────────────────────────────────────────────────────────────────

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from collections import defaultdict
from datetime import datetime
from pathlib import Path
from typing import Optional

# ══════════════════════════════════════════════════════════════════════════════
# 0. CLI & CONFIG
# ══════════════════════════════════════════════════════════════════════════════

VERSION = "2.0.0"

def parse_args():
    p = argparse.ArgumentParser(
        prog='kernel_doctor',
        description='Analisi e refactor automatico per kernel C (macOS)',
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument('root', help='Radice del progetto kernel')
    p.add_argument('--all',     action='store_true', help='Esegui tutte le fasi')
    p.add_argument('--analyze', action='store_true', help='Fase 1: analisi struttura')
    p.add_argument('--resolve', action='store_true', help='Fase 2: risoluzione define')
    p.add_argument('--tidy',    action='store_true', help='Fase 3: clang-tidy')
    p.add_argument('--cocci',   action='store_true', help='Fase 4: Coccinelle patches')
    p.add_argument('--report',  action='store_true', help='Fase 5: genera report')
    p.add_argument('--apply',   action='store_true', help='Applica modifiche (Fase 4)')
    p.add_argument('--dry-run', action='store_true', help='Mostra azioni senza modificare')
    p.add_argument('--no-backup', action='store_true', help='Non creare .bak')
    p.add_argument('--out-dir', default=None,
                   help='Directory output (default: <root>/kernel_doctor_out)')
    p.add_argument('--version', action='version', version=f'kernel_doctor {VERSION}')
    return p.parse_args()

EXCLUDE_DIRS = {'scratch', 'tools', 'build', 'CMakeFiles', '.git', '__pycache__',
                'kernel_doctor_out'}
EXTENSIONS   = {'.c', '.h', '.cpp', '.hpp'}

# ══════════════════════════════════════════════════════════════════════════════
# 1. UTILITIES
# ══════════════════════════════════════════════════════════════════════════════

# ── colori ANSI ───────────────────────────────────────────────────────────────
BOLD = '\033[1m';  RST  = '\033[0m'
RED  = '\033[91m'; GRN  = '\033[92m'; YLW = '\033[93m'
BLU  = '\033[94m'; MAG  = '\033[95m'; CYN = '\033[96m'

def c(color, text): return f'{color}{text}{RST}'
def banner(title, color=BLU):
    w = 70
    bar = '═' * w
    print(f'\n{color}{BOLD}{bar}{RST}')
    print(f'{color}{BOLD}  {title}{RST}')
    print(f'{color}{BOLD}{bar}{RST}')

def ok(msg):   print(f'  {c(GRN,"✓")} {msg}')
def warn(msg): print(f'  {c(YLW,"⚠")} {msg}')
def err(msg):  print(f'  {c(RED,"✗")} {msg}')
def info(msg): print(f'  {c(CYN,"→")} {msg}')
def skip(msg): print(f'  {c(MAG,"·")} {msg}')

# ── I/O file ──────────────────────────────────────────────────────────────────
def read_file(path: str) -> str:
    try:
        return open(path, encoding='utf-8', errors='ignore').read()
    except Exception:
        return ''

def write_file(path: str, content: str, dry: bool, backup: bool):
    if dry:
        info(f'[DRY] → {path}')
        return False
    if backup and os.path.exists(path):
        shutil.copy2(path, path + '.bak')
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, 'w', encoding='utf-8') as f:
        f.write(content)
    return True

def rel(path: str, root: str) -> str:
    return str(Path(os.path.relpath(path, root))).replace('\\', '/')

def absp(rel_path: str, root: str) -> str:
    return os.path.normpath(os.path.join(root, rel_path.lstrip('./')))

# ── tool detection macOS ──────────────────────────────────────────────────────
def find_tool(names: list[str]) -> Optional[str]:
    """Cerca tool in PATH e nei prefix brew comuni."""
    brew_prefixes = [
        '/opt/homebrew/opt/llvm/bin',   # Apple Silicon
        '/usr/local/opt/llvm/bin',      # Intel
        '/opt/homebrew/bin',
        '/usr/local/bin',
    ]
    for name in names:
        if (p := shutil.which(name)):
            return p
        for prefix in brew_prefixes:
            candidate = os.path.join(prefix, name)
            if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
                return candidate
    return None

def run(cmd: list[str], cwd: str = '.', capture: bool = True,
        timeout: int = 120) -> tuple[int, str, str]:
    try:
        r = subprocess.run(cmd, cwd=cwd, capture_output=capture,
                           text=True, timeout=timeout)
        return r.returncode, r.stdout or '', r.stderr or ''
    except subprocess.TimeoutExpired:
        return -1, '', 'TIMEOUT'
    except FileNotFoundError:
        return -2, '', f'Tool not found: {cmd[0]}'

# ══════════════════════════════════════════════════════════════════════════════
# 2. FASE 1 — ANALISI STRUTTURA
# ══════════════════════════════════════════════════════════════════════════════

INCLUDE_RE = re.compile(
    r'^\s*#\s*include\s+(?:"([^"]+)"|<([^>]+)>)', re.MULTILINE)
DEFINE_RE  = re.compile(
    r'^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)\b', re.MULTILINE)

def collect_files(root: str) -> list[str]:
    files = []
    for dp, dirnames, fns in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in EXCLUDE_DIRS]
        rel_dp = os.path.relpath(dp, root)
        if any(ex in rel_dp.split(os.sep) for ex in EXCLUDE_DIRS):
            continue
        for fn in fns:
            if Path(fn).suffix in EXTENSIONS:
                files.append(
                    str(Path(os.path.relpath(os.path.join(dp, fn), root))
                        ).replace('\\', '/'))
    return sorted(files)

def arch_of(fp: str) -> str:
    f = fp.lower()
    if 'amd64' in f or 'x86_64' in f: return 'amd64'
    if 'aarch64' in f or 'arm64' in f: return 'aarch64'
    if 'riscv' in f:                   return 'riscv'
    return 'generic'

def parse_includes(abs_path: str, root: str, files_set: set) -> list[str]:
    content = read_file(abs_path)
    rel_dir = os.path.dirname(abs_path)
    found, seen = [], set()
    search_dirs = [
        '',
        'include', 'include/api',
        'kernel/include', 'kernel/include/kernel',
        'kernel/include/drivers',
    ]
    for m in INCLUDE_RE.finditer(content):
        inc = m.group(1) or m.group(2)
        if not inc: continue
        candidates = [os.path.normpath(os.path.join(rel_dir, inc))]
        for sd in search_dirs:
            candidates.append(os.path.normpath(os.path.join(root, sd, inc)))
        for cand in candidates:
            r = str(Path(os.path.relpath(cand, root))).replace('\\', '/')
            if r in files_set and r not in seen:
                found.append(r); seen.add(r); break
    return found

def parse_defines(abs_path: str) -> list[dict]:
    content = read_file(abs_path)
    lines = content.split('\n')
    depth, line_depth = 0, []
    for ln in lines:
        s = ln.strip()
        line_depth.append(depth)
        if re.match(r'#\s*(?:ifdef|ifndef|if)\b', s): depth += 1
        elif re.match(r'#\s*endif\b', s):             depth = max(0, depth-1)
    results = []
    for m in DEFINE_RE.finditer(content):
        lno    = content[:m.start()].count('\n')
        in_cond = line_depth[lno] > 0 if lno < len(line_depth) else False
        rest   = content[m.end():].split('\n')[0]
        value  = re.sub(r'\s*//.*$', '', rest).strip()
        results.append({'name': m.group(1), 'line': lno+1,
                        'value': value, 'in_conditional': in_cond})
    return results

def build_transitive_included_by(graph: dict) -> dict:
    included_by = defaultdict(set)
    for src, incs in graph.items():
        for inc in incs: included_by[inc].add(src)
    all_nodes = set(graph) | {n for lst in graph.values() for n in lst}
    trans = {n: set(included_by[n]) for n in all_nodes}
    changed = True
    while changed:
        changed = False
        for node in all_nodes:
            for includer in list(trans[node]):
                before = len(trans[node])
                trans[node] |= trans.get(includer, set())
                if len(trans[node]) != before: changed = True
    return trans

def choose_canonical(locs: list, tib: dict) -> str:
    def score(loc):
        fp = loc['file']
        return (-len(tib.get(fp, set())), fp.count('/'),
                1 if fp.endswith('.c') else 0, fp)
    return min(locs, key=score)['file']

def categorize_conflict(name: str, vgroups: dict) -> str:
    intentional = {'KERNEL_NAME','STACK_SIZE','WIN_W','WIN_H','BUFFER_SIZE','MAX_VIRTIO_DEVS'}
    if name in intentional:                                  return 'INTENTIONAL'

    all_files = [loc['file'] for locs in vgroups.values() for loc in locs]
    files_set = set(all_files)
    if len(files_set) == 1:                                  return 'VMM_ARCH_INLINE'
    archs = {arch_of(f) for f in files_set}
    if 'generic' not in archs and len(archs) > 1:           return 'ARCH_SPECIFIC'
    return 'NEEDS_REVIEW'

def norm_val(v: str) -> str:
    v = re.sub(r'//.*$', '', v)
    v = re.sub(r'/\*.*?\*/', '', v, flags=re.DOTALL)
    v = ' '.join(v.strip().split())
    v = re.sub(r'0x([0-9a-fA-F]+)', lambda m: '0x'+m.group(1).lower(), v)
    return v.lower()

def phase_analyze(root: str, out_dir: str) -> dict:
    banner('FASE 1 — Analisi struttura', BLU)

    files = collect_files(root)
    files_set = set(files)
    info(f'{len(files)} file sorgente trovati')

    info('Costruzione grafo include...')
    include_graph = {f: parse_includes(os.path.join(root, f), root, files_set)
                     for f in files}
    tib = build_transitive_included_by(include_graph)

    info('Raccolta #define...')
    define_index: dict[str, dict[str, list]] = defaultdict(lambda: defaultdict(list))
    for f in files:
        for d in parse_defines(os.path.join(root, f)):
            nv = norm_val(d['value'])
            define_index[d['name']][nv].append({
                'file': f, 'line': d['line'], 'value': d['value'],
                'in_conditional': d['in_conditional'], 'arch': arch_of(f),
            })

    info('Classificazione macro...')
    macros = {}
    for name, vgroups in define_index.items():
        total = sum(len(v) for v in vgroups.values())
        if total == 1 and len(vgroups) == 1: continue
        if len(vgroups) == 1:
            locs = next(iter(vgroups.values()))
            macros[name] = {
                'kind': 'DUPLICATE',
                'canonical': choose_canonical(locs, tib),
                'occurrences': locs,
            }
        else:
            ct = categorize_conflict(name, vgroups)
            macros[name] = {
                'kind': 'CONFLICT',
                'conflict_type': ct,
                'value_groups': {k: v for k, v in vgroups.items()},
                'all_files': sorted({loc['file']
                                     for locs in vgroups.values() for loc in locs}),
            }

    data = {
        'meta': {'project': root, 'date': datetime.now().isoformat(),
                 'files': len(files), 'macros': len(macros)},
        'include_graph': include_graph,
        'macros': macros,
    }
    json_path = os.path.join(out_dir, 'kernel_structure.json')
    with open(json_path, 'w', encoding='utf-8') as f:
        json.dump(data, f, indent=2, ensure_ascii=False)

    # riepilogo
    dups = sum(1 for m in macros.values() if m['kind'] == 'DUPLICATE')
    cfls = {ct: sum(1 for m in macros.values()
                    if m['kind']=='CONFLICT' and m.get('conflict_type')==ct)
            for ct in ('ARCH_SPECIFIC','VMM_ARCH_INLINE','INTENTIONAL','NEEDS_REVIEW')}

    ok(f'Duplicati:          {dups}')
    ok(f'Conflitti arch:     {cfls["ARCH_SPECIFIC"]}  (già ok — file separati)')
    ok(f'Conflitti inline:   {cfls["VMM_ARCH_INLINE"]}  (stesso file, da wrappare)')
    ok(f'Conflitti intenz.:  {cfls["INTENTIONAL"]}')
    warn(f'Conflitti da rev.:  {cfls["NEEDS_REVIEW"]}  (revisione manuale)')
    ok(f'Struttura salvata:  {json_path}')

    top10 = sorted(((f, len(tib.get(f,set()))) for f in files),
                   key=lambda x: -x[1])[:10]
    print(f'\n  {BOLD}File più inclusi (top 10):{RST}')
    for fp, cnt in top10:
        print(f'    {cnt:4d}×  {fp}')

    return data

# ══════════════════════════════════════════════════════════════════════════════
# 3. FASE 2 — RISOLUZIONE DEFINE
# ══════════════════════════════════════════════════════════════════════════════

# ── patch helpers ─────────────────────────────────────────────────────────────
def remove_define(content: str, name: str) -> str:
    # Rimuove #define (gestendo multilinee con backslash)
    # Cerchiamo #define name seguita da righe che finiscono con \
    # e infine una riga che non finisce con \.
    pattern = r'^[ \t]*#[ \t]*define[ \t]+' + re.escape(name) + r'\b.*?(?<!\\)\n'
    content = re.sub(pattern, '', content, flags=re.MULTILINE | re.DOTALL)

    # Rimuove typedef (semplice o struct)
    # Cerchiamo typedef ... name; o } name;
    content = re.sub(
        r'^[ \t]*typedef\b.*?\b' + re.escape(name) + r'\b\s*;\s*\n?',
        '', content, flags=re.MULTILINE | re.DOTALL)
    content = re.sub(
        r'\}\s*' + re.escape(name) + r'\s*;\s*\n?',
        '', content, flags=re.MULTILINE)
    return content

def remove_defines(content: str, names: list) -> str:
    for n in names: content = remove_define(content, n)
    return content

def extract_all_definitions(content: str) -> set[str]:
    defs = set()
    # Macros
    defs.update(re.findall(r'^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)\b', content, re.MULTILINE))
    # Typedefs semplici
    defs.update(re.findall(r'typedef\b.*?([A-Za-z_][A-Za-z0-9_]*)\s*;\s*$', content, re.MULTILINE))
    # Typedefs struct/union/enum (fine della definizione)
    defs.update(re.findall(r'\}\s*([A-Za-z_][A-Za-z0-9_]*)\s*;\s*$', content, re.MULTILINE))
    return defs

def add_ifndef(content: str, name: str) -> str:
    if f'#ifndef {name}' in content: return content
    def repl(m):
        return f'#ifndef {name}\n{m.group(0)}\n#endif /* {name} */'
    return re.sub(
        r'^([ \t]*#[ \t]*define[ \t]+' + re.escape(name) +
        r'\b[^\n]*(?:\\\n[^\n]*)*)',
        repl, content, flags=re.MULTILINE)

def ensure_include(content: str, inc: str) -> str:
    if f'"{inc}"' in content or f'<{inc}>' in content: return content
    lasts = list(re.finditer(r'^[ \t]*#[ \t]*include\b[^\n]*', content, re.MULTILINE))
    line  = f'#include "{inc}"'
    if lasts:
        pos = lasts[-1].end()
        return content[:pos] + '\n' + line + content[pos:]
    return line + '\n' + content

def wrap_arch_ifdef(content: str, name: str, aa64: str, x86: str) -> str:
    content = remove_define(content, name)
    block = (f'#if defined(__aarch64__)\n'
             f'#define {name} {aa64}\n'
             f'#elif defined(__x86_64__)\n'
             f'#define {name} {x86}\n'
             f'#endif /* {name} */\n')
    return block + content

# ── mirror pairs ──────────────────────────────────────────────────────────────
MIRROR_PAIRS = [
    ('include/api/posix_types.h',  'kernel/include/kernel/types.h',
     '../../../../include/api/posix_types.h'),
    ('include/api/elf.h',          'kernel/include/kernel/elf.h',
     '../../../../include/api/elf.h'),
    ('include/api/os1.h',          'user/lib/lib.h',
     '../../include/api/os1.h'),
]

def resolve_mirror_pairs(root, macros, actions, dry, backup):
    for api_rel, ker_rel, inc_path in MIRROR_PAIRS:
        api_abs = absp(api_rel, root)
        ker_abs = absp(ker_rel, root)
        if not (os.path.exists(api_abs) and os.path.exists(ker_abs)): continue
        
        api_content = read_file(api_abs)
        api_defs = extract_all_definitions(api_content)
        
        ker_content = read_file(ker_abs)
        ker_defs = extract_all_definitions(ker_content)
        
        # Rimuoviamo tutto ciò che è definito in API ed è presente nel Kernel
        to_remove = sorted(list(api_defs & ker_defs))
        
        if not to_remove: continue
        
        new = remove_defines(ker_content, to_remove)
        new = ensure_include(new, inc_path)
        
        if new != ker_content:
            actions.append({'strategy':'MIRROR_PAIR','file':ker_rel,
                            'count':len(to_remove),'include':inc_path})
            write_file(ker_abs, new, dry, backup)

def resolve_ifndef_guards(root, macros, actions, dry, backup):
    # raggruppa per file per minimizzare riscritture
    file_patches: dict[str, list] = defaultdict(list)
    for name, m in macros.items():
        if m['kind'] != 'DUPLICATE': continue
        for loc in m['occurrences']:
            if loc['file'] != m['canonical'] and not loc['in_conditional']:
                file_patches[loc['file']].append(name)
    for fp, names in file_patches.items():
        abs_fp = absp(fp, root)
        if not os.path.exists(abs_fp): continue
        content = read_file(abs_fp)
        new = content
        for name in names: new = add_ifndef(new, name)
        if new != content:
            actions.append({'strategy':'IFNDEF_GUARD','file':fp,'count':len(names)})
            write_file(abs_fp, new, dry, backup)

def resolve_vmm_inline(root, macros, actions, dry, backup):
    vmm_rel = 'kernel/include/kernel/vmm.h'
    vmm_abs = absp(vmm_rel, root)
    if not os.path.exists(vmm_abs): return
    content = read_file(vmm_abs)
    modified = False
    for name, m in macros.items():
        if m.get('conflict_type') != 'VMM_ARCH_INLINE': continue
        vg = m['value_groups']
        if len(vg) != 2: continue
        if f'defined(__aarch64__)' in content and name in content: continue
        vals = list(vg.items())
        v0, v1 = vals[0][0], vals[1][0]
        # euristica: valore con 'PTE_AP' o 'EL' è aarch64
        if any(x in v0 for x in ('PTE_AP','EL','TTBR')):
            aa64, x86 = v0, v1
        else:
            aa64, x86 = v1, v0
        content = wrap_arch_ifdef(content, name, aa64, x86)
        modified = True
        actions.append({'strategy':'VMM_ARCH_INLINE','file':vmm_rel,'macro':name})
    if modified:
        write_file(vmm_abs, content, dry, backup)

def resolve_local_dups(root, macros, actions, dry, backup):
    """
    Gestisce coppie specifiche: GDT, math, virtio, MB2, PIT, PAGE_*, SECTOR_SIZE.
    """
    # ─ GDT ───────────────────────────────────────────────────────────────────
    GDT_NAMES = ['GDT_KERN_CODE','GDT_KERN_DATA','GDT_USER_CODE','GDT_USER_DATA']
    gdt_c = absp('kernel/arch/amd64/cpu/gdt.c', root)
    msr_c = absp('kernel/arch/amd64/cpu/msr.c', root)
    gdt_h = absp('kernel/arch/amd64/cpu/gdt_defs.h', root)
    if os.path.exists(gdt_c) and os.path.exists(msr_c):
        relevant = [n for n in GDT_NAMES if n in macros]
        if relevant:
            gc = read_file(gdt_c)
            vals = {}
            for n in relevant:
                mm = re.search(r'#\s*define\s+' + re.escape(n) + r'\s+(\S+)', gc)
                if mm: vals[n] = mm.group(1)
            hdr = '/* gdt_defs.h — generato da kernel_doctor */\n#pragma once\n'
            for n in GDT_NAMES:
                hdr += f'#define {n} {vals.get(n,"??")}\n'
            write_file(gdt_h, hdr, dry, backup)
            actions.append({'strategy':'GDT_HEADER','file':'kernel/arch/amd64/cpu/gdt_defs.h'})
            for fp, fc in [(gdt_c, gc), (msr_c, read_file(msr_c))]:
                new = remove_defines(fc, relevant)
                new = ensure_include(new, 'gdt_defs.h')
                if new != fc:
                    actions.append({'strategy':'GDT_PATCH','file':rel(fp,root)})
                    write_file(fp, new, dry, backup)

    # ─ MATH ──────────────────────────────────────────────────────────────────
    MATH_NAMES = ['FP_SHIFT','FP_ONE','FP_HALF','FP_PI']
    math_c = absp('kernel/lib/math.c', root)
    if os.path.exists(math_c):
        content = read_file(math_c)
        new = remove_defines(content, [n for n in MATH_NAMES if n in macros])
        new = ensure_include(new, '../include/kernel/math.h')
        if new != content:
            actions.append({'strategy':'MATH','file':'kernel/lib/math.c'})
            write_file(math_c, new, dry, backup)

    # ─ VIRTIO ────────────────────────────────────────────────────────────────
    VIRT_NAMES = ['VIRTIO_MMIO_BASE','VIRTIO_MMIO_STRIDE','VIRTIO_COUNT',
                  'VRING_DESC_F_NEXT','VRING_DESC_F_WRITE','VRING_DESC_F_INDIRECT']
    virtio_h = absp('kernel/include/drivers/virtio.h', root)
    for vfp in ['kernel/arch/aarch64/virtio.c','kernel/arch/aarch64/hal.c']:
        vabs = absp(vfp, root)
        if not os.path.exists(vabs): continue
        content = read_file(vabs)
        new = remove_defines(content, [n for n in VIRT_NAMES if n in macros])
        inc = os.path.relpath(virtio_h, os.path.dirname(vabs)).replace('\\','/')
        new = ensure_include(new, inc)
        if new != content:
            actions.append({'strategy':'VIRTIO','file':vfp})
            write_file(vabs, new, dry, backup)

    # ─ MULTIBOOT2 ────────────────────────────────────────────────────────────
    MB2_NAMES = ['MB2_TAG_TYPE_END','MB2_TAG_TYPE_MMAP','MB2_TAG_TYPE_BASIC_MEMINFO']
    mb2_h = absp('kernel/include/kernel/multiboot2.h', root)
    platform_c = absp('kernel/arch/amd64/platform/platform.c', root)
    if os.path.exists(mb2_h) and os.path.exists(platform_c):
        content = read_file(platform_c)
        new = remove_defines(content, [n for n in MB2_NAMES if n in macros])
        inc = os.path.relpath(mb2_h, os.path.dirname(platform_c)).replace('\\','/')
        new = ensure_include(new, inc)
        if new != content:
            actions.append({'strategy':'MB2','file':'kernel/arch/amd64/platform/platform.c'})
            write_file(platform_c, new, dry, backup)

    # ─ PIT ───────────────────────────────────────────────────────────────────
    PIT_NAMES = ['PIT_CH0','PIT_CMD']
    apic_c = absp('kernel/arch/amd64/cpu/apic.c', root)
    if os.path.exists(apic_c):
        content = read_file(apic_c)
        new = content
        for name in [n for n in PIT_NAMES if n in macros and macros[n]['kind']=='DUPLICATE']:
            new = add_ifndef(new, name)
        if new != content:
            actions.append({'strategy':'PIT_GUARD','file':'kernel/arch/amd64/cpu/apic.c'})
            write_file(apic_c, new, dry, backup)

    # ─ PAGE_* ────────────────────────────────────────────────────────────────
    PAGE_NAMES = ['PAGE_SIZE','PAGE_SHIFT','PAGE_MASK']
    pmm_h  = absp('kernel/include/kernel/pmm.h', root)
    mmu_c  = absp('kernel/arch/amd64/mm/mmu.c', root)
    if os.path.exists(pmm_h) and os.path.exists(mmu_c):
        content = read_file(mmu_c)
        new = remove_defines(content, [n for n in PAGE_NAMES if n in macros])
        inc = os.path.relpath(pmm_h, os.path.dirname(mmu_c)).replace('\\','/')
        new = ensure_include(new, inc)
        if new != content:
            actions.append({'strategy':'PAGE_DEDUP','file':'kernel/arch/amd64/mm/mmu.c'})
            write_file(mmu_c, new, dry, backup)

    # ─ SECTOR_SIZE ───────────────────────────────────────────────────────────
    buffer_h = absp('kernel/include/kernel/buffer.h', root)
    if 'SECTOR_SIZE' in macros and macros['SECTOR_SIZE']['kind']=='DUPLICATE':
        if os.path.exists(buffer_h):
            content = read_file(buffer_h)
            new = add_ifndef(content, 'SECTOR_SIZE')
            if new != content:
                actions.append({'strategy':'SECTOR_GUARD','file':'kernel/include/kernel/buffer.h'})
                write_file(buffer_h, new, dry, backup)

def phase_resolve(root: str, out_dir: str, data: dict, dry: bool, backup: bool):
    banner('FASE 2 — Risoluzione #define', GRN)
    if dry: warn('DRY-RUN — nessuna modifica reale')

    macros  = data['macros']
    actions = []

    info('[A] Mirror pair (include/api ↔ kernel/include)...')
    resolve_mirror_pairs(root, macros, actions, dry, backup)

    info('[B] #ifndef guard per duplicati residui...')
    resolve_ifndef_guards(root, macros, actions, dry, backup)

    info('[C] VMM arch-inline ifdef wrapping...')
    resolve_vmm_inline(root, macros, actions, dry, backup)

    info('[D–L] Consolidamenti locali (GDT, math, virtio, MB2, PIT, PAGE, sector)...')
    resolve_local_dups(root, macros, actions, dry, backup)

    # riepilogo
    by_strat = defaultdict(list)
    for a in actions: by_strat[a['strategy']].append(a)

    LABELS = {
        'MIRROR_PAIR':'[A] Mirror pair',  'IFNDEF_GUARD':'[B] #ifndef guard',
        'VMM_ARCH_INLINE':'[C] VMM arch', 'GDT_HEADER':'[D] GDT header',
        'GDT_PATCH':'[D] GDT patch',      'MATH':'[E] Math',
        'VIRTIO':'[F] Virtio',            'MB2':'[G] MB2',
        'PIT_GUARD':'[H] PIT guard',      'PAGE_DEDUP':'[I] PAGE dedup',
        'SECTOR_GUARD':'[J] SECTOR guard',
    }
    for strat, acts in sorted(by_strat.items()):
        ok(f'{LABELS.get(strat,strat)}: {len(acts)} file')

    total_files = len({a["file"] for a in actions})
    ok(f'File toccati: {total_files}  |  azioni: {len(actions)}')

    # conflitti non toccati
    nr = [n for n,m in macros.items()
          if m['kind']=='CONFLICT' and m.get('conflict_type')=='NEEDS_REVIEW']
    if nr:
        warn(f'Conflitti da revisionare manualmente ({len(nr)}):')
        for n in sorted(nr): print(f'    ⚠  {n}')

    return actions

# ══════════════════════════════════════════════════════════════════════════════
# 4. FASE 3 — CLANG-TIDY
# ══════════════════════════════════════════════════════════════════════════════

TIDY_CHECKS = ','.join([
    'bugprone-*',
    'readability-braces-around-statements',
    'readability-inconsistent-declaration-parameter-name',
    'readability-redundant-declaration',
    'readability-redundant-preprocessor',
    'misc-redundant-expression',
    'misc-unused-parameters',
    'performance-no-int-to-ptr',
    '-bugprone-easily-swappable-parameters',
    '-bugprone-reserved-identifier',
])

def make_compile_commands(root: str, files: list, out_dir: str) -> str:
    """
    Genera un compile_commands.json minimale per i file C del progetto.
    Usa Bear se disponibile e c'è un Makefile, altrimenti costruisce a mano.
    """
    bear = find_tool(['bear'])
    makefile = os.path.join(root, 'Makefile')

    if bear and os.path.exists(makefile):
        info(f'Usando bear per compile_commands ({bear})...')
        rc, _, se = run([bear, '--', 'make', '-n'], cwd=root)
        cc_path = os.path.join(root, 'compile_commands.json')
        if rc == 0 and os.path.exists(cc_path):
            shutil.copy(cc_path, os.path.join(out_dir, 'compile_commands.json'))
            ok(f'compile_commands.json generato via bear')
            return cc_path

    # fallback: costruzione manuale
    info('Generazione compile_commands.json manuale...')
    include_dirs = set()
    for f in files:
        d = os.path.dirname(os.path.join(root, f))
        include_dirs.add(d)
    # aggiungi include dir standard del progetto
    for sd in ['include', 'include/api', 'kernel/include',
               'kernel/include/kernel', 'kernel/include/drivers']:
        fp = os.path.join(root, sd)
        if os.path.isdir(fp): include_dirs.add(fp)

    inc_flags = [f'-I{d}' for d in sorted(include_dirs)]
    base_flags = ['-std=c11', '-Wall', '-DKERNEL', '-D__kernel__'] + inc_flags

    entries = []
    for f in files:
        if not f.endswith('.c'): continue
        entries.append({
            'directory': root,
            'file':      os.path.join(root, f),
            'arguments': ['cc'] + base_flags + ['-c', os.path.join(root, f)],
        })

    cc_path = os.path.join(out_dir, 'compile_commands.json')
    with open(cc_path, 'w') as fp_:
        json.dump(entries, fp_, indent=2)
    ok(f'compile_commands.json manuale: {len(entries)} translation units')
    return cc_path

def phase_tidy(root: str, out_dir: str, data: dict):
    banner('FASE 3 — clang-tidy', CYN)

    clang_tidy = find_tool(['clang-tidy', 'clang-tidy-17', 'clang-tidy-16'])
    if not clang_tidy:
        warn('clang-tidy non trovato. Installa con: brew install llvm')
        warn('Poi aggiungi al PATH: /opt/homebrew/opt/llvm/bin')
        return

    ok(f'Usando: {clang_tidy}')

    files = list(data['include_graph'].keys())
    c_files = [f for f in files if f.endswith('.c')]
    if not c_files:
        warn('Nessun file .c trovato'); return

    cc_path = make_compile_commands(root, files, out_dir)

    # limita ai file più significativi (kernel core, non arch-specific)
    priority_files = [f for f in c_files
                      if not any(x in f for x in ('test', 'demo', 'scratch'))][:30]

    info(f'Analisi {len(priority_files)} file .c core...')
    tidy_results = []

    for f in priority_files:
        abs_f = os.path.join(root, f)
        cmd = [
            clang_tidy,
            f'--checks={TIDY_CHECKS}',
            f'--header-filter=.*',
            f'-p', out_dir,
            abs_f,
            '--',
            '-std=c11',
        ]
        rc, stdout, stderr = run(cmd, cwd=root, timeout=30)
        issues = [ln for ln in (stdout + stderr).split('\n')
                  if ': warning:' in ln or ': error:' in ln]
        if issues:
            tidy_results.append({'file': f, 'issues': issues})
            print(f'    {c(YLW,"⚠")} {f}: {len(issues)} problemi')
        else:
            print(f'    {c(GRN,"✓")} {f}')

    # salva report tidy
    report_path = os.path.join(out_dir, 'tidy_report.txt')
    with open(report_path, 'w') as fp_:
        for r in tidy_results:
            fp_.write(f'\n── {r["file"]} ──\n')
            for i in r['issues']:
                fp_.write(f'  {i}\n')

    total_issues = sum(len(r['issues']) for r in tidy_results)
    ok(f'clang-tidy: {total_issues} problemi in {len(tidy_results)} file → {report_path}')

# ══════════════════════════════════════════════════════════════════════════════
# 5. FASE 4 — COCCINELLE
# ══════════════════════════════════════════════════════════════════════════════

# Semantic patches ottimizzate per kernel C
COCCI_PATCHES = {

'01_mmio_raw_write.cocci': '''\
// Trasforma accessi raw MMIO write in chiamate HAL (8, 16, 32 bit)
@@
expression addr, val;
@@
(
- *(volatile uint32_t *)addr = val;
+ mmio_write32(addr, val);
|
- *(volatile uint16_t *)addr = val;
+ mmio_write16(addr, val);
|
- *(volatile uint8_t *)addr = val;
+ mmio_write8(addr, val);
|
- *((volatile uint32_t *)(addr)) = val;
+ mmio_write32(addr, val);
|
- *((volatile uint16_t *)(addr)) = val;
+ mmio_write16(addr, val);
|
- *((volatile uint8_t *)(addr)) = val;
+ mmio_write8(addr, val);
)
''',

'02_mmio_raw_read.cocci': '''\
// Trasforma accessi raw MMIO read in chiamate HAL (8, 16, 32 bit)
@@
expression addr;
@@
(
- *(volatile uint32_t *)addr
+ mmio_read32(addr)
|
- *(volatile uint16_t *)addr
+ mmio_read16(addr)
|
- *(volatile uint8_t *)addr
+ mmio_read8(addr)
|
- *(volatile uint32_t *)(addr)
+ mmio_read32(addr)
|
- *(volatile uint16_t *)(addr)
+ mmio_read16(addr)
|
- *(volatile uint8_t *)(addr)
+ mmio_read8(addr)
)
''',

# '03_null_ptr_style.cocci': '''\
# // Normalizza confronti NULL: if (p == 0) → if (p == NULL)
# // Gestisce sia == che !=, e lo stile Yoda
# @@
# expression p;
# @@
# (
# - p == 0
# + p == NULL
# |
# - p != 0
# + p != NULL
# |
# - 0 == p
# + p == NULL
# |
# - 0 != p
# + p != NULL
# )
# ''',

'04_error_return.cocci': '''\
// Standardizza: return -1 con return -EINVAL dove ha senso
// (solo nei contesti dove si usa già errno style o in funzioni che ritornano int)
@@
identifier fn;
expression cond;
@@
int fn(...) {
  ...
  if (cond) {
-   return -1;
+   return -EINVAL;
  }
  ...
}
''',

'05_redundant_cast_void.cocci': '''\
// Rimuove cast (void*) ridondanti su malloc/kmalloc-style
@@
expression size;
identifier alloc =~ "malloc|kmalloc|kalloc|alloc_pages";
@@
- (void *)alloc(size)
+ alloc(size)
''',

'06_spin_lock_irq.cocci': '''\
// Trova pattern lock/unlock sbilanciati o potenziali deadlock
@@
expression lock;
@@
  spin_lock(lock);
  ... when != spin_unlock(lock);
* spin_lock(lock);
''',

'07_sizeof_type_to_var.cocci': '''\
// Modernizza: sizeof(type) → sizeof(*var) per allocazioni
// Questo riduce il rischio di errori se il tipo del puntatore cambia
@@
type T;
T *p;
@@
(
- p = kmalloc(sizeof(T), ...)
+ p = kmalloc(sizeof(*p), ...)
|
- p = kzalloc(sizeof(T), ...)
+ p = kzalloc(sizeof(*p), ...)
)
''',

'08_zero_init.cocci': '''\
// Rimuove memset ridondanti dopo kzalloc
@@
expression ptr, size, flags;
@@
  ptr = kzalloc(size, flags);
  ... when != ptr = ...
- memset(ptr, 0, size);
''',

'09_bool_normalization.cocci': '''\
// Normalizza l'uso di bool (se il kernel lo supporta)
// if (cond == true) -> if (cond)
@@
expression E;
@@
(
- E == true
+ E
|
- E == false
+ !E
|
- true == E
+ E
|
- false == E
+ !E
)
''',
}

def phase_cocci(root: str, out_dir: str, apply: bool = False):
    banner('FASE 4 — Coccinelle semantic patches', MAG)

    spatch = find_tool(['spatch'])
    if not spatch:
        warn('spatch (Coccinelle) non trovato. Installa con: brew install coccinelle')
        warn('Poi aggiungi /opt/homebrew/bin al PATH')
        return

    ok(f'Usando: {spatch}')
    if apply: warn('MODALITÀ APPLICAZIONE ATTIVA — i file verranno modificati')

    cocci_dir = os.path.join(out_dir, 'cocci_patches')
    os.makedirs(cocci_dir, exist_ok=True)

    # scrivi tutti i file .cocci
    for fname, content in COCCI_PATCHES.items():
        with open(os.path.join(cocci_dir, fname), 'w') as f:
            f.write(content)
    ok(f'{len(COCCI_PATCHES)} patch scritte in {cocci_dir}')

    results_path = os.path.join(out_dir, 'cocci_report.txt')
    all_results = []

    for fname in sorted(COCCI_PATCHES.keys()):
        patch_path = os.path.join(cocci_dir, fname)
        info(f'Applico {fname}...')

        cmd = [
            spatch,
            '--cocci-file', patch_path,
            '--dir', root,
            '--include-headers',
            '--very-quiet',
        ]
        if apply:
            cmd.append('--in-place')
        else:
            cmd.append('--no-show-diff')

        rc, stdout, stderr = run(cmd, cwd=root, timeout=60)

        output = (stdout + stderr).strip()
        matches = [ln for ln in output.split('\n') if ln.strip()]

        if rc == -2:
            warn(f'spatch non eseguibile: {stderr}')
            break
        elif matches:
            print(f'    {c(MAG,"→")} {fname}: {len(matches)} match')
            all_results.append({'patch': fname, 'matches': matches})
        else:
            print(f'    {c(GRN,"✓")} {fname}: nessun match (già ok)')

    # salva report
    with open(results_path, 'w') as f:
        f.write('=== REPORT COCCINELLE ===\n\n')
        for r in all_results:
            f.write(f'\n── {r["patch"]} ──\n')
            for m in r['matches']:
                f.write(f'  {m}\n')

    total_matches = sum(len(r['matches']) for r in all_results)
    if total_matches:
        warn(f'Coccinelle: {total_matches} pattern da trasformare → {results_path}')
        warn('Riesegui con --sp-flag "--in-place" per applicare le trasformazioni')
    else:
        ok('Coccinelle: nessun pattern da trasformare — codice già moderno')

# ══════════════════════════════════════════════════════════════════════════════
# 6. FASE 5 — REPORT FINALE
# ══════════════════════════════════════════════════════════════════════════════

def phase_report(root: str, out_dir: str, data: dict, actions: list):
    banner('FASE 5 — Report finale', YLW)

    macros = data['macros']
    dups   = {n: m for n,m in macros.items() if m['kind']=='DUPLICATE'}
    cfls   = {n: m for n,m in macros.items() if m['kind']=='CONFLICT'}

    lines = [
        '╔══════════════════════════════════════════════════════════════════════╗',
        '║          KERNEL DOCTOR — REPORT FINALE                              ║',
        '╚══════════════════════════════════════════════════════════════════════╝',
        '',
        f'  Progetto : {root}',
        f'  Data     : {datetime.now().strftime("%Y-%m-%d %H:%M:%S")}',
        f'  File     : {data["meta"]["files"]}',
        '',
        '─── DEFINE ────────────────────────────────────────────────────────────',
        f'  Duplicati totali          : {len(dups)}',
        f'  Conflitti arch-specific   : {sum(1 for m in cfls.values() if m.get("conflict_type")=="ARCH_SPECIFIC")}',
        f'  Conflitti arch-inline     : {sum(1 for m in cfls.values() if m.get("conflict_type")=="VMM_ARCH_INLINE")}',
        f'  Conflitti intenzionali    : {sum(1 for m in cfls.values() if m.get("conflict_type")=="INTENTIONAL")}',
        f'  Conflitti da revisionare  : {sum(1 for m in cfls.values() if m.get("conflict_type")=="NEEDS_REVIEW")}',
        '',
        '─── AZIONI APPLICATE ──────────────────────────────────────────────────',
    ]

    if actions:
        by_strat = defaultdict(int)
        for a in actions: by_strat[a['strategy']] += 1
        for s,cnt in sorted(by_strat.items()):
            lines.append(f'  {s:<30} {cnt} file')
    else:
        lines.append('  (nessuna azione applicata — dry-run o già risolto)')

    lines += [
        '',
        '─── CONFLITTI NON TOCCATI ─────────────────────────────────────────────',
    ]
    for name, m in sorted(cfls.items()):
        ct = m.get('conflict_type','?')
        tag = {'ARCH_SPECIFIC':'✓ arch ok','INTENTIONAL':'✓ design',
               'VMM_ARCH_INLINE':'✅ risolto','NEEDS_REVIEW':'⚠ manuale'}.get(ct, ct)
        lines.append(f'  {tag:<20} {name}')

    lines += [
        '',
        '─── FILE GENERATI ─────────────────────────────────────────────────────',
        f'  {out_dir}/',
        '    kernel_structure.json   — struttura analisi',
        '    include_graph.txt       — grafo include leggibile',
        '    tidy_report.txt         — output clang-tidy',
        '    cocci_report.txt        — output Coccinelle',
        '    cocci_patches/          — file .cocci generati',
        '    resolve_report.txt      — azioni risoluzione',
        '',
        '══════════════════════════════════════════════════════════════════════',
    ]

    report_text = '\n'.join(lines)
    print('\n' + report_text)

    report_path = os.path.join(out_dir, 'final_report.txt')
    with open(report_path, 'w') as f: f.write(report_text)
    ok(f'Report salvato: {report_path}')

    # grafo include leggibile
    ig_path = os.path.join(out_dir, 'include_graph.txt')
    with open(ig_path, 'w') as f:
        f.write('=== INCLUDE GRAPH ===\n')
        for fp, incs in sorted(data['include_graph'].items()):
            if incs:
                f.write(f'\n{fp}\n')
                for inc in incs: f.write(f'  └─ {inc}\n')
    ok(f'Grafo include: {ig_path}')

# ══════════════════════════════════════════════════════════════════════════════
# 7. ENTRY POINT
# ══════════════════════════════════════════════════════════════════════════════

def main():
    args = parse_args()
    root = os.path.abspath(args.root)

    if not os.path.isdir(root):
        err(f'Percorso non valido: {root}'); sys.exit(1)

    out_dir = args.out_dir or os.path.join(root, 'kernel_doctor_out')
    os.makedirs(out_dir, exist_ok=True)

    dry    = args.dry_run
    backup = not args.no_backup

    do_all     = args.all
    do_analyze = args.analyze or do_all
    do_resolve = args.resolve or do_all
    do_tidy    = args.tidy    or do_all
    do_cocci   = args.cocci   or do_all
    do_report  = args.report  or do_all

    if not any([do_analyze, do_resolve, do_tidy, do_cocci, do_report]):
        warn('Nessuna fase selezionata. Usa --all o una combinazione di --analyze --resolve --tidy --cocci --report')
        sys.exit(0)

    print(f'\n{BOLD}{BLU}kernel_doctor v{VERSION}{RST}')
    print(f'  Progetto : {root}')
    print(f'  Output   : {out_dir}')
    print(f'  Backup   : {"sì (.bak)" if backup else "no"}')
    if dry: print(f'  {c(YLW,"DRY-RUN attivo")}')

    data    = None
    actions = []
    json_path = os.path.join(out_dir, 'kernel_structure.json')

    # Fase 1
    if do_analyze:
        data = phase_analyze(root, out_dir)
    elif os.path.exists(json_path):
        info(f'Carico analisi esistente: {json_path}')
        data = json.load(open(json_path, encoding='utf-8'))
    else:
        err('kernel_structure.json non trovato. Esegui --analyze prima.')
        sys.exit(1)

    # Fase 2
    if do_resolve:
        actions = phase_resolve(root, out_dir, data, dry, backup)

    # Fase 3
    if do_tidy:
        phase_tidy(root, out_dir, data)

    # Fase 4
    if do_cocci:
        phase_cocci(root, out_dir, apply=args.apply)

    # Fase 5
    if do_report:
        phase_report(root, out_dir, data, actions)

    print(f'\n{BOLD}{GRN}✅ kernel_doctor completato.{RST}\n')

if __name__ == '__main__':
    main()
