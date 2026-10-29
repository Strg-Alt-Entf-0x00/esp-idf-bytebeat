import queue
import time
import unittest
from unittest import mock

import tools_py.bytebeat_tool as bytebeat_tool
from tools_py.bytebeat_tool import (
    DEFAULT_SAFETY_SECONDS,
    FIRMWARE_ID,
    FormulaLibrary,
    SerialWorker,
    ToolError,
    build_play_commands,
    convert_js_math,
    validate_formula,
    verify_firmware_identity,
)


class FakeSerial:
    def __init__(self, identity=FIRMWARE_ID, formula_valid=True):
        self.identity = identity
        self.formula_valid = formula_valid
        self.responses = []
        self.writes = []
        self.is_open = True
        self.closed = False

    @property
    def in_waiting(self):
        return sum(len(line) for line in self.responses)

    def reset_input_buffer(self):
        self.responses.clear()

    def write(self, data):
        self.writes.append(data)
        command = data.decode("utf-8").strip()
        if command == "TOOL_ID" and self.identity is not None:
            self.responses.append(f"{self.identity}\r\n".encode("utf-8"))
        elif command.startswith("VLD:"):
            result = "VALID" if self.formula_valid else "INVALID"
            self.responses.append(f"BYTEBEAT:FORMULA:{result}\r\n".encode("utf-8"))
        elif command.startswith("FORMULA:"):
            self.responses.extend(
                [
                    b"[SERIAL] Loading formula\r\n",
                    b"[SERIAL] Playback STARTED\r\n",
                ]
            )
        elif command == "STOP":
            self.responses.append(b"[SERIAL] Playback STOPPED\r\n")
        return len(data)

    def flush(self):
        pass

    def readline(self):
        if self.responses:
            return self.responses.pop(0)
        time.sleep(0.005)
        return b""

    def close(self):
        self.closed = True
        self.is_open = False


class BytebeatToolTests(unittest.TestCase):
    def test_controller_prevalidates_formula_before_sending_playback_commands(self):
        port = FakeSerial()
        controller = bytebeat_tool.ESP32Controller("COM_TEST")
        controller.port = port

        controller.send("t", 0, 48000, 80, 60)

        self.assertEqual(port.writes[0], b"VLD:t\n")
        self.assertEqual(port.writes[1], b"P:0\n")
        self.assertTrue(controller.playback_started)

    def test_controller_rejects_invalid_formula_without_changing_playback(self):
        port = FakeSerial(formula_valid=False)
        controller = bytebeat_tool.ESP32Controller("COM_TEST")
        controller.port = port

        with self.assertRaisesRegex(ToolError, "no playback command was sent"):
            controller.send("t?1:0", 0, 48000, 80, 60)

        self.assertEqual(port.writes, [b"VLD:t?1:0\n"])
        self.assertFalse(controller.playback_started)

    def test_cli_send_strips_infinite_safety_flag_before_controller_call(self):
        payload = {
            "formula": "t",
            "mode": 0,
            "sample_rate": 48000,
            "volume": 20,
            "duration": 5,
            "allow_infinite": False,
        }
        with mock.patch.object(bytebeat_tool, "ESP32Controller") as controller_type:
            result = bytebeat_tool._cli_send_payload("COM_TEST", payload)

        self.assertEqual(result, 0)
        controller_type.return_value.connect.assert_called_once_with()
        controller_type.return_value.send.assert_called_once_with(
            formula="t", mode=0, sample_rate=48000, volume=20, duration=5
        )
        controller_type.return_value.disconnect.assert_called_once_with()

    def test_identity_verification_resets_buffer_and_accepts_expected_id(self):
        port = FakeSerial()

        identity = verify_firmware_identity(port, timeout=0.1)

        self.assertEqual(identity, FIRMWARE_ID)
        self.assertEqual(port.writes, [b"TOOL_ID\n"])

    def test_identity_verification_returns_mismatched_firmware_for_rejection(self):
        port = FakeSerial("TOOL_ID:another-project:firmware:1")

        identity = verify_firmware_identity(port, timeout=0.1)

        self.assertEqual(identity, "TOOL_ID:another-project:firmware:1")

    def test_identity_verification_timeout_is_bounded(self):
        port = FakeSerial(identity=None)
        started = time.monotonic()

        identity = verify_firmware_identity(port, timeout=0.02)

        self.assertIsNone(identity)
        self.assertLess(time.monotonic() - started, 0.5)

    def test_formula_validator_checks_single_line_and_parentheses(self):
        self.assertEqual(validate_formula("t*(42&t>>10)"), [])
        self.assertTrue(validate_formula("sin((t)"))
        self.assertTrue(validate_formula("t)\nSTOP"))
        self.assertTrue(validate_formula(" "))

    def test_build_play_commands_sets_safety_and_never_uses_unbounded_by_default(self):
        commands = build_play_commands(
            formula="t>>4",
            mode=2,
            sample_rate=8000,
            volume=80,
            duration=DEFAULT_SAFETY_SECONDS,
        )

        self.assertEqual(
            commands,
            ["P:0", "V:204", "S:0.1667", "T:60", "M:2", "FORMULA:t>>4"],
        )

    def test_build_play_commands_rejects_bad_settings(self):
        with self.assertRaisesRegex(ValueError, "one formula line"):
            build_play_commands("t\nSTOP", 0, 8000, 80, 60)
        with self.assertRaisesRegex(ValueError, "supports modes"):
            build_play_commands("t", 3, 8000, 80, 60)
        with self.assertRaisesRegex(ValueError, "Safety timeout"):
            build_play_commands("t", 0, 8000, 80, 3601)

    def test_converter_only_replaces_known_math_names(self):
        converted, warnings = convert_js_math("Math.sin(t)+Math.PI")

        self.assertEqual(converted, "sin(t)+PI")
        self.assertTrue(warnings)

    def test_formula_library_loads_project_catalog_and_searches(self):
        library = FormulaLibrary()

        self.assertGreater(len(library.formulas), 1000)
        self.assertTrue(library.categories())
        results = library.search("42 melody")
        self.assertTrue(results)
        self.assertIn("42", results[0][1]["name"])
        with self.assertRaises(ToolError):
            library.get(-1)

    def test_connect_only_verifies_identity_and_does_not_change_playback(self):
        events = queue.Queue()
        jobs = queue.Queue()
        port = FakeSerial()
        worker = SerialWorker(
            events,
            jobs,
            serial_factory=lambda *_args, **_kwargs: port,
            boot_delay=0,
            identity_timeout=0.1,
        )
        worker.start()
        jobs.put(("connect", 1, "COM_TEST"))

        self.assertTrue(self._wait_for(events, "job_done", 1)[3])
        self.assertEqual(port.writes, [b"TOOL_ID\n"])
        self.assertTrue(worker.verified)
        self.assertEqual(worker.playback_state, "unknown")

        jobs.put(("shutdown", 2, False))
        self.assertTrue(self._wait_for(events, "closed", 2))
        worker.join(timeout=1)
        self.assertFalse(worker.is_alive())
        self.assertTrue(port.closed)

    def test_worker_sends_formula_and_confirms_stop(self):
        events = queue.Queue()
        jobs = queue.Queue()
        port = FakeSerial()
        worker = SerialWorker(
            events,
            jobs,
            serial_factory=lambda *_args, **_kwargs: port,
            boot_delay=0,
            identity_timeout=0.1,
        )
        worker.start()
        jobs.put(("connect", 1, "COM_TEST"))
        self.assertTrue(self._wait_for(events, "job_done", 1)[3])

        payload = {
            "formula": "t>>4",
            "mode": 0,
            "sample_rate": 8000,
            "volume": 80,
            "duration": 60,
        }
        jobs.put(("send", 2, payload))
        self.assertTrue(self._wait_for(events, "job_done", 2)[3])
        self.assertTrue(self._wait_for_value(events, "playback", "active"))
        self.assertTrue(worker.playback_started_by_tool)

        expected = [
            b"TOOL_ID\n",
            b"VLD:t>>4\n",
            b"P:0\n",
            b"V:204\n",
            b"S:0.1667\n",
            b"T:60\n",
            b"M:0\n",
            b"FORMULA:t>>4\n",
        ]
        self.assertEqual(port.writes, expected)

        jobs.put(("stop", 3))
        stop_result = self._wait_for(events, "job_done", 3)
        self.assertTrue(stop_result[3])
        self.assertEqual(worker.playback_state, "idle")
        self.assertFalse(worker.playback_started_by_tool)
        self.assertEqual(port.writes[-1], b"STOP\n")

        jobs.put(("shutdown", 4, False))
        self.assertTrue(self._wait_for(events, "closed", 4))
        worker.join(timeout=1)
        self.assertFalse(worker.is_alive())

    def test_worker_rejects_formula_before_sending_playback_commands(self):
        events = queue.Queue()
        jobs = queue.Queue()
        port = FakeSerial(formula_valid=False)
        worker = SerialWorker(
            events,
            jobs,
            serial_factory=lambda *_args, **_kwargs: port,
            boot_delay=0,
            identity_timeout=0.1,
        )
        worker.start()
        jobs.put(("connect", 1, "COM_TEST"))
        self.assertTrue(self._wait_for(events, "job_done", 1)[3])

        jobs.put(
            (
                "send",
                2,
                {
                    "formula": "t?1:0",
                    "mode": 0,
                    "sample_rate": 48000,
                    "volume": 80,
                    "duration": 60,
                },
            )
        )
        result = self._wait_for(events, "job_done", 2)

        self.assertFalse(result[3])
        self.assertIn("no playback command was sent", result[4])
        self.assertEqual(port.writes, [b"TOOL_ID\n", b"VLD:t?1:0\n"])
        self.assertEqual(worker.playback_state, "unknown")
        self.assertFalse(worker.playback_started_by_tool)

        jobs.put(("shutdown", 3, False))
        self.assertTrue(self._wait_for(events, "closed", 3))
        worker.join(timeout=1)

    def test_worker_closes_port_when_identity_does_not_match(self):
        events = queue.Queue()
        jobs = queue.Queue()
        port = FakeSerial("TOOL_ID:wrong:firmware:1")
        worker = SerialWorker(
            events,
            jobs,
            serial_factory=lambda *_args, **_kwargs: port,
            boot_delay=0,
            identity_timeout=0.1,
        )
        worker.start()
        jobs.put(("connect", 1, "COM_TEST"))

        result = self._wait_for(events, "job_done", 1)

        self.assertFalse(result[3])
        self.assertIn("Wrong firmware", result[4])
        self.assertTrue(port.closed)
        self.assertFalse(worker.verified)

        jobs.put(("shutdown", 2, False))
        self.assertTrue(self._wait_for(events, "closed", 2))
        worker.join(timeout=1)

    def test_disconnect_sends_stop_for_playback_started_by_this_tool(self):
        events = queue.Queue()
        jobs = queue.Queue()
        port = FakeSerial()
        worker = SerialWorker(
            events,
            jobs,
            serial_factory=lambda *_args, **_kwargs: port,
            boot_delay=0,
            identity_timeout=0.1,
        )
        worker.start()
        jobs.put(("connect", 1, "COM_TEST"))
        self.assertTrue(self._wait_for(events, "job_done", 1)[3])
        jobs.put(
            (
                "send",
                2,
                {
                    "formula": "t",
                    "mode": 0,
                    "sample_rate": 8000,
                    "volume": 80,
                    "duration": 60,
                },
            )
        )
        self.assertTrue(self._wait_for(events, "job_done", 2)[3])
        self.assertTrue(self._wait_for_value(events, "playback", "active"))

        jobs.put(("disconnect", 3, True))
        disconnect_result = self._wait_for(events, "job_done", 3)

        self.assertTrue(disconnect_result[3])
        self.assertEqual(port.writes[-1], b"STOP\n")
        self.assertTrue(port.closed)
        self.assertFalse(worker.verified)
        jobs.put(("shutdown", 4, False))
        self.assertTrue(self._wait_for(events, "closed", 4))
        worker.join(timeout=1)
        self.assertFalse(worker.is_alive())

    @staticmethod
    def _wait_for(events, kind, value, timeout=2.0):
        deadline = time.monotonic() + timeout
        retained = []
        while time.monotonic() < deadline:
            try:
                event = events.get(timeout=0.02)
            except queue.Empty:
                continue
            retained.append(event)
            if event[0] == kind and value in event[1:]:
                return event
        raise AssertionError(f"Did not receive {kind} event for {value}: {retained}")

    @classmethod
    def _wait_for_value(cls, events, kind, value, timeout=2.0):
        return cls._wait_for(events, kind, value, timeout)


if __name__ == "__main__":
    unittest.main()
