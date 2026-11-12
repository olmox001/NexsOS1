#!/usr/bin/env python3
"""Check actual scheduler quantum updates propagate to the clockevent."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/power12'))
from common import clang_ast, source_definitions
PREFIX = r'''

#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
struct process { int priority, state; uint64_t quantum_deadline_ns; int pid; };
static struct process task;
static unsigned programmed;
#define PROC_PRIO_IDLE 31
#define PROC_RUNNING 1
#define OS1LIBKERNEL_SCHED_QUANTUM_NS UINT64_C(30000000)
#define SCHED_ASSERT(condition, ...) do { if (!(condition)) abort(); } while (0)
static void panic(const char *text, ...) { (void)text; abort(); }
static int OS1libkernel_time_deadline_after_ns(uint64_t delta, uint64_t *out) {
    *out = UINT64_C(1000000000) + delta;
    return 0;
}
void OS1libkernel_timer_reschedule_local(void) {
    if (task.priority == PROC_PRIO_IDLE && task.quantum_deadline_ns != 0) abort();
    if (task.priority != PROC_PRIO_IDLE && task.quantum_deadline_ns != UINT64_C(1030000000)) abort();
    ++programmed;
}
'''
SUFFIX = r'''

int main(void) {
    task = (struct process){.priority=2, .state=PROC_RUNNING, .pid=7};
    __sched_arm_quantum(&task);
    if (programmed != 1) return 1;
    task.priority = PROC_PRIO_IDLE;
    __sched_arm_quantum(&task);
    return programmed == 2 ? 0 : 2;
}
'''

def actual_quantum_code(path):
    ast, error = clang_ast(path, 'amd64')
    if ast is None:
        raise AssertionError(error)
    nodes = [n for n in source_definitions(ast, path) if n['name'] == '__sched_arm_quantum']
    if len(nodes) != 1:
        raise AssertionError('missing unique quantum provider')
    bounds = nodes[0]['range']
    raw = path.read_bytes()
    return raw[bounds['begin']['offset']:bounds['end']['offset'] + bounds['end']['tokLen']].decode()

class SchedulerQuantumTests(unittest.TestCase):
    def test_every_quantum_update_reaches_clockevent(self):
        source = ROOT / 'kernel/sched/process.c'
        host = ROOT / 'build/host'; host.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='quantum-rearm-', dir=host) as temp:
            work = Path(temp)
            if os.environ.get('SCHED_BASELINE_SOURCE'):
                source = work / 'baseline.c'
                source.write_bytes(Path(os.environ['SCHED_BASELINE_SOURCE']).read_bytes())
            fixture = work / 'fixture.c'
            fixture.write_text(PREFIX + actual_quantum_code(source) + SUFFIX)
            binary = work / 'fixture'
            subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', str(fixture), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], check=False)
            self.assertEqual(result.returncode, 0, 'same-task and idle transitions must both refresh the clockevent')
if __name__ == '__main__':
    unittest.main()
