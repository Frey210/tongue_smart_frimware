from pathlib import Path
import tempfile
import unittest
from emg_live_plot import tail_rows


class TailTests(unittest.TestCase):
    def test_incremental_tail_preserves_partial_lines(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "test.csv"
            path.write_bytes(b"sequence,elapsed_us,adc_count,adc_output_mv\n0,0,1000,800\n1,1000,20")
            rows, offset, partial = tail_rows(path)
            self.assertEqual(rows, [(0, 0, 1000, 800)])
            with path.open("ab") as handle:
                handle.write(b"00,1600\n")
            rows, offset, partial = tail_rows(path, offset, partial)
            self.assertEqual(rows, [(1, 1000, 2000, 1600)])
            self.assertEqual(partial, b"")
            self.assertEqual(tail_rows(path, offset, partial)[0], [])

    def test_missing_file_is_a_waiting_state(self):
        self.assertEqual(tail_rows(Path("this_file_does_not_exist.csv")), ([], 0, b""))


if __name__ == "__main__":
    unittest.main()
