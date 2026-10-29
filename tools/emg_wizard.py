"""Guided EMG output characterization; does not invent input-uV calibration."""
import argparse
import csv
import json
import math
import html
from pathlib import Path
import statistics
import time
from datetime import datetime, timezone


def summarize(rows, ended, seconds, baseline=None):
    if not rows:
        return {"usable": False, "reason": "Tidak ada sampel"}
    volts = [r[3] for r in rows]
    center = statistics.mean(volts) if baseline is None else baseline
    elapsed = ended.get("duration_us", 0) / 1e6
    clipped = sum(r[2] <= 5 or r[2] >= 4090 for r in rows)
    gaps = sum(max(0, b[0] - a[0] - 1) for a, b in zip(rows, rows[1:]))
    intervals = [b[1] - a[1] for a, b in zip(rows, rows[1:])]
    rate = (len(rows) - 1) * 1e6 / (rows[-1][1] - rows[0][1]) if len(rows) > 1 and rows[-1][1] > rows[0][1] else 0
    reasons = []
    if ended.get("stopped"):
        reasons.append("Capture dihentikan operator")
    if elapsed < seconds * .95:
        reasons.append("Capture terhenti sebelum durasi selesai")
    if ended.get("lost", 0) or gaps or ended.get("acquired") != len(rows):
        reasons.append("Sampel hilang / tidak lengkap")
    if not intervals or min(intervals) <= 0 or max(intervals) > 10000 or rate < 900 or rate > 1100:
        reasons.append("Timing akuisisi di luar pemeriksaan teknis")
    acquisition_ok = not reasons
    if clipped / len(rows) > .01:
        reasons.append("ADC clipping lebih dari 1%")
    return {
        "samples": len(rows), "duration_s": elapsed, "rate_hz": round(rate, 2),
        "adc_min": min(r[2] for r in rows), "adc_max": max(r[2] for r in rows),
        "output_mean_mv": statistics.mean(volts), "output_min_mv": min(volts),
        "output_max_mv": max(volts), "output_sd_mv": statistics.pstdev(volts),
        "rms_centered_mv": math.sqrt(statistics.mean((v - center) ** 2 for v in volts)),
        "center_mv": center, "clipped_percent": clipped * 100 / len(rows),
        "lost": ended.get("lost", 0), "sequence_gaps": gaps,
        "max_interval_us": max(intervals, default=0),
        "acquisition_ok": acquisition_ok,
        "usable": not reasons, "reason": "; ".join(reasons) or "Pemeriksaan teknis lolos; bukan validasi klinis",
    }


def relative_profile(stages):
    rest = stages.get("rest")
    refs = [stages.get(f"clench_{i}") for i in range(1, 4)]
    if not rest or not rest["usable"] or not all(r and r["usable"] for r in refs):
        return {"accepted": False, "reason": "Baseline / tiga kontraksi referensi belum lengkap dan valid"}
    noise = rest["rms_centered_mv"]
    amplitudes = [math.sqrt(max(0, r["rms_centered_mv"] ** 2 - noise ** 2)) for r in refs]
    reference = statistics.median(amplitudes)
    if reference <= max(5, 2 * noise):
        return {"accepted": False, "reason": "Respons referensi belum cukup terpisah dari noise istirahat"}
    variability = statistics.pstdev(amplitudes) / statistics.mean(amplitudes)
    if variability > .30:
        return {"accepted": False, "reason": "Tiga kontraksi tidak konsisten (CV > 30%); ulang protokol"}
    normalized = {}
    for name, stage in stages.items():
        if name != "open" and stage.get("usable"):
            amplitude = math.sqrt(max(0, stage["rms_centered_mv"] ** 2 - noise ** 2))
            normalized[name] = amplitude / reference * 100
    return {"accepted": True, "kind": "relative_output_reference_only", "absolute_uv_calibrated": False,
            "baseline_output_mv": rest["output_mean_mv"], "rest_noise_mv": noise,
            "reference_output_mv": reference, "relative_scale_percent_per_mv": 100 / reference,
            "reference_cv": variability,
            "normalized_percent": normalized,
            "reason": "Profil relatif keluaran modul; bukan %MVC, gain atau amplitudo input EMG terkalibrasi"}


def self_test():
    rows = [(i, i * 1000, 1500, 1200 + (i % 2) * 2) for i in range(1000)]
    end = {"duration_us": 1000000, "lost": 0, "acquired": 1000}
    good = summarize(rows, end, 1)
    assert good["usable"] and good["rms_centered_mv"] == 1
    assert not summarize([], end, 1)["usable"]
    assert not summarize([(a,b,4095,d) for a,b,c,d in rows], end, 1)["usable"]
    assert not summarize(rows[:-10], end, 1)["usable"]
    assert not summarize(rows, {**end, "duration_us": 100000}, 1)["usable"]
    assert not summarize(rows, {**end, "stopped": True}, 1)["usable"]
    assert not relative_profile({})["accepted"]
    stages = {"rest": good}
    for i in range(1, 4):
        stages[f"clench_{i}"] = {**good, "rms_centered_mv": 20}
    profile = relative_profile(stages)
    assert profile["accepted"] and abs(profile["normalized_percent"]["clench_1"] - 100) < 1e-9
    stages["clench_3"] = {**good, "rms_centered_mv": 200}
    assert not relative_profile(stages)["accepted"]
    print("Self-test passed: empty, clipping, packet loss, cancel, baseline, normalization, repeatability")


def ask(text, exact=None):
    answer = input(text + "\n> ").strip()
    if answer.lower() in ("q", "quit", "stop"):
        raise KeyboardInterrupt
    if exact is not None and answer != exact:
        raise RuntimeError("Konfirmasi tidak cocok; tahap manusia tidak dijalankan")


class Device:
    def __init__(self, port):
        import serial
        self.serial = serial.Serial()
        self.serial.port = port
        self.serial.baudrate = 460800
        self.serial.timeout = .2
        self.serial.dtr = False
        self.serial.rts = False
        self.serial.open()
        self.buffer = b""

    def send(self, value):
        self.serial.write((value + "\n").encode())

    def lines(self):
        self.buffer += self.serial.read(self.serial.in_waiting or 1)
        while b"\n" in self.buffer:
            line, self.buffer = self.buffer.split(b"\n", 1)
            yield line.decode(errors="replace").strip()

    def identity(self):
        self.send("info")
        until = time.monotonic() + 5
        while time.monotonic() < until:
            for line in self.lines():
                try:
                    info = json.loads(line)
                except ValueError:
                    continue
                if info.get("diagnostic") == "emg-diag-1":
                    return info
        raise RuntimeError("Firmware diagnostik tidak menjawab pada 460800 baud. Upload environment emg-diagnostic.")

    def capture(self, stage, seconds, destination):
        rows = []
        self.send(f"start {stage} {seconds}")
        deadline = time.monotonic() + seconds + 10
        with destination.open("w", newline="", encoding="utf-8") as handle:
            writer = csv.writer(handle)
            writer.writerow(["sequence", "elapsed_us", "adc_count", "adc_output_mv"])
            while time.monotonic() < deadline:
                for line in self.lines():
                    if line.startswith("S,"):
                        fields = line.split(",")
                        if len(fields) != 5:
                            raise RuntimeError("Paket sampel rusak")
                        row = tuple(map(int, fields[1:]))
                        if not (0 <= row[2] <= 4095 and 0 <= row[3] <= 4000):
                            raise RuntimeError("Nilai ADC di luar format")
                        rows.append(row)
                        writer.writerow(row)
                    elif line.startswith("{"):
                        event = json.loads(line)
                        if "error" in event:
                            raise RuntimeError(str(event))
                        if event.get("ended") == stage:
                            return rows, event
        self.send("stop")
        raise RuntimeError("Capture timeout; data parsial disimpan, hasil tidak diterima")


def save_report(folder, report):
    report["relative_profile"] = relative_profile(report["stages"])
    if report["status"] != "complete":
        report["relative_profile"] = {"accepted": False, "reason": "Sesi lengkap belum selesai; tidak ada profil yang diterapkan"}
    (folder / "hasil.json").write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
    text = ["# Hasil karakterisasi EMG", "", f"Device: {report['device']['device_id']}",
            f"Lokasi: {report['site']}", f"Status sesi: {report['status']}", "",
            "Nilai mV adalah keluaran modul. Bukan amplitudo input EMG dalam uV.", "",
            "| Tahap | Sampel | Hz | Mean mV | RMS terpusat mV | Clip % | Pemeriksaan |",
            "|---|---:|---:|---:|---:|---:|---|"]
    for name, row in report["stages"].items():
        if "samples" not in row:
            continue
        text.append(f"| {name} | {row['samples']} | {row['rate_hz']:.1f} | {row['output_mean_mv']:.2f} | "
                    f"{row['rms_centered_mv']:.2f} | {row['clipped_percent']:.2f} | {row['reason']} |")
    text += ["", report["relative_profile"]["reason"], "",
             "Tidak ada koefisien yang otomatis ditulis ke firmware produksi.",
             "Pemeriksaan teknis bukan ambang diagnosis. Angka normal dari referensi client tidak dipakai untuk memaksa gain.",
             "RMS dihitung dari keluaran modul terhadap mean istirahat; sinyal open memakai mean tahapnya sendiri.",
             "Referensi tiga kontraksi nyaman bukan MVC terstandar. Satu kanal tidak mengukur simetri bilateral serentak.",
             "Raw count dan mV merupakan dua konversi ADC berurutan, bukan sampel identik."]
    if report.get("error"):
        text += ["", "Penghentian: " + report["error"]]
    (folder / "hasil.md").write_text("\n".join(text) + "\n", encoding="utf-8")


def save_trace(path, rows, title):
    if not rows:
        return
    width, height = 1000, 320
    maximum_us = max(rows[-1][1], 1)
    # Fixed ADC voltage scale preserves visible clipping and permits stage comparison.
    points = rows[::max(1, len(rows) // 1200)]
    coordinates = " ".join(f"{65+r[1]/maximum_us*905:.1f},{270-r[3]/3300*225:.1f}" for r in points)
    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 1000 320">',
             '<rect width="1000" height="320" fill="white"/>',
             f'<text x="65" y="24" font-family="Arial" font-size="16" fill="#033075">{html.escape(title)} — keluaran modul (mV)</text>']
    for value in (0, 1100, 2200, 3300):
        y = 270 - value / 3300 * 225
        parts += [f'<path d="M65 {y}H970" stroke="#d6e3ec"/>',
                  f'<text x="55" y="{y+4}" text-anchor="end" font-family="Arial" font-size="12">{value}</text>']
    for i in range(6):
        x = 65 + i * 181
        parts.append(f'<text x="{x}" y="292" text-anchor="middle" font-family="Arial" font-size="12">{maximum_us/1e6*i/5:.1f}</text>')
    parts += [f'<polyline points="{coordinates}" fill="none" stroke="#02999c" stroke-width="1.2"/>',
              '<text x="500" y="314" text-anchor="middle" font-family="Arial" font-size="12">Waktu sejak mulai (detik)</text>', '</svg>']
    path.write_text("\n".join(parts), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM20")
    parser.add_argument("--bench-only", action="store_true", help="Hanya elektroda lepas; tidak ada tahap manusia")
    parser.add_argument("--seconds", type=int, default=10, choices=range(3, 31), metavar="3-30")
    parser.add_argument("--site", choices=["unspecified", "masseter_left", "masseter_right", "temporalis_left", "temporalis_right"], default="unspecified")
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parents[2] / "output" / "emg_wizard")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    device = Device(args.port)
    report = None
    folder = None
    try:
        info = device.identity()
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S_%fZ")
        folder = args.output / (info["device_id"] + "_" + stamp)
        folder.mkdir(parents=True)
        report = {"schema": 1, "device": info, "site": args.site, "started_utc": stamp,
                  "status": "in_progress", "stages": {}, "patient_isolation_confirmed_by_operator": False}
        save_report(folder, report)
        print(f"Device {info['device_id']}. Gerak motor/jaringan nonaktif. q pada prompt; Ctrl+C atau BACK saat rekaman.")

        def record(name, instruction, seconds, baseline=None):
            ask(instruction + "\nTekan Enter jika siap; q untuk batal.")
            for value in (3, 2, 1):
                print(f"Mulai dalam {value}...", flush=True)
                time.sleep(1)
            print(f"REKAM {name}: {seconds} detik. Tombol BACK alat menghentikan capture.", flush=True)
            started_utc = datetime.now(timezone.utc).isoformat()
            rows, event = device.capture(name, seconds, folder / f"{name}.csv")
            save_trace(folder / f"{name}.svg", rows, name)
            metrics = summarize(rows, event, seconds, baseline)
            metrics["started_utc"] = started_utc
            metrics["ended_utc"] = datetime.now(timezone.utc).isoformat()
            report["stages"][name] = metrics
            save_report(folder, report)
            print(f"{name}: {metrics.get('reason')}; clipping {metrics.get('clipped_percent', 0):.2f}%", flush=True)
            if not metrics.get("acquisition_ok") or (name != "open" and not metrics["usable"]):
                raise RuntimeError("Tahap ditolak. Periksa pemasangan/saturasi; ulang wizard sebelum melanjutkan.")
            return metrics

        record("open", "TAHAP 1. Lepaskan semua elektroda dari tubuh. Jangan sentuh kontak elektroda. Ini pemeriksaan input terbuka, bukan baseline otot.", args.seconds)
        if args.bench_only:
            report["status"] = "bench_complete"
            return
        ask("TAHAP MANUSIA. Pastikan isolasi listrik dari adaptor, USB/laptop, dan alat ukur telah diverifikasi.\n"
            "Pemasangan/protokol wajah dipandu dokter/peneliti. Hentikan bila nyeri atau tidak nyaman.\n"
            "Ketik ISOLASI TERVERIFIKASI untuk melanjutkan.", "ISOLASI TERVERIFIKASI")
        report["patient_isolation_confirmed_by_operator"] = True
        if args.site == "unspecified":
            raise RuntimeError("Lokasi belum dipilih. Jalankan ulang dengan --site masseter_left atau lokasi yang sesuai.")
        rest = record("rest", "TAHAP 2. Pasang elektroda sesuai protokol dan identitas kabel yang telah diperiksa.\n"
                      "Duduk tenang, rahang rileks, jangan berbicara atau mengunyah. Pastikan kontak elektroda sudah stabil.", args.seconds)
        baseline = rest["output_mean_mv"]
        for i in range(1, 4):
            if i > 1:
                ask("Rileks dan pulihkan kenyamanan sebelum ulangan berikutnya. Enter mulai jeda 30 detik; q batal.")
                for remaining in (30, 20, 10):
                    print(f"Jeda istirahat: {remaining} detik", flush=True)
                    time.sleep(10)
            record(f"clench_{i}", f"TAHAP 3, ulangan {i}/3. Ikuti instruksi dokter untuk menggigit dengan nyaman selama 3 detik.\n"
                   "Jangan memaksakan kontraksi maksimal. Jangan ubah elektroda. Hentikan jika nyeri.", 3, baseline)
        ask("Rileks sebelum tahap aktivitas. Enter mulai jeda 30 detik; q batal.")
        for remaining in (30, 20, 10):
            print(f"Jeda istirahat: {remaining} detik", flush=True)
            time.sleep(10)
        record("chew", "TAHAP 4. Lakukan gerakan mengunyah ringan sesuai protokol dokter.\n"
               "Tidak perlu makanan untuk tes ini. Hindari menarik kabel atau menggeser elektroda.", args.seconds, baseline)
        record("recovery", "TAHAP 5. Kembali rileks, tanpa mengunyah atau berbicara.", args.seconds, baseline)
        report["status"] = "complete"
    except KeyboardInterrupt:
        if report:
            report["status"] = "cancelled"
        print("Dibatalkan. Data yang sudah direkam tetap disimpan.")
    except Exception as exc:
        if report:
            report["status"] = "failed"
            report["error"] = str(exc)
        raise
    finally:
        try:
            device.send("stop")
        except OSError:
            pass
        finally:
            device.serial.close()
        if report and folder:
            save_report(folder, report)
            print(f"Hasil tersimpan: {folder}")


if __name__ == "__main__":
    main()
