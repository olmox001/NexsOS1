#!/usr/bin/env python3
"""Exercise actual ext4 write sizing and explicit truncate against disk mocks."""
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
#include <stddef.h>
#include <string.h>
#include <errno.h>
#define EXT4_BLOCK_SIZE 4096U
struct ext4_fs { int read_only; };
struct ext4_inode { uint32_t i_mode, i_size_lo, i_size_high; };
struct vfs_mount { void *fs_private; };
static struct ext4_fs filesystem;
static struct ext4_inode stored = {0x8000, 10, 0};
static struct vfs_mount mount = {&filesystem};
static unsigned char disk[16384];
static int lookup_failure, inode_failure, write_failure, update_failure;
static int ext4_find_ino(struct ext4_fs *fs, const char *path, uint32_t *number) {
    (void)fs; (void)path; *number = 7; return lookup_failure;
}
static int get_inode_struct(struct ext4_fs *fs, uint32_t number, struct ext4_inode *inode) {
    (void)fs; (void)number; *inode = stored; return inode_failure;
}
static int ext4_update_inode(struct ext4_fs *fs, uint32_t number, struct ext4_inode *inode) {
    (void)fs; (void)number;
    if (update_failure) return -1;
    stored = *inode; return 0;
}
static int ext4_write(struct vfs_mount *mnt, const char *path, uint64_t offset,
                       const void *buffer, uint32_t size) {
    (void)mnt; (void)path;
    if (write_failure || offset + size > sizeof(disk)) return -1;
    memcpy(disk + offset, buffer, size);
    if (offset + size > stored.i_size_lo) stored.i_size_lo = (uint32_t)(offset + size);
    return (int)size;
}
'''
SUFFIX = r'''
int main(void) {
    memcpy(disk, "0123456789", 10);
    if (ext4_truncate(&mount, "f", 4) || stored.i_size_lo != 4) return 1;
    if (ext4_truncate(&mount, "f", 9000) || stored.i_size_lo != 9000 || memcmp(disk, "0123", 4)) return 2;
    for (size_t i = 4; i < 9000; ++i) if (disk[i]) return 3;
    if (ext4_truncate(&mount, "f", 0) || stored.i_size_lo) return 4;
    if (ext4_truncate(&mount, "f", 9000) || stored.i_size_lo != 9000) return 5;
    for (size_t i = 0; i < 9000; ++i) if (disk[i]) return 6;
    if (ext4_truncate(&mount, "f", UINT64_C(0x100000000)) != -EFBIG) return 7;
    filesystem.read_only = 1;
    if (ext4_truncate(&mount, "f", 0) != -EROFS) return 8;
    filesystem.read_only = 0; lookup_failure = 1;
    if (ext4_truncate(&mount, "f", 0) != -ENOENT) return 9;
    lookup_failure = 0; inode_failure = 1;
    if (ext4_truncate(&mount, "f", 0) != -EIO) return 10;
    inode_failure = 0; stored.i_mode = 0x4000;
    if (ext4_truncate(&mount, "f", 0) != -EISDIR) return 11;
    stored.i_mode = 0x8000; update_failure = 1;
    if (ext4_truncate(&mount, "f", 0) != -EIO || stored.i_size_lo != 9000) return 12;
    update_failure = 0; write_failure = 1;
    if (ext4_truncate(&mount, "f", 10000) != -EIO || stored.i_size_lo != 9000) return 13;
    stored.i_size_high = 1;
    return ext4_truncate(&mount, "f", 0) == -EFBIG ? 0 : 14;
}
'''

def actual_functions(path, names):
    ast, error = clang_ast(path, 'amd64')
    if ast is None: raise AssertionError(error)
    nodes = [n for n in source_definitions(ast, path) if n['name'] in names]
    if {n['name'] for n in nodes} != names: raise AssertionError('missing provider code')
    raw = path.read_bytes()
    return nodes, '\n'.join(raw[n['range']['begin']['offset']:n['range']['end']['offset'] + n['range']['end']['tokLen']].decode() for n in nodes)

class FileLengthTests(unittest.TestCase):
    def run_fixture(self, text):
        with tempfile.TemporaryDirectory(prefix='file-length-') as temp:
            source = Path(temp) / 'fixture.c'; source.write_text(text)
            binary = Path(temp) / 'fixture'
            subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', str(source), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], check=False)
            self.assertEqual(result.returncode, 0)

    def test_write_does_not_shrink_at_position_zero(self):
        host = ROOT / 'build/host'
        host.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='file-size-source-', dir=host) as temp:
            source = ROOT / 'kernel/fs/ext4.c'
            if os.environ.get('FILE_LENGTH_BASELINE_SOURCE'):
                source = Path(temp) / 'baseline.c'
                source.write_bytes(Path(os.environ['FILE_LENGTH_BASELINE_SOURCE']).read_bytes())
            nodes, code = actual_functions(source, {'ext4_write_new_size'})
            argc = sum(n.get('kind') == 'ParmVarDecl' for n in nodes[0].get('inner', []))
            arguments = '0, 57, 8' if argc == 3 else '57, 8'
            self.run_fixture('#include <stdint.h>\n' + code + '\nint main(void) { return ext4_write_new_size(' + arguments + ') == 57 ? 0 : 1; }\n')

    def test_explicit_shrink_and_growth_zero_hidden_bytes(self):
        _, code = actual_functions(ROOT / 'kernel/fs/ext4.c', {'ext4_extend_zero', 'ext4_truncate'})
        self.run_fixture(PREFIX + code + SUFFIX)

if __name__ == '__main__':
    unittest.main()
