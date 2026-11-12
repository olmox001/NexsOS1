#!/usr/bin/env python3
"""Exercise the actual sampling functions with a deliberately slow logger."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/power12'))
from common import clang_ast, source_definitions

FUNCTIONS = {
    'timespec_ns', 'interval_gap', 'point_gap', 'millis_gap', 'stats_init',
    'stats_add', 'diag_init', 'diag_status', 'diag_account',
    'diag_read_post_clocks', 'diag_do_sleep', 'diag_sample',
}
RECORDS = {'sleep_stats', 'diag_state', 'diag_post_clocks'}

PREFIX = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
static unsigned long long fake_now = 1000000000ULL;
static unsigned pre_logs;
static long get_time(void) { return (long)(fake_now / 1000000ULL); }
static unsigned long long os1_mono_ns(void) { return fake_now; }
static unsigned long long os1_cpu_ns(void) { return 0ULL; }
static void OS1_sleep(int ms) { fake_now += (unsigned long long)ms * 1000000ULL; }
static int test_nanosleep(const struct timespec *request, struct timespec *remaining) {
    (void)remaining;
    fake_now += (unsigned long long)request->tv_nsec;
    return 0;
}
static int test_clock_gettime(clockid_t clock, struct timespec *value) {
    unsigned long long now = clock == CLOCK_MONOTONIC ? fake_now : 0;
    value->tv_sec = (time_t)(now / 1000000000ULL);
    value->tv_nsec = (long)(now % 1000000000ULL);
    return 0;
}
static int test_printf(const char *format, ...) {
    if (strncmp(format, "[TIMERDIAG] PRE ", 16) == 0) {
        fake_now += 300000000ULL;
        ++pre_logs;
    }
    return 1;
}
#define printf test_printf
#define nanosleep test_nanosleep
#define clock_gettime test_clock_gettime
'''

SUFFIX = r'''
int main(void) {
    struct diag_state state;
    diag_init(&state);
    diag_sample(&state, 0U);
    diag_sample(&state, 1U);
    if (state.failures || state.os1_stats.max_ns != TIMERDIAG_SLEEP_NS ||
        state.posix_stats.max_ns != TIMERDIAG_SLEEP_NS || pre_logs != 2U)
        return 1;
#ifndef TIMER_TEST_LEGACY
    diag_init(&state);
    state.quiet = 1;
    diag_sample(&state, 0U);
    diag_sample(&state, 1U);
    if (state.failures || pre_logs != 2U ||
        state.os1_stats.max_ns != TIMERDIAG_SLEEP_NS ||
        state.posix_stats.max_ns != TIMERDIAG_SLEEP_NS)
        return 2;
#endif
    return 0;
}
'''


def actual_sampling_code(path):
    ast, error = clang_ast(path, 'amd64')
    if ast is None:
        raise AssertionError(error)
    raw = path.read_bytes()
    nodes = [n for n in source_definitions(ast, path) if n['name'] in FUNCTIONS]
    found = {n['name'] for n in nodes}
    if found != FUNCTIONS:
        raise AssertionError(f'missing sampling functions: {FUNCTIONS - found}')
    records = [n for n in ast['inner'] if n.get('kind') == 'RecordDecl' and
               n.get('completeDefinition') and n.get('name') in RECORDS]
    if {n['name'] for n in records} != RECORDS:
        raise AssertionError('missing sampling state records')
    definitions = [line for line in raw.decode().splitlines() if
                   line.startswith('#define TIMERDIAG_')]
    code = ['\n'.join(definitions)]
    # Keep the source declaration order, using Clang offsets rather than a
    # second hand-maintained implementation of the measurement algorithm.
    for node in sorted(nodes + records, key=lambda n: n['range']['begin']['offset']):
        bounds = node['range']
        start = bounds['begin']['offset']
        end = bounds['end']['offset'] + bounds['end']['tokLen']
        text = raw[start:end].decode()
        code.append(text + (';' if node['kind'] == 'RecordDecl' else ''))
    return '\n'.join(code)


class SamplingTests(unittest.TestCase):
    def test_logging_is_outside_measured_interval(self):
        host = ROOT / 'build/host'
        host.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='timer-sampling-', dir=host) as temp:
            work = Path(temp)
            override = os.environ.get('TIMERDIAG_BASELINE_SOURCE')
            source = ROOT / 'user/sys/bin/nxtimerdiag.c'
            if override:
                source = work / 'baseline.c'
                source.write_bytes(Path(override).read_bytes())
            fixture = work / 'sampling.c'
            fixture.write_text(PREFIX + actual_sampling_code(source) + SUFFIX)
            binary = work / 'sampling'
            command = ['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                       '-fsanitize=address,undefined', str(fixture), '-o', str(binary)]
            if override:
                command.insert(1, '-DTIMER_TEST_LEGACY=1')
            subprocess.run(command, check=True)
            result = subprocess.run([str(binary)], check=False)
            self.assertEqual(result.returncode, 0,
                             'a 300 ms PRE logger must not inflate a 33 ms sleep')


if __name__ == '__main__':
    unittest.main()
