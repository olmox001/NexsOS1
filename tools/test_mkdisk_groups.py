#!/usr/bin/env python3
"""Read back real images crossing an ext4 group boundary in both layouts."""
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def u32(data, offset):
    return struct.unpack_from('<I', data, offset)[0]


class Image:
    def __init__(self, path):
        self.file = path.open('rb')
        self.file.seek(1024)
        self.base = struct.unpack_from('<Q', self.file.read(128), 32)[0] * 512
        sb = self.read(1024, 1024)
        self.block_size = 1024 << u32(sb, 24)
        self.per_group = u32(sb, 40)
        self.inode_size = struct.unpack_from('<H', sb, 88)[0]

    def read(self, offset, size):
        self.file.seek(self.base + offset)
        data = self.file.read(size)
        if len(data) != size:
            raise AssertionError('short image read')
        return data

    def block(self, number):
        return self.read(number * self.block_size, self.block_size)

    def inode(self, number):
        group, index = divmod(number - 1, self.per_group)
        table = u32(self.read(self.block_size + group * 32, 32), 8)
        return self.read(table * self.block_size + index * self.inode_size,
                         self.inode_size)

    def extent_blocks(self, raw, logical_start=0):
        magic, count, _, depth = struct.unpack_from('<HHHH', raw)
        if magic != 0xf30a or depth > 1:
            raise AssertionError('invalid extent tree')
        result = []
        for i in range(count):
            entry = raw[12 + i * 12:24 + i * 12]
            logical = u32(entry, 0)
            if logical != logical_start + len(result):
                raise AssertionError('non-contiguous logical extent range')
            if depth:
                result.extend(self.extent_blocks(self.block(u32(entry, 4)), logical))
            else:
                length, high, low = struct.unpack_from('<HHI', entry, 4)
                start = low + (high << 32)
                result.extend(range(start, start + length))
        return result

    def blocks(self, inode):
        count = (u32(inode, 4) + self.block_size - 1) // self.block_size
        if u32(inode, 32) & 0x80000:
            return self.extent_blocks(inode[40:100])
        pointers = struct.unpack_from('<15I', inode, 40)
        result = list(pointers[:min(count, 12)])
        if count > 12:
            result.extend(struct.unpack('<1024I', self.block(pointers[12])))
        if count > 1036:
            for pointer in struct.unpack('<1024I', self.block(pointers[13])):
                if pointer:
                    result.extend(struct.unpack('<1024I', self.block(pointer)))
        return result[:count]

    def contents(self, inode):
        return b''.join(self.block(b) for b in self.blocks(inode))[:u32(inode, 4)]

    def directory(self, inode):
        raw = self.contents(inode)
        offset = 0
        result = {}
        while offset + 8 <= len(raw):
            number, length, name_length, _ = struct.unpack_from('<IHBB', raw, offset)
            if length < 8:
                raise AssertionError('invalid directory record')
            if number:
                name = raw[offset + 8:offset + 8 + name_length].decode()
                result[name] = number
            offset += length
        return result


class GroupBoundaryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='nexs-mkdisk-')
        cls.work = Path(cls.temp.name)
        cls.binary = Path(os.environ.get('MKDISK_TEST_BINARY', cls.work / 'mkdisk'))
        if 'MKDISK_TEST_BINARY' not in os.environ:
            subprocess.run(['cc', '-Wall', '-Wextra', '-o', str(cls.binary),
                            str(ROOT / 'tools/mkdisk.c')], check=True)
        cls.root = cls.work / 'root'
        cls.root.mkdir()
        cls.expected = {}
        # 136 MiB crosses the 128 MiB group boundary. Individual files also
        # exercise depth-1 extents or double-indirect legacy mappings.
        for i in range(17):
            name = f'payload-{i:02d}'
            payload = bytes([17 + i]) * (8 * 1024 * 1024)
            (cls.root / name).write_bytes(payload)
            cls.expected[name] = hashlib.sha256(payload).digest()
        (cls.root / 'empty').touch()
        cls.expected['empty'] = hashlib.sha256(b'').digest()

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def verify_layout(self, layout):
        path = self.work / f'{layout}.img'
        subprocess.run([str(self.binary), str(path), '-', '-', str(self.root),
                        f'--{layout}'], check=True, stdout=subprocess.DEVNULL)
        image = Image(path)
        try:
            entries = image.directory(image.inode(2))
            crossed = False
            claimed = set(image.blocks(image.inode(2)))
            for name, expected in self.expected.items():
                inode = image.inode(entries[name])
                blocks = image.blocks(inode)
                if blocks:
                    crossed |= blocks[0] // 32768 != blocks[-1] // 32768
                for block in blocks:
                    minimum = 68 if block < 32768 else 3
                    self.assertGreaterEqual(block % 32768, minimum,
                                            f'{name}: data overlaps group metadata')
                    self.assertNotIn(block, claimed, f'{name}: reused data block')
                    claimed.add(block)
                self.assertEqual(hashlib.sha256(image.contents(inode)).digest(),
                                 expected, f'{name}: image differs from source')
            self.assertTrue(crossed, 'fixture must cross a group within a file')
        finally:
            image.file.close()
            path.unlink()

    def test_extents(self):
        self.verify_layout('extents')

    def test_legacy(self):
        self.verify_layout('legacy')


if __name__ == '__main__':
    unittest.main()
