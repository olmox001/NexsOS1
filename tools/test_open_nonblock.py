#!/usr/bin/env python3
"""Exercise real libc open against capability acquisition and cleanup faults."""
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
#include <errno.h>
static int native_result = 7, native_calls, queries, closes, seeks;
static long query_result, close_result;
static int observed_flags;
static long _sys_open(const char *path, int flags) {
    (void)path; ++native_calls; observed_flags = flags;
    if (flags & ~(O_ACCMODE | O_CREAT | O_TRUNC | O_APPEND)) return -EINVAL;
    return native_result;
}
static long OS1low_cap_query(int fd) { if (fd != 7) return -EBADF; ++queries; return query_result; }
static long OS1low_handle_close(int fd) { if (fd != 7) return -EBADF; ++closes; return close_result; }
static long _sys_lseek(int fd, long offset, int whence) {
    if (fd != 7 || offset || whence != SEEK_END) return -EINVAL;
    ++seeks; return 123;
}
static long errno_ret_ctx(long result, const char *path) {
    (void)path; errno = (int)-result; return -1;
}
#define open tested_open
'''
SUFFIX = r'''
#undef open
static void reset(void) {
    native_calls = queries = closes = seeks = 0;
    native_result = 7; query_result = (long)OBJ_TYPE_FILE << 24;
    close_result = 0; errno = 0;
}
int main(void) {
    reset();
    if (tested_open("file", O_WRONLY | O_CREAT | O_NONBLOCK) != 7 ||
        observed_flags != (O_WRONLY | O_CREAT) || queries != 1 || closes) return 1;
    reset(); query_result = (long)OBJ_TYPE_PROCESS << 24;
    if (tested_open("process", O_RDONLY | O_NONBLOCK) != -1 || errno != ENOTSUP || closes != 1) return 2;
    reset(); query_result = -EBADF;
    if (tested_open("file", O_RDONLY | O_NONBLOCK) != -1 || errno != EBADF || closes != 1) return 3;
    reset(); query_result = (long)OBJ_TYPE_PROCESS << 24; close_result = -EIO;
    if (tested_open("process", O_RDONLY | O_NONBLOCK) != -1 || errno != EIO || closes != 1) return 4;
    reset(); native_result = -EACCES;
    if (tested_open("protected", O_WRONLY | O_NONBLOCK) != -1 || errno != EACCES || queries || closes) return 5;
    reset();
    if (tested_open("file", O_TRUNC | O_NONBLOCK | 0x40000000) != -1 || errno != EINVAL || queries || closes) return 6;
    reset();
    if (tested_open("file", O_TRUNC | O_NONBLOCK | O_ACCMODE) != -1 || errno != EINVAL || native_calls) return 7;
    reset(); query_result = (long)OBJ_TYPE_PROCESS << 24;
    if (tested_open("process", O_RDONLY) != 7 || queries || closes) return 8;
    reset();
    if (tested_open("file", O_WRONLY | O_APPEND | O_NONBLOCK) != 7 || seeks != 1 || closes) return 9;
    return 0;
}
'''

class OpenNonblockTests(unittest.TestCase):
    def test_actual_type_gate_native_errors_and_cleanup(self):
        host = ROOT / 'build/host'; host.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='open-nonblock-', dir=host) as temp:
            source = ROOT / 'system/lib/lib.c'
            if os.environ.get('OPEN_BASELINE_SOURCE'):
                source = Path(temp) / 'baseline.c'
                source.write_bytes(Path(os.environ['OPEN_BASELINE_SOURCE']).read_bytes())
                # Preserve the production translation unit's quoted includes.
                (Path(temp) / 'internal.h').symlink_to(ROOT / 'system/lib/internal.h')
                (Path(temp) / 'common').symlink_to(ROOT / 'system/lib/common', target_is_directory=True)
            ast, error = clang_ast(source, 'amd64')
            self.assertIsNotNone(ast, error)
            nodes = [n for n in source_definitions(ast, source) if n['name'] in {'open', 'open_check_nonblock'}]
            self.assertTrue(any(n['name'] == 'open' for n in nodes))
            raw = source.read_bytes()
            code = '\n'.join(raw[n['range']['begin']['offset']:n['range']['end']['offset'] + n['range']['end']['tokLen']].decode() for n in nodes)
            constants = []
            wanted = {'O_RDONLY','O_WRONLY','O_RDWR','O_CREAT','O_TRUNC','O_APPEND','O_NONBLOCK','O_ACCMODE','SEEK_END','OBJ_TYPE_FILE','OBJ_TYPE_PROCESS','OS1_CAPQ_TYPE(v)'}
            for path in ('system/include/api/fcntl.h','system/include/contract/posix_types.h','system/include/contract/object.h'):
                for line in (ROOT / path).read_text().splitlines():
                    parts = line.split()
                    if len(parts) > 2 and parts[0] == '#define' and parts[1] in wanted:
                        constants.append(line); wanted.remove(parts[1])
            self.assertFalse(wanted)
            fixture = Path(temp) / 'fixture.c'
            fixture.write_text('\n'.join(constants) + PREFIX + code + SUFFIX)
            binary = Path(temp) / 'fixture'
            subprocess.run(['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
                            '-Wno-unused-function','-fsanitize=address,undefined',str(fixture),'-o',str(binary)],check=True)
            result = subprocess.run([str(binary)],check=False)
            self.assertEqual(result.returncode,0)

if __name__ == '__main__':
    unittest.main()
