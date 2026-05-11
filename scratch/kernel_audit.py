import os
import re
import sys

def audit_kernel(root_dir):
    functions = {} # name -> {file, lines, callers}
    old_patterns = [
        r'\bgic_\w+',
        r'\b__asm__\b.*?\bdsb\b',
        r'\b__asm__\b.*?\bisb\b',
        r'\b__asm__\b.*?\bdmb\b',
        r'\bgic_init\b',
        r'\bgic_enable_irq\b',
    ]
    
    hal_patterns = [
        r'\barch_\w+',
        r'\birq_\w+',
    ]

    report = []
    
    for root, _, files in os.walk(root_dir):
        if 'build' in root or '.git' in root:
            continue
        for file in files:
            if file.endswith(('.c', '.h')):
                path = os.path.join(root, file)
                with open(path, 'r') as f:
                    content = f.read()
                    
                    # Find function definitions (simple regex for C)
                    # matches: void func_name(args) {
                    defs = re.findall(r'^(\w+\s+\*?\w+)\s*\((.*?)\)\s*\{', content, re.MULTILINE)
                    for d in defs:
                        name = d[0].split()[-1].replace('*', '')
                        functions[name] = {'file': path, 'refs': []}
                    
                    # Find potential issues
                    for pattern in old_patterns:
                        matches = re.finditer(pattern, content)
                        for m in matches:
                            # Skip definitions in gic.c/irq.c
                            if 'gic.c' in file or 'irq.c' in file:
                                if 'gic_' in m.group() or 'irq_' in m.group():
                                    continue
                            line_no = content.count('\n', 0, m.start()) + 1
                            report.append(f"[WARNING] Old/Arch-Specific call found: {m.group()} in {path}:{line_no}")

    # Map references
    for root, _, files in os.walk(root_dir):
        if 'build' in root or '.git' in root:
            continue
        for file in files:
            if file.endswith(('.c', '.h')):
                path = os.path.join(root, file)
                with open(path, 'r') as f:
                    content = f.read()
                    for func in functions:
                        if func in content:
                            # Simple check if it's a call
                            if re.search(r'\b' + func + r'\s*\(', content):
                                if functions[func]['file'] != path:
                                    functions[func]['refs'].append(path)

    print("--- KERNEL AUDIT REPORT ---")
    for r in report:
        print(r)
    
    print("\n--- FUNCTION DEPENDENCY MAP ---")
    for func, data in functions.items():
        if data['refs']:
            print(f"{func} ({data['file']}) <- {', '.join(set(data['refs']))}")
        else:
            # Potentially orphaned or entry point
            if not func.startswith('__'):
                print(f"[INFO] Potential leaf/entry function: {func} in {data['file']}")

if __name__ == "__main__":
    audit_kernel(sys.argv[1] if len(sys.argv) > 1 else '.')
