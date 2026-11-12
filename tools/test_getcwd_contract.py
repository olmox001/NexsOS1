#!/usr/bin/env python3
"""Check actual native GETCWD block for bounded copies and exact error states."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PREFIX = r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
struct process { char cwd[128]; };
static struct process owner = {.cwd="/home"};
static struct process *current_process = &owner;
typedef struct { long result; } frame_type;
static int copy_fault;
static int hal_copy_to_user(void *dest, const void *source, size_t size) {
    if (copy_fault) return -1;
    memcpy(dest, source, size); return 0;
}
static void hal_frame_set_return(frame_type *frame, long result) { frame->result = result; }
static frame_type *call(frame_type *frame, uintptr_t arg0, uintptr_t arg1)
'''
SUFFIX = r'''
int main(void) {
    frame_type frame;
    unsigned char buffer[256]; memset(buffer, 0xa5, sizeof(buffer));
    if (call(&frame, (uintptr_t)buffer, sizeof(buffer)) != &frame || frame.result || strcmp((char *)buffer, "/home")) return 1;
    for (size_t i=6; i<sizeof(buffer); ++i) if (buffer[i] != 0xa5) return 2;
    memset(buffer, 0xa5, sizeof(buffer));
    if (call(&frame, (uintptr_t)buffer, 5) != &frame || frame.result != -ERANGE || buffer[0] != 0xa5) return 3;
    if (call(&frame, (uintptr_t)buffer, 6) != &frame || frame.result) return 4;
    if (call(&frame, 0, sizeof(buffer)) != &frame || frame.result != -EINVAL) return 5;
    if (call(&frame, (uintptr_t)buffer, 0) != &frame || frame.result != -EINVAL) return 6;
    copy_fault=1;
    if (call(&frame, (uintptr_t)buffer, sizeof(buffer)) != &frame || frame.result != -EFAULT) return 7;
    memset(owner.cwd, 'x', sizeof(owner.cwd)); copy_fault=0;
    return call(&frame, (uintptr_t)buffer, sizeof(buffer)) == &frame && frame.result == -EIO ? 0 : 8;
}
'''

class GetcwdContractTests(unittest.TestCase):
    def test_copy_includes_only_cwd_and_its_terminator(self):
        text = (ROOT / 'kernel/core/syscall_dispatch.c').read_text()
        start = text.index('case SYS_GETCWD: {') + len('case SYS_GETCWD: ')
        end = text.index('\n  case SYS_PORT_SEND_CAPS:', start)
        block = text[start:end].strip()
        self.assertTrue(block.endswith('}'))
        with tempfile.TemporaryDirectory(prefix='getcwd-contract-') as temp:
            source = Path(temp) / 'fixture.c'; source.write_text(PREFIX + block + SUFFIX)
            binary = Path(temp) / 'fixture'
            subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', str(source), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

if __name__ == '__main__':
    unittest.main()
