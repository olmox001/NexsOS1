#!/usr/bin/env python3
"""Mutation tests for NEXS Power12 auditors.

The mutants are parsed as real C translation units. They are not linked into
NexsOS and no test passes by searching for a renamed function token.
"""
from __future__ import annotations
import shutil
import tempfile
import unittest
from pathlib import Path

from audit_ast import analyze_translation_unit, preprocess_findings
from audit_limits import is_material_operation_id
from common import ROOT, load_policy

class Power12MutationTests(unittest.TestCase):
    def setUp(self):
        self.policy = load_policy()
        self.base = ROOT / "build" / "power12-mutants"
        self.base.mkdir(parents=True, exist_ok=True)
        self.temp = Path(tempfile.mkdtemp(prefix="case-", dir=self.base))

    def tearDown(self):
        shutil.rmtree(self.temp, ignore_errors=True)

    def analyze(self, source: str):
        path = self.temp / "mutant.c"
        path.write_text(source)
        pre = preprocess_findings(path, self.policy)
        ast, _, _ = analyze_translation_unit(path, "amd64", self.policy)
        return pre + ast

    def has(self, findings, rule):
        return any(item["rule"] == rule for item in findings)

    def test_r11_rejects_direction_and_hardware_bit_false_positives(self):
        for name in ("KEY_RIGHT", "BTN_RIGHT", "MOUSE_RIGHT",
                     "NS_PER_TICK", "VIRTIO_GPU_EVENT_DISPLAY"):
            self.assertFalse(is_material_operation_id(name), name)

    def test_r11_preserves_real_operation_and_authority_identifiers(self):
        for name in ("REG_OP_READ", "NXHAL_OP_HARDWARE_DMA_VALIDATE",
                     "OS1_RIGHT_READ", "OS1_NS_TIME",
                     "IPC_TYPE_SERVICE", "OBJ_TYPE_TIMER",
                     "SYS_OBJECT_CTL", "OS1_EVENT_FAULT"):
            self.assertTrue(is_material_operation_id(name), name)

    def test_goto_is_structural_failure(self):
        result = self.analyze("int f(int x){goto out; out:return x;}\n")
        self.assertTrue(self.has(result, "R1"), result)

    def test_direct_recursion_is_failure(self):
        # Direct recursion is visible as a self call edge during source analysis;
        # the production runner additionally computes cross-function cycles.
        path = self.temp / "mutant.c"
        path.write_text("int f(int x){return x?f(x-1):0;}\n")
        _, edges, _ = analyze_translation_unit(path, "amd64", self.policy)
        self.assertTrue(any(a == b == "f" for a, b, _ in edges), edges)

    def test_unbounded_service_loop_fails(self):
        result = self.analyze("int f(void){for(;;){} return 0;}\n")
        self.assertTrue(self.has(result, "R2"), result)

    def test_literal_bounded_for_is_proved(self):
        result = self.analyze(
            "int f(void){int x=0;for(unsigned i=0;i<4;++i){x+=(int)i;}return x;}\n")
        r2 = [item for item in result if item["rule"] == "R2"]
        self.assertFalse(r2, r2)

    def test_bounded_indexed_array_write_is_not_loop_counter_mutation(self):
        result = self.analyze(
            "int f(void){int a[7]={0};for(unsigned i=0;i<7;++i)"
            "{a[i]=1;}return a[6];}\n")
        r2 = [item for item in result if item["rule"] == "R2"]
        self.assertFalse(r2, r2)

    def test_mutating_loop_counter_in_body_remains_rejected(self):
        result = self.analyze(
            "int f(void){int a[7]={0};for(unsigned i=0;i<7;++i)"
            "{a[i]=1;i=3;}return a[6];}\n")
        self.assertTrue(self.has(result, "R2"), result)

    def test_allocator_call_fails(self):
        result = self.analyze(
            "extern void *kmalloc(unsigned long);"
            "int f(void){return kmalloc(16)!=0;}\n")
        self.assertTrue(self.has(result, "R3"), result)

    def test_long_function_fails(self):
        body = ["int f(void){", "int x=0;"]
        body.extend("x += 1;" for _ in range(52))
        body.append("return x;}")
        result = self.analyze("\n".join(body) + "\n")
        self.assertTrue(self.has(result, "R4"), result)

    def test_static_assert_fails(self):
        result = self.analyze("_Static_assert(sizeof(int)>=2,\"x\");int f(void){return 0;}\n")
        self.assertTrue(self.has(result, "R5"), result)

    def test_mutable_global_fails(self):
        result = self.analyze("int state; int f(void){return state;}\n")
        self.assertTrue(self.has(result, "R6"), result)

    def test_discarded_nonvoid_result_fails(self):
        result = self.analyze(
            "int helper(void){return 1;} int f(void){helper();return 0;}\n")
        self.assertTrue(self.has(result, "R7"), result)

    def test_function_macro_fails(self):
        result = self.analyze("#define HIDE(x) ((x)+1)\nint f(void){return HIDE(1);}\n")
        self.assertTrue(self.has(result, "R8"), result)

    def test_double_pointer_fails(self):
        result = self.analyze("int f(char **argv){return argv!=0;}\n")
        self.assertTrue(self.has(result, "R9"), result)

if __name__ == "__main__":
    unittest.main(verbosity=2)
