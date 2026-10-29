import unittest

from tools_py.bytebeat_tool import FIRMWARE_ID, verify_firmware_identity


class FakeSerial:
    def __init__(self, responses):
        self.responses = iter(responses)
        self.writes = []
        self.reset = False

    def reset_input_buffer(self):
        self.reset = True

    def write(self, data):
        self.writes.append(data)
        return len(data)

    def flush(self):
        pass

    def readline(self):
        try:
            return next(self.responses)
        except StopIteration:
            return b""


class SerialIdentityTests(unittest.TestCase):
    def test_accepts_exact_expected_firmware_identity(self):
        port = FakeSerial(
            [
                b"boot message\r\n",
                (FIRMWARE_ID + "\r\n").encode("ascii"),
            ]
        )

        identity = verify_firmware_identity(port)

        self.assertEqual(identity, FIRMWARE_ID)
        self.assertTrue(port.reset)
        self.assertEqual(port.writes, [b"TOOL_ID\n"])

    def test_reports_a_different_project_identity_for_rejection(self):
        port = FakeSerial([b"TOOL_ID:esp-idf-siren-synth:console_siren_dev:1\n"])

        identity = verify_firmware_identity(port)

        self.assertNotEqual(identity, FIRMWARE_ID)

    def test_returns_no_identity_after_timeout(self):
        port = FakeSerial([])

        identity = verify_firmware_identity(port, timeout=0.01)

        self.assertIsNone(identity)


if __name__ == "__main__":
    unittest.main()
