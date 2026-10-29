import math
import unittest
from emg_review import fit_mains


class ReviewTests(unittest.TestCase):
    def test_known_sinusoid_is_recovered(self):
        rows = [(i, i * 1000, 1500, 1400 + 200 * math.sin(2 * math.pi * 49.9 * i / 1000)) for i in range(2000)]
        fit = fit_mains(rows)
        self.assertAlmostEqual(fit["frequency_hz"], 49.9)
        self.assertAlmostEqual(fit["amplitude_peak_mv"], 200, places=6)
        self.assertGreater(fit["explained_variance_fraction"], .99999)

    def test_constant_and_empty_do_not_imply_a_peak(self):
        self.assertIsNone(fit_mains([]))
        self.assertIsNone(fit_mains([(i, i*1000, 1500, 1400) for i in range(100)]))


if __name__ == "__main__":
    unittest.main()
