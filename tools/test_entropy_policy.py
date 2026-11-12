#!/usr/bin/env python3
"""Test real early entropy policy with controlled HAL availability."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
FIXTURE = r'''
#include <kernel/entropy.h>
#include <hal/entropy.h>
#include <stdint.h>
static unsigned hardware_calls, jitter_calls, success_at;
static int jitter_available;
int nxhal_entropy_hardware_random(uint64_t *value) {
    ++hardware_calls;
    if (hardware_calls != success_at) return NXHAL_ENTROPY_E_UNAVAILABLE;
    *value = UINT64_C(0x1122334455667788);
    return NXHAL_ENTROPY_OK;
}
int nxhal_entropy_jitter_sample(uint64_t *value) {
    ++jitter_calls;
    if (!jitter_available) return NXHAL_ENTROPY_E_UNAVAILABLE;
    *value = jitter_calls;
    return NXHAL_ENTROPY_OK;
}
int main(void) {
    success_at = 1;
    struct entropy_sample sample = entropy_sample_u64();
    if (sample.source != ENTROPY_SOURCE_ARCH_RNG || sample.value != UINT64_C(0x1122334455667788) || hardware_calls != 1 || jitter_calls) return 1;
    hardware_calls = jitter_calls = 0; success_at = 16;
    sample = entropy_sample_u64();
    if (sample.source != ENTROPY_SOURCE_ARCH_RNG || hardware_calls != 16 || jitter_calls) return 2;
    hardware_calls = jitter_calls = success_at = 0; jitter_available = 1;
    sample = entropy_sample_u64();
    if (sample.source != ENTROPY_SOURCE_COUNTER_MIX || hardware_calls != 16 || jitter_calls != 8) return 3;
    hardware_calls = jitter_calls = 0; jitter_available = 0;
    sample = entropy_sample_u64();
    if (sample.source != ENTROPY_SOURCE_ADDRESS_MIX || hardware_calls != 16 || jitter_calls != 8) return 4;
    hardware_calls = jitter_calls = 0; success_at = 1;
    return entropy_u64() == UINT64_C(0x1122334455667788) ? 0 : 5;
}
'''

class EntropyPolicyTests(unittest.TestCase):
    def test_provenance_and_bounded_retries(self):
        with tempfile.TemporaryDirectory(prefix="nexs-entropy-") as temp:
            source = Path(temp) / "policy_test.c"
            binary = Path(temp) / "policy_test"
            source.write_text(FIXTURE)
            subprocess.run(["cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra",
                            "-Werror", "-fsanitize=address,undefined",
                            "-I" + str(ROOT / "kernel/include"),
                            "-I" + str(ROOT / "kernel/HAL/include"),
                            str(ROOT / "kernel/lib/entropy.c"), str(source),
                            "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
if __name__ == "__main__":
    unittest.main()
