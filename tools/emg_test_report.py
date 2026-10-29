"""Build a five-page technical report from the three recorded EMG sessions."""
import csv
import hashlib
import json
from datetime import datetime, timedelta, timezone
from pathlib import Path
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from reportlab.lib import colors
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import getSampleStyleSheet, ParagraphStyle
from reportlab.platypus import SimpleDocTemplate, Paragraph, Spacer, Table, TableStyle, Image, PageBreak

ROOT = Path(__file__).resolve().parents[2]
OUTPUT = ROOT / "output/pdf/Laporan_Uji_EMG_2026-10-29.pdf"
TEMP = ROOT / "tmp/pdfs/emg_20261029"
SESSIONS = [
    ("TS-D4085CF93030_20261029T120950_805196Z", "Percobaan 1", "Adaptor eksternal tidak terpasang menurut koreksi operator setelah tes.", "complete"),
    ("TS-D4085CF93030_20261029T130610_125325Z", "Percobaan 2", "Adaptor eksternal terpasang. Tes berhenti pada kontraksi kedua.", "failed"),
    ("TS-D4085CF93030_20261029T132309_193585Z", "Percobaan 3", "Adaptor terpasang; elektroda tetap pada masseter kanan. Ulang referensi tanpa open/chew.", "complete"),
]
LABELS = {"open": "Open", "rest": "Istirahat", "clench_1": "Gigit 1", "clench_2": "Gigit 2", "clench_3": "Gigit 3", "chew": "Mengunyah", "recovery": "Pemulihan"}
NAVY, TEAL = "#033075", "#02999c"
styles = getSampleStyleSheet()
styles.add(ParagraphStyle(name="TitleMedical", fontName="Helvetica-Bold", fontSize=23, leading=28, textColor=colors.HexColor(NAVY), spaceAfter=15))
styles.add(ParagraphStyle(name="SectionMedical", fontName="Helvetica-Bold", fontSize=14, leading=18, textColor=colors.HexColor(TEAL), spaceAfter=10))
styles.add(ParagraphStyle(name="BodyMedical", fontName="Helvetica", fontSize=10, leading=15, textColor=colors.HexColor("#21364c"), spaceAfter=9))
styles.add(ParagraphStyle(name="SmallMedical", fontName="Helvetica", fontSize=8, leading=11, textColor=colors.HexColor("#52657b"), spaceAfter=7))


def para(text, style="BodyMedical"):
    return Paragraph(text, styles[style])


def table(rows, widths):
    cells = [[para(str(cell), "SmallMedical") for cell in row] for row in rows]
    result = Table(cells, colWidths=widths, repeatRows=1, hAlign="LEFT")
    result.setStyle(TableStyle([
        ("BACKGROUND", (0, 0), (-1, 0), colors.HexColor("#e3f4f4")),
        ("ROWBACKGROUNDS", (0, 1), (-1, -1), [colors.white, colors.HexColor("#f3f6fa")]),
        ("VALIGN", (0, 0), (-1, -1), "TOP"),
        ("LEFTPADDING", (0, 0), (-1, -1), 7), ("RIGHTPADDING", (0, 0), (-1, -1), 7),
        ("TOPPADDING", (0, 0), (-1, -1), 5), ("BOTTOMPADDING", (0, 0), (-1, -1), 3),
        ("LINEBELOW", (0, 0), (-1, 0), .6, colors.HexColor(TEAL)),
    ]))
    return result


def load_session(folder, expected):
    report = json.loads((folder / "hasil.json").read_text(encoding="utf-8"))
    assert report["status"] == expected
    rows = {}
    for name, metrics in report["stages"].items():
        path = folder / f"{name}.csv"
        with path.open(newline="", encoding="utf-8") as handle:
            data = list(csv.DictReader(handle))
        values = np.array([[int(r[k]) for k in ("sequence", "elapsed_us", "adc_count", "adc_output_mv")] for r in data])
        assert len(values) == metrics["samples"], (name, "sample mismatch")
        assert np.all(np.diff(values[:, 1]) > 0)
        center = np.mean(values[:, 3]) if name == "open" else report["stages"]["rest"]["output_mean_mv"]
        assert abs(float(np.sqrt(np.mean((values[:, 3] - center) ** 2))) - metrics["rms_centered_mv"]) < .001
        rows[name] = values
    return report, rows


def plot_session(report, rows, index):
    fig, axes = plt.subplots(2, 1, figsize=(9.6, 5.2), sharex=True, gridspec_kw={"height_ratios": [1.25, 1]})
    offset, boundaries, centers = 0.0, [], []
    for number, (name, values) in enumerate(rows.items()):
        t = values[:, 1] / 1e6
        duration = report["stages"][name]["duration_s"]
        # Every bin preserves min/max; no hidden clipping from stride decimation.
        for start in range(0, len(values), 10):
            block = values[start:start + 10]
            x = offset + float(np.mean(block[:, 1])) / 1e6
            axes[0].vlines(x, block[:, 3].min(), block[:, 3].max(), color=TEAL, linewidth=.6)
        clip = (values[:, 2] <= 5) | (values[:, 2] >= 4090)
        axes[0].scatter(offset + t[clip], values[clip, 3], color="#c0392b", s=4, zorder=4)
        center = report["stages"][name]["center_mv"]
        rms_t, rms_v = [], []
        for start in range(0, len(values), 250):
            block = values[start:start + 250]
            rms_t.append(offset + np.mean(block[:, 1]) / 1e6)
            rms_v.append(np.sqrt(np.mean((block[:, 3] - center) ** 2)))
        axes[1].plot(rms_t, rms_v, color=NAVY, linewidth=1.1)
        centers.append((offset + duration / 2, LABELS[name]))
        for ax in axes:
            if number % 2 == 0:
                ax.axvspan(offset, offset + duration, color="#e4f1f5", alpha=.6, zorder=0)
            if number:
                ax.axvline(offset, color="#8598ad", linestyle="--", linewidth=.7)
        boundaries.append(offset)
        offset += duration
    for x, label in centers:
        axes[0].text(x, 1.03, label, transform=axes[0].get_xaxis_transform(), ha="center", fontsize=8, rotation=25)
    axes[0].set_ylim(0, 3400)
    axes[0].set_yticks([0, 1100, 2200, 3300])
    axes[0].set_ylabel("Keluaran modul (mV)")
    axes[1].set_ylabel("RMS 250 ms (mV)")
    axes[1].set_ylim(bottom=0)
    axes[1].set_xlabel("Waktu rekaman gabungan (detik); jeda antar-tahap tidak ditampilkan")
    axes[1].set_xlim(0, offset)
    for ax in axes:
        ax.grid(axis="y", color="#d8e2eb", linewidth=.6)
        ax.spines[["top", "right"]].set_visible(False)
        ax.tick_params(labelsize=8)
        ax.yaxis.label.set_size(9)
    axes[1].xaxis.label.set_size(8)
    fig.subplots_adjust(left=.105, right=.98, top=.87, bottom=.13, hspace=.24)
    path = TEMP / f"session_{index}.png"
    fig.savefig(path, dpi=180, facecolor="white")
    plt.close(fig)
    return path


def footer(canvas, doc):
    width, height = A4
    canvas.setStrokeColor(colors.HexColor(TEAL))
    canvas.line(42, height - 33, width - 42, height - 33)
    canvas.setFont("Helvetica", 8)
    canvas.setFillColor(colors.HexColor(NAVY))
    canvas.drawString(42, height - 25, "TONGUE SMART / LAPORAN TEKNIS EMG")
    canvas.drawString(42, 24, "29 Oktober 2026 | Riset teknis - bukan validasi klinis")
    canvas.drawRightString(width - 42, 24, str(doc.page))


def main():
    TEMP.mkdir(parents=True, exist_ok=True)
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    records = []
    manifest = []
    for folder_name, title, condition, expected in SESSIONS:
        folder = ROOT / "output/emg_wizard" / folder_name
        report, rows = load_session(folder, expected)
        records.append((folder, title, condition, report, rows))
        for path in [folder / "hasil.json", *[folder / f"{name}.csv" for name in rows]]:
            manifest.append({"source": str(path.relative_to(ROOT)), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()})
    total = sum(sum(s["samples"] for s in r[3]["stages"].values()) for r in records)
    assert total == 104000, total
    story = [para("Laporan hasil uji\nkarakterisasi EMG".replace("\n", "<br/>"), "TitleMedical"),
             para("29 Oktober 2026, malam hari (UTC+8)", "SectionMedical"),
             para("Perangkat TS-D4085CF93030 (TS-01). Lokasi uji terpasang: masseter kanan. Firmware diagnostik emg-diag-1; jaringan dan pulsa motor nonaktif."),
             para("Kesimpulan: belum layak menetapkan koefisien kalibrasi", "SectionMedical"),
             para("Perekaman digital berjalan pada sekitar 1.000 Hz, tanpa kehilangan sampel yang tercatat. Namun respons antar-kontraksi tidak konsisten, satu percobaan mengalami saturasi, dan sinyal pemulihan terakhir belum kembali mendekati baseline. Kalibrasi amplitudo input dalam mikrovolt belum tercapai."),
             table([["Percobaan", "Mulai UTC+8", "Sampel", "Hasil"],
                    ["1", "20:09", "49.000", "Rangkaian selesai; profil relatif ditolak"],
                    ["2", "21:06", "26.000", "Berhenti pada Gigit 2: clipping 1,30%"],
                    ["3", "21:23", "29.000", "Selesai; tiga kontraksi tidak konsisten"]], [72, 80, 66, 293]), Spacer(1, 15),
             para("Cakupan dan batas", "SectionMedical"),
             para("Total 104.000 sampel dari tiga sesi. Sesi pertama diketahui tanpa adaptor eksternal setelah operator mengoreksi kondisi daya. Kondisi daya berbeda tidak digabung menjadi satu profil. Koneksi USB dan jalur daya aktual tidak diaudit secara elektrik dalam laporan ini."),
             para("Semua grafik memakai mV keluaran modul, bukan mikrovolt input elektroda. Nilai RMS terpusat dihitung terhadap mean tahap istirahat; open memakai mean tahapnya sendiri. Sampel mentah dipertahankan. Tidak ada koefisien dari uji ini yang ditulis ke NVS."),
             para("Operator menyatakan isolasi sudah diverifikasi sebelum tahap manusia. Pernyataan ini bukan pengujian keselamatan listrik oleh perangkat lunak.", "SmallMedical"), PageBreak()]
    notes = [
        "Input terbuka clipping 75,66%. Dengan elektroda terpasang clipping hilang, tetapi respons kontraksi tidak cukup terpisah dari noise untuk menerima profil relatif. Kondisi ini bukan bukti sensor rusak ataupun sehat.",
        "Gigit 2 memiliki 39 dari 3.000 sampel di batas ADC atas. Operator kemudian menyatakan sempat menggigit keras. Hubungan waktu tersebut masuk akal sebagai penjelasan, tetapi bukan bukti penyebab tunggal. Tahap berikutnya tidak direkam.",
        "Semua tahap bebas clipping. RMS kontraksi meningkat 103,11; 173,43; 821,94 mV, sementara pemulihan 825,03 mV. Profil relatif ditolak karena variasi tiga kontraksi melebihi batas heuristik CV 30%. Tidak ada pemaksaan baseline atau gain.",
    ]
    for index, (folder, title, condition, report, rows) in enumerate(records, 1):
        start = datetime.strptime(report["started_utc"], "%Y%m%dT%H%M%S_%fZ").replace(tzinfo=timezone.utc).astimezone(timezone(timedelta(hours=8)))
        story += [para(title, "TitleMedical"), para(f"Mulai {start:%H:%M:%S} UTC+8. {condition}"),
                  Image(str(plot_session(report, rows, index)), width=511, height=277),
                  para("Atas: rentang min/max tiap 10 sampel, skala sama untuk seluruh sesi; titik merah menandai clipping raw ADC. Bawah: RMS blok 250 ms, hanya untuk visualisasi; statistik tabel dihitung dari seluruh sampel tiap tahap.", "SmallMedical")]
        data = [["Tahap", "Sampel", "Mean mV", "RMS mV", "Clip %"]]
        for name, metrics in report["stages"].items():
            data.append([LABELS[name], f"{metrics['samples']:,}".replace(",", "."), f"{metrics['output_mean_mv']:.2f}", f"{metrics['rms_centered_mv']:.2f}", f"{metrics['clipped_percent']:.2f}"])
        story += [table(data, [121, 90, 100, 100, 100]), Spacer(1, 12), para(notes[index - 1]),
                  para(f"Sumber rekaman: {folder.name}. Seluruh tahap yang tersedia: sekitar 1.000 Hz, lost=0, sequence gaps=0.", "SmallMedical"), PageBreak()]
    story += [para("Tindak lanjut dan firmware", "TitleMedical"),
              para("1. Periksa sumber ketidakstabilan", "SectionMedical"),
              para("Lepaskan elektroda dari tubuh dan matikan alat sebelum memeriksa kabel, jack, solder dan identitas elektroda referensi. Periksa kestabilan suplai pada uji meja. Ulang pengujian satu faktor setiap kali agar penyebab perubahan dapat dibedakan. Jangan mengubah grounding pengaman."),
              para("2. Stabilisasi indikator pada firmware lengkap 0.6.1", "SectionMedical"),
              para("Rancangan pengolahan baru: x = keluaran ADC mV + offset tersimpan - baseline tersimpan. RMS = sqrt(mean(x kuadrat)) pada 25 sampel, nominal 250 ms pada 100 Hz. Lalu y = y sebelumnya + alpha x (RMS - y sebelumnya), dengan alpha = 1 - exp(-dt/150 ms). Estimasi mikrovolt = y x 1000 / gain asumsi."),
              para("Tujuan: mengurangi jitter indikator dan mengurangi ketergantungan alpha pada waktu sampling. Konsekuensi: respons lebih lambat dan makna amplitudo berubah dari rectified EMA menjadi RMS yang diperhalus. Window memakai sampel yang tersedia saat startup. Jeda akuisisi lebih dari 100 ms mereset envelope."),
              para("Data ADC/mV mentah dan pemeriksaan saturasi tetap dipertahankan. Tidak ada penyesuaian baseline otomatis, penghilangan nilai tinggi yang menetap, filter notch, atau koefisien baru dari tes malam ini. Perataan tidak menulis profil kalibrasi atau pairing ke NVS."),
              para("Verifikasi implementasi: firmware lengkap 0.6.1 berhasil dibangun dan di-upload ke COM20 dengan hash flash terverifikasi. Perangkat menjawab ID yang sama pada 115200 baud; emg_selftest dan envelope_selftest memberikan passed. Efek stabilisasi pada subjek belum diuji ulang.", "SmallMedical"),
              para("3. Batas klaim", "SectionMedical"),
              para("Stabilisasi tampilan bukan perbaikan interferensi, bukan kalibrasi gain, dan bukan validasi klinis. Produksi 100 Hz dengan jaringan aktif tidak setara dengan diagnostik 1.000 Hz tanpa jaringan. Jangan menyambung kedua kondisi sebagai satu deret amplitudo terkalibrasi."),
              para("Untuk mikrovolt absolut diperlukan sumber diferensial terukur atau pembanding tervalidasi. Gain firmware masih asumsi. Uji manusia berikutnya harus mengikuti protokol peneliti/dokter dan verifikasi keselamatan listrik."),
              para("Referensi dan keterlacakan", "SectionMedical"),
              para("Sumber primer: hasil.json dan CSV setiap tahap dari tiga sesi pada halaman 1-4. Arsip sumber tetap utuh; checksum SHA-256 disimpan dalam manifest pendamping laporan. Waktu halaman menggunakan UTC+8, nama folder menggunakan UTC.", "SmallMedical"),
              para('Seeed Studio, Grove - EMG Detector: standby nominal 1,5 V, maksimum keluaran 3,3 V, supply 3,3-5 V; produsen menyatakan bukan untuk penggunaan medis. <link href="https://wiki.seeedstudio.com/Grove-EMG_Detector/" color="#02999c">wiki.seeedstudio.com/Grove-EMG_Detector/</link>', "SmallMedical")]
    doc = SimpleDocTemplate(str(OUTPUT), pagesize=A4, leftMargin=42, rightMargin=42, topMargin=54, bottomMargin=42,
                            title="Laporan Uji Karakterisasi EMG - 29 Oktober 2026", author="Tongue Smart")
    doc.build(story, onFirstPage=footer, onLaterPages=footer)
    (OUTPUT.with_suffix(".manifest.json")).write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print(f"PDF: {OUTPUT}; {total} samples cross-checked against source summaries")


if __name__ == "__main__":
    main()
