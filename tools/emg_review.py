"""Read-only review of saved EMG output; preserve source CSV/hasil.json."""
import argparse
import csv
import json
import math
from pathlib import Path


def fit_mains(rows):
    n = len(rows)
    if n < 3:
        return None
    times = [row[1] / 1e6 for row in rows]
    values = [row[3] for row in rows]
    mean = sum(values) / n
    centered = [value - mean for value in values]
    total = sum(value * value for value in centered)
    if total == 0:
        return None
    best = None
    for index in range(81):
        frequency = 49 + index * .025
        cosines = [math.cos(2 * math.pi * frequency * t) for t in times]
        sines = [math.sin(2 * math.pi * frequency * t) for t in times]
        c, s = sum(cosines), sum(sines)
        cc = sum(v * v for v in cosines) - c * c / n
        ss = sum(v * v for v in sines) - s * s / n
        cs = sum(a * b for a, b in zip(cosines, sines)) - c * s / n
        yc = sum(a * b for a, b in zip(centered, cosines))
        ys = sum(a * b for a, b in zip(centered, sines))
        determinant = cc * ss - cs * cs
        if determinant <= 0:
            continue
        a = (yc * ss - ys * cs) / determinant
        b = (ys * cc - yc * cs) / determinant
        explained = min(1, max(0, (a * yc + b * ys) / total))
        if best is None or explained > best["explained_variance_fraction"]:
            best = {"frequency_hz": frequency, "amplitude_peak_mv": math.hypot(a, b),
                    "explained_variance_fraction": explained}
    return best


def review(folder):
    original = json.loads((folder / "hasil.json").read_text(encoding="utf-8"))
    if original.get("status") != "complete":
        raise ValueError("Review ini memerlukan rangkaian lengkap; lihat hasil.md untuk sesi parsial")
    findings = {}
    for stage, metrics in original["stages"].items():
        with (folder / f"{stage}.csv").open(newline="", encoding="utf-8") as handle:
            reader = csv.DictReader(handle)
            rows = [tuple(int(row[key]) for key in ("sequence", "elapsed_us", "adc_count", "adc_output_mv")) for row in reader]
        if len(rows) != metrics["samples"]:
            raise ValueError(f"CSV/summary count mismatch: {stage}")
        findings[stage] = fit_mains(rows)
    result = {"method": "least-squares DC + one sinusoid, search 49-51 Hz in 0.025 Hz steps",
              "interpretation": "A narrow ~50 Hz component suggests interference; fit is not proof of its physical source or muscle activity",
              "absolute_uv_calibrated": False, "calibration_applied": False, "stages": findings}
    (folder / "review.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    profile_status = "diterima untuk peninjauan" if original["relative_profile"]["accepted"] else "ditolak"
    lines = ["# Tinjauan hasil EMG", "", f"Perangkat: {original['device']['device_id']}",
             f"Lokasi: {original['site']}",
             "", f"Rangkaian selesai. Profil relatif {profile_status}: " + original["relative_profile"]["reason"], "",
             "| Tahap | Sampel | RMS keluaran terpusat (mV) | Clip (%) | Fit dekat 50 Hz | Variasi dijelaskan fit (%) |",
             "|---|---:|---:|---:|---:|---:|"]
    for stage, fit in findings.items():
        metrics = original["stages"][stage]
        frequency = f"{fit['frequency_hz']:.2f}" if fit else "Tidak tersedia"
        fraction = f"{fit['explained_variance_fraction']*100:.1f}" if fit else "Tidak tersedia"
        lines.append(f"| {stage} | {metrics['samples']} | {metrics['rms_centered_mv']:.2f} | "
                     f"{metrics['clipped_percent']:.2f} | {frequency} | {fraction} |")
    lines += ["", "## Arti hasil", "",
              "- Clipping input terbuka hilang setelah elektroda dipasang. Ini mendukung adanya pengaruh kondisi masukan, bukan bukti semua hardware sehat.",
              "- Komponen sempit sekitar 50 Hz menjelaskan sebagian besar variasi pada beberapa tahap. Ini indikasi kuat kontaminasi periodik; sumber fisiknya belum dibuktikan.",
              "- Persentase fit adalah R-squared satu sinusoid + DC terhadap keluaran modul. Bukan persentase noise yang sudah dipisahkan dari EMG secara pasti.",
              "- Tiga kontraksi tidak memberikan amplitudo yang konsisten terhadap baseline. Jangan memasang gain agar hasil mengikuti angka normal dari referensi client.",
              "- Seluruh data adalah ADC/mV keluaran modul. Belum ada kalibrasi gain input-uV atau diagnosis klinis.",
              "", "## Langkah berikutnya", "",
              "1. Lepaskan elektroda dari tubuh dan matikan alat sebelum memeriksa kabel, jack dan identitas elektroda referensi. Jangan pakai mode ohm/continuity pada tubuh.",
              "2. Periksa pemasangan/kontak elektroda bersama dokter, penahan kabel, dan sumber gangguan lingkungan. Jangan mengubah grounding pengaman atau menganggap isolasi pasien terbukti oleh tes ini.",
              "3. Ulang baseline setelah kontak stabil. Bandingkan rekaman ulang sebelum memilih filtering atau kompensasi; filter notch dapat mengubah amplitudo dan tidak mengkalibrasi gain.",
              "4. Gunakan sumber sinyal terukur/pembanding tervalidasi bila memerlukan uV absolut.",
              "", "Sumber: https://pubmed.ncbi.nlm.nih.gov/15320462/ (gangguan listrik 50/60 Hz berada dalam bandwidth EMG).",
              "Metode fit: least-squares pada timestamp sampel aktual, satu sinusoid dan DC, grid 49–51 Hz; angka frekuensi merupakan estimasi.",
              "CSV dan hasil.json asli tidak diubah. Firmware masih mode diagnostik; belum ada profil ditulis ke NVS."]
    (folder / "review.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("folder", type=Path)
    args = parser.parse_args()
    print(json.dumps(review(args.folder), indent=2))


if __name__ == "__main__":
    main()
