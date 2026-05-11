#!/usr/bin/env python3
import os
import re
import sys
import subprocess

class KernelAuditorV3:
    def __init__(self, root_dir):
        self.root_dir = root_dir
        self.functions = {}  
        self.violations = []
        self.stability_issues = []
        self.stats = {"core": 0, "hal": 0, "driver": 0, "arch": 0, "user": 0}
        self.layers_hierarchy = {"core": 3, "driver": 2, "hal": 1, "arch": 0, "user": 4}

    def get_layer(self, path):
        path = os.path.relpath(path, self.root_dir)
        if 'arch/' in path or 'boot/' in path: return 'arch'
        if 'drivers/' in path: return 'driver'
        if 'irq/' in path or 'include/kernel/arch.h' in path or 'include/kernel/platform.h' in path: return 'hal'
        if 'user/' in path: return 'user'
        return 'core'

    def scan_files(self):
        """Phase 1: Deep scan and registration."""
        for root, _, files in os.walk(self.root_dir):
            if any(d in root for d in ['build', '.git', 'tools', 'scratch']): continue
            for file in files:
                if file.endswith(('.c', '.h', '.S')):
                    path = os.path.join(root, file)
                    layer = self.get_layer(path)
                    with open(path, 'r', errors='ignore') as f:
                        content = f.read()
                        
                        # Register function definitions
                        c_defs = re.findall(r'^(\w+\s+\*?\w+)\s*(\w+)\s*\((.*?)\)\s*\{', content, re.MULTILINE)
                        for d in c_defs:
                            name = d[1]
                            if name in ['if', 'while', 'for', 'switch', 'return']: continue
                            self._register_func(name, path, layer)

                        asm_defs = re.findall(r'^\.global\s+(\w+)', content, re.MULTILINE)
                        for name in asm_defs:
                            self._register_func(name, path, layer)

                        if 'arch.h' in file:
                            macros = re.findall(r'^#define\s+(arch_\w+)\(', content, re.MULTILINE)
                            for m in macros:
                                self._register_func(m, path, 'hal', is_hal=True)

    def _register_func(self, name, path, layer, is_hal=False):
        if name not in self.functions:
            self.functions[name] = {
                'file': path,
                'layer': layer,
                'refs': [],
                'is_hal': is_hal or name.startswith(('arch_', 'irq_')),
                'is_internal': name.startswith('__')
            }
            self.stats[layer] += 1

    def perform_audit(self):
        """Phase 2: Architectural and Stability Audit."""
        for root, _, files in os.walk(self.root_dir):
            if any(d in root for d in ['build', '.git', 'tools', 'scratch']): continue
            for file in files:
                if file.endswith(('.c', '.h', '.S')):
                    path = os.path.join(root, file)
                    layer = self.get_layer(path)
                    with open(path, 'r', errors='ignore') as f:
                        content = f.read()
                        
                        self._check_layering(content, path, layer)
                        self._check_stability(content, path, layer)

    def _check_layering(self, content, path, layer):
        clean_content = re.sub(r'/\*.*?\*/|//.*', '', content, flags=re.DOTALL)
        for func, data in self.functions.items():
            if re.search(r'\b' + func + r'\b', clean_content):
                if data['file'] != path:
                    data['refs'].append(path)
                    target_layer = data['layer']
                    
                    # Core to Arch violation (excluding boot specific access)
                    if layer == 'core' and target_layer == 'arch' and not data['is_hal']:
                        if 'main.c' in path and 'kernel_main' in func: continue # Entry point is special
                        self.violations.append(f"[LAYER] Core {path} calls Arch {func} directly.")
                    
                    # User to Core violation (Must use Syscalls)
                    if layer == 'user' and target_layer == 'core':
                        self.violations.append(f"[SECURITY] User code {path} attempts direct Core call: {func}")

    def _check_stability(self, content, path, layer):
        # 3.1 Memory Leak Heuristics (Basic scope-based tracking)
        allocs = len(re.findall(r'\b(kmalloc|pmm_alloc_page|pmm_alloc_pages)\b', content))
        frees = len(re.findall(r'\b(kfree|pmm_free_page|pmm_free_pages)\b', content))
        if allocs > frees:
            self.stability_issues.append(f"[LEAK] {path}: Found {allocs} allocs vs {frees} frees. Potential memory leak.")

        # 3.2 Lock Balance
        locks = len(re.findall(r'\b(spin_lock|spin_lock_irqsave)\b', content))
        unlocks = len(re.findall(r'\b(spin_unlock|spin_unlock_irqrestore)\b', content))
        if locks != unlocks:
            self.stability_issues.append(f"[LOCK] {path}: Unbalanced spinlocks ({locks} locks, {unlocks} unlocks). Risk of deadlock.")

        # 3.3 Infinite Allocation Loops
        if re.search(r'while\s*\(\s*(1|true)\s*\)\s*\{[^}]*(kmalloc|pmm_alloc)', content, re.DOTALL):
            self.stability_issues.append(f"[CRITICAL] {path}: Memory allocation detected inside infinite loop. System exhaustion risk.")

        # 3.4 Missing Error Handling
        if 'kmalloc' in content and '== NULL' not in content and 'if (!' not in content:
            self.stability_issues.append(f"[ERROR_HANDLING] {path}: kmalloc used without NULL check.")

    def report(self):
        print("\n" + "="*80)
        print("          KERNEL INTEGRITY, STABILITY & PERFORMANCE AUDIT")
        print("="*80)
        
        print(f"\n[SUMMARY] System Composition:")
        for layer, count in sorted(self.stats.items(), key=lambda x: self.layers_hierarchy.get(x[0], 99)):
            print(f"  - {layer.upper():<8}: {count:<4} entities")

        if self.violations:
            print(f"\n[!!!] ARCHITECTURAL VIOLATIONS: {len(self.violations)}")
            for v in self.violations[:10]: print(f"  - {v}")
            if len(self.violations) > 10: print(f"    ... and {len(self.violations)-10} more.")

        if self.stability_issues:
            print(f"\n[WAR] STABILITY & PERFORMANCE ISSUES: {len(self.stability_issues)}")
            for s in self.stability_issues[:15]: print(f"  - {s}")
            if len(self.stability_issues) > 15: print(f"    ... and {len(self.stability_issues)-15} more.")

        print(f"\n[ANALYSIS] Redundant/Unused Code:")
        unused = [f for f, d in self.functions.items() if not d['refs'] and not f.startswith(('kernel_main', 'arch_', 'irq_', '_start', 'secondary_cpu'))]
        for f in unused[:10]: print(f"  - Unused function: {f}")
        print(f"  Total dead code candidates: {len(unused)}")

        print("\n" + "="*80)

if __name__ == "__main__":
    v = KernelAuditorV3('.')
    v.scan_files()
    v.perform_audit()
    v.report()
