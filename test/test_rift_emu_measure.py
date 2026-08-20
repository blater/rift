#!/usr/bin/env python3
"""Focused tests for bounded ZRCP allocator measurement runs."""

import contextlib
import importlib.machinery
import io
from pathlib import Path
import sys
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
RIFT_EMU_PATH = ROOT / "tools" / "rift-emu"
RIFT_EMU = importlib.machinery.SourceFileLoader(
    "rift_emu_measure", str(RIFT_EMU_PATH)).load_module()


class RiftEmuMeasureTest(unittest.TestCase):
  def parse(self, opcode_limit):
    argv = [
        "rift-emu", "measure", "program.nex", "--target", "zxn",
        "--gate-address", "0x7000", "--breakpoint", "0x7100",
        "--opcode-limit", opcode_limit,
    ]
    with mock.patch.object(sys, "argv", argv):
      return RIFT_EMU.parse_args()

  def test_positive_opcode_limit_is_parsed(self):
    self.assertEqual(self.parse("37").opcode_limit, 37)

  def test_non_positive_opcode_limits_are_rejected(self):
    for value in ("0", "-1"):
      with self.subTest(value=value), contextlib.redirect_stderr(io.StringIO()):
        with self.assertRaises(SystemExit):
          self.parse(value)

  def test_gate_and_endpoint_construct_only_finite_runs(self):
    source = RIFT_EMU_PATH.read_text(encoding="utf-8")
    self.assertEqual(source.count("bounded_run_command(args.opcode_limit)"), 2)
    self.assertNotIn('zrcp_command(adapter.port, "run no-stop-on-data"',
                     source)
    self.assertEqual(RIFT_EMU.bounded_run_command(1234),
                     "run 1234 no-stop-on-data")
    self.assertEqual(
        source.count("finally:\n    stop_owned_process(adapter, process)"), 1)

  def assert_failure_cleans_owned_process(self, output, message):
    adapter = mock.Mock()
    process = mock.Mock()
    process.poll.return_value = None
    process.wait.return_value = 0
    with self.assertRaisesRegex(
        RIFT_EMU.LauncherError, message):
      try:
        RIFT_EMU.verify_bounded_run_stop(
            output, "PC=7100", 0x7100, "benchmark endpoint")
      finally:
        RIFT_EMU.stop_owned_process(adapter, process)
    adapter.stop.assert_called_once_with()
    process.wait.assert_called_once_with(timeout=2)

  def test_opcode_limit_exhaustion_cleans_owned_process(self):
    self.assert_failure_cleans_owned_process(
        "Returning after 123 opcodes\ncommand@cpu-step>",
        "within 123 opcodes")

  def test_missing_breakpoint_cleans_owned_process(self):
    self.assert_failure_cleans_owned_process(
        "Running until a breakpoint\ncommand@cpu-step>",
        "returned without a breakpoint")

  def test_cleanup_uses_sigterm_after_graceful_timeout(self):
    adapter = mock.Mock()
    process = mock.Mock()
    process.pid = 123
    process.poll.return_value = None
    process.wait.side_effect = [
        RIFT_EMU.subprocess.TimeoutExpired("zesarux", 2), 0]
    with mock.patch.object(RIFT_EMU.os, "killpg") as killpg:
      RIFT_EMU.stop_owned_process(adapter, process)
    adapter.stop.assert_called_once_with()
    killpg.assert_called_once_with(123, RIFT_EMU.signal.SIGTERM)
    self.assertEqual(process.wait.call_count, 2)

  def test_cleanup_cannot_mask_measurement_failure(self):
    adapter = mock.Mock()
    adapter.stop.side_effect = RuntimeError("stop failed")
    process = mock.Mock()
    process.pid = 123
    process.poll.side_effect = RuntimeError("poll failed")
    process.wait.side_effect = RuntimeError("wait failed")
    with mock.patch.object(
        RIFT_EMU.os, "killpg", side_effect=RuntimeError("signal failed")):
      with self.assertRaisesRegex(
          RIFT_EMU.LauncherError, "returned without a breakpoint"):
        try:
          RIFT_EMU.verify_bounded_run_response(
              "command@cpu-step>", 0x7100, "benchmark endpoint")
        finally:
          RIFT_EMU.stop_owned_process(adapter, process)

  def test_breakpoint_at_expected_pc_is_accepted(self):
    RIFT_EMU.verify_bounded_run_stop(
        "Breakpoint fired: PC=28928\ncommand@cpu-step>",
        "AF=0000 PC=7100 SP=ff00", 0x7100, "benchmark endpoint")

  def test_breakpoint_requires_expected_pc(self):
    with self.assertRaisesRegex(
        RIFT_EMU.LauncherError, "did not stop at 0x7100"):
      RIFT_EMU.verify_bounded_run_stop(
          "Breakpoint fired: PC=28928\ncommand@cpu-step>",
          "AF=0000 PC=7101 SP=ff00", 0x7100, "benchmark endpoint")


if __name__ == "__main__":
  unittest.main()
