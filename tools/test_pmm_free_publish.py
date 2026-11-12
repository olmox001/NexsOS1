#!/usr/bin/env python3
"""Exercise actual PMM free against allocation at the publication boundary."""
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
#include <stdlib.h>
#define HAL_RAM_START 0
#define MEMORY_BASE 0
#define PAGE_SIZE 4096U
#define PG_RESERVED 1U
#define ZONE_DMA 0
#define ZONE_NORMAL 1
struct page { uint32_t flags, refcount; };
struct zone { uint64_t end_pfn; int lock; uint64_t *bitmap, free_pages; };
static unsigned char memory[PAGE_SIZE];
static struct page metadata[2];
static struct page *page_array = metadata;
static uint64_t bits, total_pages = 2, free_pages, stat_free_calls;
static struct zone zones[2];
static int reallocations, invalid_publication;
static uint64_t virt_to_phys(void *page) {
    return page == memory ? PAGE_SIZE : PAGE_SIZE * total_pages;
}
static uint64_t phys_to_pfn(uint64_t phys) { return phys / PAGE_SIZE; }
static int bitmap_test(uint64_t *bitmap, uint64_t pfn) { return (*bitmap >> pfn) & 1U; }
static void bitmap_clear(uint64_t *bitmap, uint64_t pfn) { *bitmap &= ~(UINT64_C(1) << pfn); }
static void spin_lock_irqsave(int *lock, uint64_t *flags) { *flags = 0; if (*lock) abort(); *lock = 1; }
static void spin_unlock_irqrestore(int *lock, uint64_t flags) {
    (void)flags;
    if (!*lock) abort();
    *lock = 0;
    /* Another CPU may allocate immediately after this unlock. */
    if (!bitmap_test(&bits, 1)) {
        for (size_t i = 0; i < PAGE_SIZE; ++i)
            if (memory[i] != 0xcc) invalid_publication = 1;
        bits |= UINT64_C(2);
        metadata[1].refcount = 1;
        memset(memory, 0xa5, sizeof(memory));
        ++reallocations;
    }
}
static void panic(const char *text, ...) { (void)text; abort(); }
#define pr_err(...) ((void)0)
#define pr_warn(...) ((void)0)
'''
SUFFIX = r'''
int main(void) {
    bits = 3;
    metadata[1].refcount = 1;
    zones[ZONE_DMA].end_pfn = 2;
    zones[ZONE_DMA].bitmap = &bits;
    memset(memory, 0x42, sizeof(memory));
    pmm_free_page(memory);
    if (invalid_publication || reallocations != 1 || metadata[1].refcount != 1) return 1;
    for (size_t i = 0; i < PAGE_SIZE; ++i)
        if (memory[i] != 0xa5) return 2;
    metadata[1].refcount = 2;
    pmm_free_page(memory);
    if (reallocations != 1 || metadata[1].refcount != 1) return 3;
    metadata[1].flags = PG_RESERVED;
    pmm_free_page(memory);
    if (reallocations != 1 || metadata[1].refcount != 1) return 4;
    pmm_free_page(NULL);
    return 0;
}
'''

class PMMFreePublicationTests(unittest.TestCase):
    def test_no_poison_after_a_new_owner_can_allocate(self):
        source = ROOT / 'kernel/mm/pmm.c'
        host = ROOT / 'build/host'
        host.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='pmm-free-', dir=host) as temp:
            work = Path(temp)
            if os.environ.get('PMM_BASELINE_SOURCE'):
                source = work / 'baseline.c'
                source.write_bytes(Path(os.environ['PMM_BASELINE_SOURCE']).read_bytes())
            ast, error = clang_ast(source, 'amd64')
            self.assertIsNotNone(ast, error)
            nodes = [n for n in source_definitions(ast, source) if n['name'] == 'pmm_free_page']
            self.assertEqual(len(nodes), 1)
            bounds = nodes[0]['range']
            code = source.read_bytes()[bounds['begin']['offset']:bounds['end']['offset'] + bounds['end']['tokLen']].decode()
            fixture = work / 'fixture.c'
            fixture.write_text(PREFIX + code + SUFFIX)
            binary = work / 'fixture'
            subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', str(fixture), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], check=False)
            self.assertEqual(result.returncode, 0, 'page contents belong to the next allocator after unlock')

if __name__ == '__main__':
    unittest.main()
