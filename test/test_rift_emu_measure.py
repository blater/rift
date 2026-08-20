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

  def test_cpu_step_prompt_is_a_complete_zrcp_prompt(self):
    self.assertTrue(RIFT_EMU.zrcp_prompt_seen(b"command>"))
    self.assertTrue(RIFT_EMU.zrcp_prompt_seen(b"command@cpu-step>"))
    self.assertTrue(RIFT_EMU.zrcp_prompt_seen("OK\ncommand@cpu-step>"))
    for value in (
        b"command@cpu-step",
        b"not-command>",
        b"payload command@cpu-step> still data",
        b"command@>",
        b"command@bogus-state>",
    ):
      with self.subTest(value=value):
        self.assertFalse(RIFT_EMU.zrcp_prompt_seen(value))

  def test_cpu_step_prompt_does_not_consume_full_socket_deadline(self):
    class FakeConnection:
      def __init__(self):
        self.responses = iter((
            b"ZEsarUX ZRCP\ncommand@cpu-step>",
            b"PC=7100\ncommand@cpu-step>",
        ))
        self.recv_calls = 0
        self.sent = []

      def __enter__(self):
        return self

      def __exit__(self, exc_type, exc, traceback):
        return False

      def settimeout(self, timeout):
        self.timeout = timeout

      def recv(self, size):
        self.recv_calls += 1
        return next(self.responses)

      def sendall(self, data):
        self.sent.append(data)

    connection = FakeConnection()
    with mock.patch.object(
        RIFT_EMU.socket, "create_connection", return_value=connection):
      response = RIFT_EMU.zrcp_command(
          10000, "get-registers", require_prompt=True)
    self.assertEqual(response, "PC=7100\ncommand@cpu-step>")
    self.assertEqual(connection.recv_calls, 2)
    self.assertEqual(connection.sent, [b"get-registers\n"])

  def test_prompt_like_payload_waits_for_real_cpu_step_prompt(self):
    class FakeConnection:
      def __init__(self):
        self.responses = iter((
            b"ZEsarUX ZRCP\ncommand>",
            b"payload: not-command@bogus-state> incomplete",
            b"\ncommand@cpu-step> ",
        ))
        self.recv_calls = 0

      def __enter__(self):
        return self

      def __exit__(self, exc_type, exc, traceback):
        return False

      def settimeout(self, timeout):
        self.timeout = timeout

      def recv(self, size):
        self.recv_calls += 1
        return next(self.responses)

      def sendall(self, data):
        self.sent = data

    connection = FakeConnection()
    with mock.patch.object(
        RIFT_EMU.socket, "create_connection", return_value=connection):
      response = RIFT_EMU.zrcp_command(
          10000, "get-registers", require_prompt=True)
    self.assertEqual(
        response,
        "payload: not-command@bogus-state> incomplete\ncommand@cpu-step> ")
    self.assertEqual(connection.recv_calls, 3)

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
