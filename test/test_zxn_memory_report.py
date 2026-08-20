#!/usr/bin/env python3

import argparse
import importlib.machinery
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
REPORT = importlib.machinery.SourceFileLoader(
    "zxn_memory_report", str(ROOT / "tools" / "zxn-memory-report")).load_module()


MAP_TEXT = """\
__CODE_head = $5B00 ; const
__CODE_END_head = $A9D0 ; const
__DATA_head = $A9D0 ; const
__DATA_END_head = $A9F9 ; const
__BSS_head = $A9F9 ; const
__BSS_END_head = $AB8D ; const
__CODE_tail = $5B04 ; const
__Start = $5B00 ; addr, public, , zxn_crt, CODE, crt.asm:1
__code_crt_init_head = $5B04 ; const
__code_crt_init_tail = $6051 ; const
_crt_init = $5B04 ; addr, public, , zxn_crt, code_crt_init, crt.asm:2
__code_compiler_head = $6051 ; const
__code_compiler_tail = $A4D8 ; const
_main = $6051 ; addr, public, , hangman_c, code_compiler, hangman.exe.c:1
_runtime = $7000 ; addr, public, , pools_c, code_compiler, /repo/src/lib/pools.c:1
__rodata_compiler_head = $A4D8 ; const
__rodata_compiler_tail = $A9D0 ; const
_runtime_text = $A4D8 ; addr, local, , pools_c, rodata_compiler, /repo/src/lib/pools.c:4
__data_threads_head = $A9D0 ; const
__data_threads_tail = $A9D1 ; const
_thread_data = $A9D0 ; addr, public, , zxn_crt, data_threads, crt.asm:3
__data_compiler_head = $A9D1 ; const
__data_compiler_tail = $A9F9 ; const
_runtime_data = $A9D1 ; addr, local, , pools_c, data_compiler, /repo/src/lib/pools.c:2
__bss_error_head = $A9F9 ; const
__bss_error_tail = $A9FD ; const
_error_bss = $A9F9 ; addr, public, , error_runtime, bss_error, error.asm:1
__bss_compiler_head = $A9FD ; const
__bss_compiler_tail = $AB8D ; const
_runtime_bss = $A9FD ; addr, local, , pools_c, bss_compiler, /repo/src/lib/pools.c:3
_rift_zxn_arena_link_start = $A4D4 ; addr
_bump_base = $A9D2 ; addr
_bump_top = $A9D4 ; addr
_bump_cap = $A9D6 ; addr
_zxn_arena_start = $A9D8 ; addr
_zxn_arena_end = $A9DA ; addr
_zxn_arena_capacity = $A9DC ; addr
_rift_ll_base = $A9DE ; addr
_rift_ll_live_bytes = $A9E0 ; addr
_rift_ll_peak_live_bytes = $A9E2 ; addr
_rift_ll_magazine_free_bytes = $A9E4 ; addr
_rift_ll_cap = $A9E6 ; addr
"""


class ZxnMemoryReportTest(unittest.TestCase):

  def setUp(self):
    self.temporary = tempfile.TemporaryDirectory()
    self.root = Path(self.temporary.name)
    self.map = self.root / "sample.map"
    self.map.write_text(MAP_TEXT, encoding="utf-8")
    self.nex = self.root / "sample.nex"
    self.nex.write_bytes(bytes(33280))
    self.code_bin = self.root / "sample_CODE.bin"
    self.code_bin.write_bytes(bytes(20621))

  def tearDown(self):
    self.temporary.cleanup()

  def args(self, **overrides):
    values = dict(
        artifact=self.nex,
        map=self.map,
        code_bin=self.code_bin,
        build_log=None,
        memory_max=None,
        memory_reserve=None,
        stack_top=0xFF58,
        stack_size=2048,
        alignment=16,
        registers=None,
        allocator_memory=None,
        verbose=False,
        modules=False,
        json=False,
    )
    values.update(overrides)
    return argparse.Namespace(**values)

  def test_static_layout_matches_link_boundaries(self):
    report = REPORT.build_report(self.args())
    self.assertEqual(report["resident"]["bytes"], 20621)
    self.assertEqual(report["resident"]["code_and_rodata_bytes"], 20176)
    self.assertEqual(report["resident"]["initialized_data_bytes"], 41)
    self.assertEqual(report["resident"]["bss_bytes"], 404)
    self.assertEqual(report["alignment_gap_bytes"], 3)
    self.assertEqual(report["managed_arena"]["capacity_bytes"], 19400)
    self.assertEqual(report["protected_stack"]["bytes"], 2048)
    self.assertEqual(report["runtime_window"]["bytes"], 42072)
    self.assertTrue(report["code_bin_matches_resident"])
    self.assertEqual(sum(item["bytes"] for item in report["modules"]), 20621)
    origins = {item["origin"]: item["bytes"] for item in report["origins"]}
    self.assertGreater(origins["generated_program"], 0)
    self.assertGreater(origins["rift_runtime"], 0)
    self.assertEqual(report["generated_functions"][0]["name"], "main")
    self.assertEqual(report["generated_functions"][0]["bytes"], 0x7000 - 0x6051)

  def test_cap_and_reserve_are_separate_from_resident_bytes(self):
    report = REPORT.build_report(self.args(memory_max=8192, memory_reserve=4096))
    self.assertEqual(report["resident"]["bytes"], 20621)
    self.assertEqual(report["managed_arena"]["capacity_bytes"], 8192)
    self.assertEqual(report["managed_arena"]["available_before_cap_bytes"], 15304)
    self.assertEqual(report["unallocated_high_headroom_bytes"], 7112)
    self.assertEqual(report["high_memory_reserve_bytes"], 4096)

  def test_runtime_samples_decode_allocator_and_stack(self):
    memory = self.root / "memory-A9D2-22.txt"
    words = (0, 0, 0, 0xAB90, 0xF758, 19400, 0xAB90,
             96, 160, 32, 512)
    memory.write_text("".join(value.to_bytes(2, "little").hex()
                              for value in words) + "\ncommand> ",
                      encoding="utf-8")
    registers = self.root / "registers.txt"
    registers.write_text("PC=9B82 SP=FD2B AF=0000\n", encoding="utf-8")
    report = REPORT.build_report(self.args(
        allocator_memory="0xA9D2:%s" % memory,
        registers=registers))
    self.assertEqual(report["allocator_sample"]["committed_frontiers"], 512)
    self.assertEqual(report["allocator_sample"]["rift_ll_live_bytes"], 96)
    self.assertEqual(report["allocator_sample"]["rift_ll_peak_live_bytes"], 160)
    self.assertEqual(report["allocator_sample"]["uncommitted_capacity"], 18888)
    self.assertEqual(report["stack_sample"]["bytes_below_stack_top"], 557)
    self.assertEqual(report["stack_sample"]["bytes_above_stack_floor"], 1491)


if __name__ == "__main__":
  unittest.main()
