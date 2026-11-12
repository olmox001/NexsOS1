#!/usr/bin/env python3
"""Exercise actual bounded owner-before-publication creation code with HAL mocks."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/power12'))
from common import clang_ast, source_definitions
FUNCTIONS = {'level_for_path', 'spawn_path_is_sane', 'spawn_redirect_one',
             'spawn_prepare', 'spawn_load_and_publish', 'spawn_dispatch_options',
             'spawn_copy_header', 'kernel_spawn_owned'}
RECORDS = {'spawn_arguments', 'spawn_options', 'spawn_user_copy'}
PREFIX = r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <caps.h>
struct process { uint64_t pid; int ctty_win, owner_pid; };
static struct process creator = {7, 9, 0}, child;
static struct process *current_process = &creator;
static int privileged = 1, has_spawn = 1, irqs_enabled = 1;
static int created, assigned, loaded, published, aborted, owner_alive = 1;
static int load_fails, violation;
static int proc_is_machine(const struct process *p) { return p && p->pid == 1; }
static int proc_is_privileged(const struct process *p) { (void)p; return privileged; }
static int proc_has_cap(const struct process *p, uint32_t cap) { (void)p; (void)cap; return has_spawn; }
int process_creation_begin(void) { return 0; }
void hal_irq_disable(void) { irqs_enabled = 0; }
void hal_irq_enable(void) { irqs_enabled = 1; }
static struct process *process_create(const char *path, int priority, uint8_t level) {
    (void)path; (void)priority; (void)level;
    ++created; child = (struct process){123, 9, 0}; return &child;
}
static struct process *process_create_caps(const char *path, int priority, uint8_t level, uint32_t caps) {
    (void)caps; return process_create(path, priority, level);
}
static int process_set_owner(int pid, int owner) {
    if (pid != 123 || published || loaded) violation = 1;
    if (!owner_alive) return -3;
    child.owner_pid = owner; ++assigned; return 0;
}
static int process_redirect_child_fd(struct process *p, int child_fd, int parent_fd) {
    (void)p; (void)child_fd; (void)parent_fd; return 0;
}
static int process_redirect_child_fd_from(struct process *s, struct process *p, int child_fd, int parent_fd) {
    (void)s; return process_redirect_child_fd(p, child_fd, parent_fd);
}
static struct process *process_find_by_pid(int pid) { (void)pid; return NULL; }
static int process_load_elf_args(struct process *p, const char *path, int argc, char *const argv[]) {
    if (!irqs_enabled || p->owner_pid != 77 || !assigned || strcmp(path, "/bin/true") || argc != 1 || strcmp(argv[0], "/bin/true")) violation = 1;
    ++loaded; return load_fails ? -1 : 0;
}
static void process_finalize_spawn(struct process *p) {
    if (!assigned || !loaded || p->owner_pid != 77) violation = 1;
    ++published;
    /* A real fast child can be reaped before this call returns. */
    p->pid = 0;
}
static void process_abort_spawn(struct process *p) { (void)p; ++aborted; }
static int hal_copy_from_user(void *out, const void *in, size_t size) {
    if (!in) return -1; memcpy(out, in, size); return 0;
}
static int hal_copy_string_from_user(char *out, const char *in, size_t size) {
    if (!in) return -1;
    for (size_t i = 0; i < size; ++i) { out[i] = in[i]; if (!in[i]) return 0; }
    return -1;
}
#define PROC_PRIO_USER 2
#define EINVAL 22
#define EPERM 1
#define ESRCH 3
#define EAGAIN 11
#define EFAULT 14
#define ENOENT 2
'''
SUFFIX = r'''
static void reset(void) {
    created = assigned = loaded = published = aborted = load_fails = violation = 0;
    privileged = has_spawn = irqs_enabled = owner_alive = 1;
}
int main(void) {
    uint64_t argv[1] = {(uint64_t)(uintptr_t)"/bin/true"};
    struct os1_spawn_request_v1 request = {
        .abi_version = OS1_SPAWN_REQUEST_VERSION, .struct_size = sizeof(request),
        .argc = 1, .path = (uint64_t)(uintptr_t)"/bin/true",
        .argv = (uint64_t)(uintptr_t)argv
    };
    reset();
    if (kernel_spawn_owned(77, &request) != 123 || violation || published != 1 || aborted) return 1;
    reset(); owner_alive = 0;
    if (kernel_spawn_owned(77, &request) != -ESRCH || published || loaded || aborted != 1) return 2;
    reset(); load_fails = 1;
    if (kernel_spawn_owned(77, &request) != -ENOENT || published || aborted != 1) return 3;
    reset(); privileged = 0;
    if (kernel_spawn_owned(77, &request) != -EPERM || created) return 4;
    reset(); has_spawn = 0;
    if (kernel_spawn_owned(77, &request) != -EPERM || created) return 5;
    reset(); request.reserved = 1;
    if (kernel_spawn_owned(77, &request) != -EINVAL || created) return 6;
    request.reserved = 0; request.argc = SPAWN_MAX_ARGS + 1;
    if (kernel_spawn_owned(77, &request) != -EINVAL || created) return 7;
    request.argc = 1; request.flags = UINT32_MAX;
    if (kernel_spawn_owned(77, &request) != -EINVAL || created) return 8;
    request.flags = 0; request.argv = 0;
    if (kernel_spawn_owned(77, &request) != -EFAULT || created) return 9;
    request.argv = (uint64_t)(uintptr_t)argv; request.path = 0;
    if (kernel_spawn_owned(77, &request) != -EFAULT || created) return 10;
    request.path = (uint64_t)(uintptr_t)"/bin/true"; request.nredir = 1;
    if (kernel_spawn_owned(77, &request) != -EFAULT || created) return 11;
    request.nredir = 0; request.abi_version = OS1_SPAWN_REQUEST_VERSION + 1;
    if (kernel_spawn_owned(77, &request) != -EINVAL || created) return 12;
    return 0;
}
'''

def fragment(path, functions, records):
    ast, error = clang_ast(path, 'amd64')
    if ast is None:
        raise AssertionError(error)
    raw = path.read_bytes()
    nodes = [n for n in source_definitions(ast, path) if n['name'] in functions]
    if {n['name'] for n in nodes} != functions:
        raise AssertionError('missing creation function')
    record_nodes = [n for n in ast['inner'] if n.get('kind') == 'RecordDecl' and
                    n.get('completeDefinition') and n.get('name') in records and
                    not n.get('loc', {}).get('includedFrom')]
    if {n['name'] for n in record_nodes} != records:
        raise AssertionError('missing creation record')
    code = []
    for node in sorted(nodes + record_nodes, key=lambda n: n['range']['begin']['offset']):
        bounds = node['range']
        first = bounds['begin']['offset']
        end = bounds['end']['offset'] + bounds['end']['tokLen']
        code.append(raw[first:end].decode() + (';' if node['kind'] == 'RecordDecl' else ''))
    return '\n'.join(code)

class SpawnOwnedTests(unittest.TestCase):
    def test_owner_precedes_fast_exit_and_loader_is_preemptible(self):
        host = ROOT / 'build/host'
        host.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='spawn-owned-', dir=host) as temp:
            work = Path(temp)
            source = ROOT / 'kernel/core/syscall_dispatch.c'
            if os.environ.get('SPAWN_BASELINE_SOURCE'):
                baseline = work / 'baseline.c'
                baseline.write_bytes(Path(os.environ['SPAWN_BASELINE_SOURCE']).read_bytes())
                source = baseline
            header = ROOT / 'kernel/include/abi/posix_types.h'
            define = next(line for line in header.read_text().splitlines() if
                          line.startswith('#define OS1_SPAWN_REQUEST_VERSION '))
            descriptor = fragment(header, set(), {'os1_spawn_request_v1'})
            defines = [line for line in source.read_text().splitlines() if
                       line.startswith('#define SPAWN_MAX_ARGS ') or
                       line.startswith('#define SPAWN_ARG_LEN ')]
            code = PREFIX + '\n' + define + '\n' + descriptor + '\n'
            code += '\n'.join(defines) + '\n' + fragment(source, FUNCTIONS, RECORDS) + SUFFIX
            fixture = work / 'fixture.c'; fixture.write_text(code)
            binary = work / 'fixture'
            subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I' + str(ROOT / 'kernel/include/abi'),
                            str(fixture), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], check=False)
            self.assertEqual(result.returncode, 0, 'ownership must precede publication; ELF I/O must not mask IRQs')
    def test_creation_control_is_deferred_until_syscall_cleanup(self):
        source = ROOT / 'kernel/sched/process.c'
        functions = {'process_defer_creation_control', 'process_continue_creation',
                     'process_creation_begin', 'process_creation_finish'}
        code = '#include <stdint.h>\n#include <stdlib.h>\n#define ESRCH 3\n#define EBUSY 16\nstruct process { unsigned pid; int dying, creation_active, stop_pending, kill_pending; };\ntypedef struct { int id; } hal_frame_t;\nstatic struct process parent = {7, 0, 0, 0, 0};\nstatic struct process *current_process = &parent;\nstatic int sched_lock, kills, stops, violations;\nstatic hal_frame_t next_frame;\nstatic void spin_lock_irqsave(int *lock, uint64_t *flags) { (void)lock; *flags = 0; }\nstatic void spin_unlock_irqrestore(int *lock, uint64_t flags) { (void)lock; (void)flags; }\nstatic int process_terminate(int pid) {\n    if (pid != 7 || parent.creation_active) violations = 1;\n    ++kills; return 0;\n}\nstatic int __proc_park_locked(struct process *process) {\n    if (process != &parent || !parent.creation_active) violations = 1;\n    ++stops; return 0;\n}\nstatic hal_frame_t *schedule(hal_frame_t *frame) { (void)frame; return &next_frame; }\nstatic void panic(const char *format, ...) { (void)format; abort(); }\n' + fragment(source, functions, set()) + 'int main(void) {\n    hal_frame_t frame;\n    if (process_creation_begin() || !parent.creation_active) return 1;\n    if (!process_defer_creation_control(&parent, 1) || !parent.stop_pending || stops) return 2;\n    if (!process_defer_creation_control(&parent, 0) || !parent.kill_pending || kills) return 3;\n    if (!process_continue_creation(&parent) || parent.stop_pending || !parent.kill_pending) return 4;\n    if (process_creation_finish(&frame) != &next_frame || kills != 1 || stops || violations || parent.creation_active || parent.kill_pending) return 5;\n    if (process_creation_finish(&frame) != &frame || kills != 1) return 6;\n    if (process_creation_begin() || process_creation_begin() != -EBUSY) return 7;\n    if (!process_defer_creation_control(&parent, 1)) return 8;\n    if (process_creation_finish(&frame) != &next_frame || stops != 1 || parent.stop_pending || violations) return 9;\n    parent.dying = 1;\n    if (process_creation_begin() != -ESRCH) return 10;\n    current_process = NULL;\n    if (process_creation_begin() != -ESRCH || process_creation_finish(&frame) != &frame) return 11;\n    return 0;\n}\n'
        with tempfile.TemporaryDirectory(prefix='creation-guard-') as temp:
            fixture = Path(temp) / 'fixture.c'; fixture.write_text(code)
            binary = Path(temp) / 'fixture'
            subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', str(fixture), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
