import csv
import json
from pathlib import Path
import tempfile
import unittest

import emg_wizard as wizard


class FakeSerial:
    def __init__(self, payload):
        self.payload = payload
        self.written = b""

    @property
    def in_waiting(self):
        return len(self.payload)

    def read(self, count):
        result, self.payload = self.payload[:count], self.payload[count:]
        return result

    def write(self, data):
        self.written += data


class WizardTests(unittest.TestCase):
    def test_quality_and_normalization_checks(self):
        wizard.self_test()

    def test_serial_capture_preserves_rows_and_csv(self):
        lines = ['{"started":"open","seconds":1}']
        lines += [f"S,{i},{i*1000},1500,1200" for i in range(1000)]
        lines += [json.dumps({"ended": "open", "acquired": 1000, "lost": 0, "duration_us": 1000000})]
        device = wizard.Device.__new__(wizard.Device)
        device.serial = FakeSerial(("\n".join(lines) + "\n").encode())
        device.buffer = b""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "capture.csv"
            rows, event = device.capture("open", 1, path)
            self.assertEqual(len(rows), 1000)
            self.assertEqual(device.serial.written, b"start open 1\n")
            self.assertTrue(wizard.summarize(rows, event, 1)["usable"])
            with path.open(newline="", encoding="utf-8") as handle:
                self.assertEqual(len(list(csv.reader(handle))), 1001)

    def test_capture_rejects_firmware_error(self):
        device = wizard.Device.__new__(wizard.Device)
        device.serial = FakeSerial(b'{"error":"invalid_start_or_busy"}\n')
        device.buffer = b""
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(RuntimeError, "invalid_start_or_busy"):
                device.capture("open", 1, Path(directory) / "partial.csv")

    def test_bad_timing_is_not_accepted(self):
        rows = [(i, i * 2000, 1500, 1200) for i in range(500)]
        event = {"duration_us": 1000000, "acquired": 500, "lost": 0}
        self.assertFalse(wizard.summarize(rows, event, 1)["usable"])

    def test_cancelled_session_has_no_profile(self):
        with tempfile.TemporaryDirectory() as directory:
            report = {"device": {"device_id": "TEST"}, "site": "masseter_right", "status": "cancelled", "stages": {}}
            wizard.save_report(Path(directory), report)
            self.assertFalse(report["relative_profile"]["accepted"])
            saved = json.loads((Path(directory) / "hasil.json").read_text(encoding="utf-8"))
            self.assertEqual(saved["status"], "cancelled")


if __name__ == "__main__":
    unittest.main()
