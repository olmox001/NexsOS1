#!/usr/bin/env python3
"""Mutation controls for the real NXPAX Power12 Clang AST gate.

A strict checker is not credible unless known-incompatible real C functions
cause the appropriate AST findings. None of these test functions are linked
into the operating system; they are compiled for the audit itself.
"""
from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "power12_audit", ROOT / "tools" / "power12_audit.py")
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("Power12 checker could not be loaded")
AUDIT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(AUDIT)


class Power12MutationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="nxpax-power12-test-")
        self.old_root = AUDIT.ROOT
        AUDIT.ROOT = Path(self.temp.name)

    def tearDown(self):
        AUDIT.ROOT = self.old_root
        self.temp.cleanup()

    def run_source(self, source):
        name = "verified_target.c"
        (AUDIT.ROOT / name).write_text(source)
        edges, findings, checked = {}, [], []
        AUDIT.check_source(name, edges, findings, checked)
        for function in AUDIT.cycles(edges):
            for path, symbol, line in checked:
                if symbol == function:
                    findings.append(("R1", path, line, "recursive call cycle"))
        self.assertTrue(checked, "a valid sample must yield AST function bodies")
        self.assertFalse(any(rule == "R10" for rule, *_ in findings),
                         f"Clang parsing failed: {findings}")
        return findings

    def test_goto_cannot_be_hidden_by_name(self):
        findings = self.run_source(
            "int ordinary_function(int x){goto cleanup; cleanup: return x;}\n")
        self.assertTrue(any(rule == "R1" for rule, *_ in findings), findings)

    def test_recursive_call_cycle_is_detected(self):
        findings = self.run_source(
            "int sum_down(int x){return x>0?sum_down(x-1)+x:0;}\n")
        self.assertTrue(any(rule == "R1" for rule, *_ in findings), findings)

    def test_bounded_real_for_does_not_fail_rule_two(self):
        findings = self.run_source(
            "int bounded_work(void){int x=0;"
            "for(unsigned i=0;i<4;++i){++x;} return x;}\n")
        self.assertFalse(any(rule == "R2" for rule, *_ in findings), findings)

    def test_mutating_bound_counter_rejects_false_proof(self):
        findings = self.run_source(
            "int deceptive_loop(void){int x=0;"
            "for(unsigned i=0;i<4;++i){i=0;++x;} return x;}\n")
        self.assertTrue(any(rule == "R2" for rule, *_ in findings), findings)

    def test_while_loop_requires_independent_proof(self):
        findings = self.run_source(
            "int uncontrolled_work(int x){while(x){x=x-1;}return x;}\n")
        self.assertTrue(any(rule == "R2" for rule, *_ in findings), findings)

    def test_real_heap_call_detected_by_call_edge(self):
        findings = self.run_source(
            "extern void *malloc(unsigned long);"
            "int use_heap(void){return malloc(16)!=0;}\n")
        self.assertTrue(any(rule == "R3" for rule, *_ in findings), findings)

    def test_function_physical_size_measured(self):
        body = "".join("x += 1;\n" for _ in range(53))
        findings = self.run_source(
            "int too_long(int x){\n" + body + "return x;\n}\n")
        self.assertTrue(any(rule == "R4" for rule, *_ in findings), findings)

    def test_token_pasting_macro_rejected(self):
        findings = self.run_source(
            "#define CONCAT(a,b) a##b\nint ordinary(void){return 0;}\n")
        self.assertTrue(any(rule == "R8" for rule, *_ in findings), findings)

    def test_double_indirection_is_not_renamed_away(self):
        findings = self.run_source(
            "int safe_sounding_name(char **ptr){return *ptr != 0;}\n")
        self.assertTrue(any(rule == "R9" for rule, *_ in findings), findings)


if __name__ == "__main__":
    unittest.main(verbosity=2)
