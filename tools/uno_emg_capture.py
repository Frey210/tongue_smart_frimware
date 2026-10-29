"""Capture ten seconds of standalone Uno EMG bench output, never input-referred uV."""
import argparse
import csv
from datetime import datetime, timezone
import json
from pathlib import Path
import statistics
import time


def summarize(rows, end, vref_mv):
    if len(rows) < 2:
        raise ValueError("Tidak cukup sampel")
    if any(not 0 <= r[2] <= 1023 for r in rows):
        raise ValueError("ADC di luar 10 bit")
    intervals = [b[1] - a[1] for a, b in zip(rows, rows[1:])]
    if min(intervals) <= 0:
        raise ValueError("Timestamp tidak meningkat")
    rate = (len(rows) - 1) * 1e6 / (rows[-1][1] - rows[0][1])
    gaps = sum(b[0] - a[0] != 1 for a, b in zip(rows, rows[1:]))
    mv = [r[2] * vref_mv / 1023 for r in rows]
    return {"samples": len(rows), "rate_hz": rate, "vref_assumed_mv": vref_mv,
            "mean_output_mv": statistics.mean(mv), "min_output_mv": min(mv), "max_output_mv": max(mv),
            "ac_rms_output_mv": statistics.pstdev(mv),
            "adc_min": min(r[2] for r in rows), "adc_max": max(r[2] for r in rows),
            "adc_rail_percent": 100 * sum(r[2] <= 1 or r[2] >= 1022 for r in rows) / len(rows),
            "near_module_3300mv_percent": 100 * sum(v >= 3250 for v in mv) / len(rows),
            "sequence_gaps": gaps, "max_interval_us": max(intervals),
            "acquisition_ok": not end.get("stopped", True) and end.get("samples") == len(rows)
                and end.get("missed_deadlines", -1) == 0 and not gaps
                and rows[0][0] == 0 and end.get("duration_us", 0) >= 9500000
                and 475 <= rate <= 525 and max(intervals) <= 4000,
            "absolute_uv_calibrated": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--vref-mv", type=float, default=5000)
    parser.add_argument("--bench-ready", action="store_true", help="Operator confirms disconnected electrodes and sensor isolated from ESP wiring")
    args = parser.parse_args()
    if not args.bench_ready or not 4500 <= args.vref_mv <= 5500:
        parser.error("Uji meja wajib dikonfirmasi --bench-ready; Vref harus 4500-5500 mV")
    import serial
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S_%fZ")
    folder = Path(__file__).resolve().parents[2] / "output/uno_emg" / stamp
    folder.mkdir(parents=True)
    rows, end, info = [], {}, {}
    with serial.Serial(args.port, 230400, timeout=.2) as device:
        time.sleep(2) # Uno normally resets on USB serial open.
        device.reset_input_buffer()
        device.write(b"info\n")
        deadline = time.monotonic() + 4
        while time.monotonic() < deadline:
            line = device.readline().decode(errors="replace").strip()
            if line.startswith('{'):
                info = json.loads(line)
                if info.get("diagnostic") == "uno-emg-1": break
        if info.get("diagnostic") != "uno-emg-1":
            raise RuntimeError("Uno diagnostic tidak menjawab; capture tidak dimulai")
        print("Rekam uji meja 10 detik; jangan sentuh elektroda.", flush=True)
        try:
            with (folder / "bench.csv").open("w", newline="", encoding="utf-8") as handle:
                writer = csv.writer(handle)
                writer.writerow(["sequence", "elapsed_us", "adc_count"])
                device.write(b"start\n")
                deadline = time.monotonic() + 15
                while time.monotonic() < deadline:
                    line = device.readline().decode(errors="replace").strip()
                    if line.startswith("S,"):
                        fields = line.split(',')
                        if len(fields) != 4: raise ValueError("Paket rusak")
                        row = tuple(map(int, fields[1:]))
                        rows.append(row)
                        writer.writerow(row)
                    elif line.startswith('{'):
                        event = json.loads(line)
                        if "error" in event: raise RuntimeError(str(event))
                        if event.get("ended"):
                            end = event
                            break
                else: raise RuntimeError("Timeout; CSV parsial dipertahankan")
        finally:
            device.write(b"stop\n")
    result = {"board": info, "port": args.port, "condition": "bench, electrodes disconnected from body; operator confirmed",
              "ended": end, **summarize(rows, end, args.vref_mv)}
    (folder / "hasil.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    t = [r[1] / 1e6 for r in rows]
    mv = [r[2] * args.vref_mv / 1023 for r in rows]
    fig, axes = plt.subplots(2, 1, figsize=(11, 6))
    for ax in axes:
        ax.plot(t, mv, color="#02999c", linewidth=.7)
        ax.set_ylabel("Keluaran modul (mV estimasi)")
        ax.set_xlabel("Waktu (detik)")
        ax.set_ylim(0, args.vref_mv)
        ax.axhline(3300, color="#c0392b", linestyle="--", linewidth=.8, label="Maks. nominal modul 3300 mV")
        ax.grid(alpha=.2)
    axes[0].set_title("Uno R3 / A0 / uji meja elektroda lepas / Vref asumsi " + str(args.vref_mv) + " mV")
    axes[0].legend(loc="upper right", fontsize=8)
    axes[1].set_xlim(0, .5)
    axes[1].set_title("Detail 0-500 ms")
    fig.tight_layout()
    fig.savefig(folder / "grafik.png", dpi=160)
    plt.close(fig)
    print(json.dumps(result, indent=2))
    print("Hasil: " + str(folder))


if __name__ == "__main__":
    main()
