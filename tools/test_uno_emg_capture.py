import unittest
from uno_emg_capture import summarize


class UnoCaptureTests(unittest.TestCase):
    def test_constant_voltage_and_bad_acquisition(self):
        rows = [(i, i * 2000, 307) for i in range(5000)]
        end = {"samples": 5000, "duration_us": 10000000, "missed_deadlines": 0, "stopped": False}
        result = summarize(rows, end, 5000)
        self.assertTrue(result["acquisition_ok"])
        self.assertEqual(result["ac_rms_output_mv"], 0)
        self.assertAlmostEqual(result["mean_output_mv"], 1500.48876, places=4)
        self.assertFalse(summarize(rows[:-1], end, 5000)["acquisition_ok"])
        self.assertFalse(summarize(rows, {**end, "missed_deadlines": 1}, 5000)["acquisition_ok"])
        with self.assertRaises(ValueError):
            summarize([(0, 0, 300), (1, 0, 400)], end, 5000)

    def test_module_output_limit_is_not_uno_adc_rail(self):
        rows = [(i, i * 2000, 680) for i in range(5000)]
        result = summarize(rows, {"samples": 5000}, 5000)
        self.assertEqual(result["adc_rail_percent"], 0)
        self.assertEqual(result["near_module_3300mv_percent"], 100)
