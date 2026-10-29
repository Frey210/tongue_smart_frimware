import csv
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

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
    def test_repeat_reference_records_only_attached_stages(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(wizard, "Device") as device, \
                patch.object(wizard, "ask"), patch.object(wizard.LiveSession, "wait"), \
                patch("sys.argv", ["emg_wizard.py", "--repeat-reference", "--site", "masseter_right", "--output", directory]):
            device.return_value.identity.return_value = {"device_id": "TEST"}
            def capture(stage, seconds, destination, live):
                amplitude = 1 if stage in ("rest", "recovery") else 30
                rows = [(i, i * 1000, 1500, 1500 + amplitude * (1 if i % 2 else -1)) for i in range(seconds * 1000)]
                return rows, {"duration_us": seconds * 1000000, "acquired": len(rows), "lost": 0}
            device.return_value.capture.side_effect = capture
            wizard.main()
            stages = [call.args[0] for call in device.return_value.capture.call_args_list]
            self.assertEqual(stages, ["rest", "clench_1", "clench_2", "clench_3", "recovery"])
            report = json.loads(next(Path(directory).glob("*/hasil.json")).read_text(encoding="utf-8"))
            self.assertEqual(report["protocol"], "repeat_reference")
            self.assertEqual(report["status"], "complete")
            self.assertTrue(report["relative_profile"]["accepted"])

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

    def test_live_stop_request_prevents_capture_start(self):
        device = wizard.Device.__new__(wizard.Device)
        device.serial = FakeSerial(b"")
        device.buffer = b""
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            live = wizard.LiveSession(folder)
            live.update(stage="open", state="waiting")
            state = json.loads((folder / "live.json").read_text(encoding="utf-8"))
            self.assertEqual(state["stage"], "open")
            (folder / "stop.request").touch()
            with self.assertRaises(KeyboardInterrupt):
                device.capture("open", 1, folder / "capture.csv", live)
            self.assertEqual(device.serial.written, b"")

    def test_expected_error_returns_failure_and_closes_serial(self):
        with patch.object(wizard, "Device") as device, patch("sys.argv", ["emg_wizard.py", "--port", "TEST"]):
            device.return_value.identity.side_effect = RuntimeError("Perangkat tidak menjawab")
            self.assertEqual(wizard.main(), 1)
            device.return_value.send.assert_called_with("stop")
            device.return_value.serial.close.assert_called_once()


if __name__ == "__main__":
    unittest.main()
