"""Live read-only plot of wizard CSV files. Only emg_wizard owns the serial port."""
import argparse
from collections import deque
import json
from pathlib import Path
import tkinter as tk
from tkinter import ttk


STAGES = {"open", "rest", "clench_1", "clench_2", "clench_3", "chew", "recovery"}
TERMINAL = {"complete", "bench_complete", "failed", "cancelled"}


def tail_rows(path, position=0, partial=b""):
    try:
        with path.open("rb") as handle:
            handle.seek(position)
            block = handle.read()
            position = handle.tell()
    except FileNotFoundError:
        return [], position, partial
    lines = (partial + block).split(b"\n")
    rows = []
    for line in lines[:-1]:
        fields = line.strip().split(b",")
        if len(fields) != 4 or not fields[0].isdigit():
            continue
        try:
            row = tuple(map(int, fields))
        except ValueError:
            continue
        if 0 <= row[2] <= 4095 and 0 <= row[3] <= 4000:
            rows.append(row)
    return rows, position, lines[-1]


class LivePlot:
    def __init__(self, root, folder):
        from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg, NavigationToolbar2Tk
        from matplotlib.figure import Figure
        self.root, self.folder = root, folder
        self.stage = None
        self.position, self.partial = 0, b""
        self.rows = deque(maxlen=20000)
        self.total, self.clipped = 0, 0
        self.paused = False
        self.dirty = True
        self.last_window = None
        self.last_baseline = None
        self.state = {}
        root.title("Tongue Smart — Grafik EMG live")
        root.geometry("1100x780")
        root.minsize(850, 650)
        root.configure(bg="#f4f8fb")
        root.protocol("WM_DELETE_WINDOW", self.close)
        style = ttk.Style(root)
        style.theme_use("clam")
        style.configure("TFrame", background="#f4f8fb")
        style.configure("TLabel", background="#f4f8fb", foreground="#17354f", font=("Segoe UI", 10))
        frame = ttk.Frame(root, padding=16)
        frame.pack(fill="both", expand=True)
        ttk.Label(frame, text="Tongue Smart · EMG live", font=("Segoe UI", 17, "bold")).pack(anchor="w")
        self.identity = ttk.Label(frame, text="Menunggu identitas perangkat")
        self.identity.pack(anchor="w", pady=(4, 0))
        self.power = ttk.Label(frame, text="")
        self.power.pack(anchor="w")
        self.status = ttk.Label(frame, text="Menunggu wizard", wraplength=1000, font=("Segoe UI", 11, "bold"))
        self.status.pack(anchor="w", pady=(10, 5))
        self.metrics = ttk.Label(frame, text="Belum ada sampel. Nilai mV adalah keluaran modul, bukan µV input.")
        self.metrics.pack(anchor="w", pady=(0, 8))
        controls = ttk.Frame(frame)
        controls.pack(fill="x", pady=(0, 8))
        ttk.Label(controls, text="Jendela waktu:").pack(side="left")
        self.window = tk.StringVar(value="2")
        chooser = ttk.Combobox(controls, textvariable=self.window, values=["1", "2", "5", "10"], state="readonly", width=4)
        chooser.pack(side="left", padx=(5, 0))
        ttk.Label(controls, text="detik").pack(side="left", padx=(4, 16))
        self.pause_button = ttk.Button(controls, text="Jeda grafik", command=self.toggle_pause)
        self.pause_button.pack(side="left")
        self.stop_button = tk.Button(controls, text="Hentikan tes", command=self.stop, bg="#a92525", fg="white", relief="flat", padx=14, pady=5)
        self.stop_button.pack(side="right")
        self.figure = Figure(figsize=(10, 5), dpi=100, facecolor="#f4f8fb", layout="constrained")
        self.voltage = self.figure.add_subplot(211)
        self.adc = self.figure.add_subplot(212, sharex=self.voltage)
        for axis in (self.voltage, self.adc):
            axis.set_facecolor("white")
            axis.grid(True, color="#d6e3ec", linewidth=.6)
            axis.tick_params(labelsize=9)
            axis.spines[["top", "right"]].set_visible(False)
        self.voltage.set_ylabel("Keluaran modul (mV)")
        self.voltage.set_ylim(0, 3300)
        self.voltage.set_yticks([0, 1100, 2200, 3300])
        self.adc.set_ylabel("ADC mentah (count)")
        self.adc.set_ylim(0, 4095)
        self.adc.set_yticks([0, 1024, 2048, 3072, 4095])
        self.adc.set_xlabel("Waktu sejak mulai tahap (detik)")
        self.adc.axhline(4090, color="#a92525", linestyle=":", linewidth=.9, label="Batas clipping ADC")
        self.adc.axhline(5, color="#a92525", linestyle=":", linewidth=.9)
        self.voltage_line, = self.voltage.plot([], [], color="#02999c", linewidth=1, label="Tegangan ADC")
        self.adc_line, = self.adc.plot([], [], color="#033075", linewidth=1, label="ADC mentah")
        self.baseline_line = self.voltage.axhline(0, color="#946300", linestyle="--", linewidth=1, visible=False)
        self.voltage.legend(loc="upper right", fontsize=8)
        self.adc.legend(loc="upper right", fontsize=8)
        self.canvas = FigureCanvasTkAgg(self.figure, master=frame)
        self.canvas.get_tk_widget().pack(fill="both", expand=True)
        self.toolbar = NavigationToolbar2Tk(self.canvas, frame, pack_toolbar=False)
        self.toolbar.update()
        self.toolbar.pack(fill="x")
        ttk.Label(frame, text="Jeda grafik dahulu untuk zoom/pan. Menutup jendela membatalkan tes aktif; data tetap disimpan.").pack(anchor="w", pady=(5, 0))
        self.canvas.draw()
        self.root.after(100, self.poll)

    def toggle_pause(self):
        self.paused = not self.paused
        self.dirty = True
        self.pause_button.configure(text="Lanjutkan grafik" if self.paused else "Jeda grafik")

    def stop(self):
        if self.state.get("state") not in TERMINAL:
            (self.folder / "stop.request").touch()
            self.status.configure(text="Permintaan berhenti dikirim. Data yang sudah direkam tetap disimpan.")

    def close(self):
        self.stop()
        self.root.destroy()

    def poll(self):
        try:
            self.state = json.loads((self.folder / "live.json").read_text(encoding="utf-8"))
        except (OSError, ValueError):
            self.root.after(100, self.poll)
            return
        state = self.state
        stage = state.get("stage", "idle")
        if stage != self.stage:
            self.stage = stage
            self.position, self.partial = 0, b""
            self.rows.clear()
            self.total, self.clipped = 0, 0
            self.dirty = True
        fresh = []
        if stage in STAGES:
            fresh, self.position, self.partial = tail_rows(self.folder / f"{stage}.csv", self.position, self.partial)
            self.rows.extend(fresh)
            self.total += len(fresh)
            self.clipped += sum(row[2] <= 5 or row[2] >= 4090 for row in fresh)
        self.identity.configure(text=f"{state.get('device_id', '—')}  ·  {state.get('site', '—')}  ·  Data tahap: {stage}")
        self.power.configure(text="Kondisi daya: " + state.get("power_condition", "tidak dicatat"))
        labels = {"waiting": "MENUNGGU", "countdown": "BERSIAP", "capturing": "MEREKAM", "resting": "JEDA ISTIRAHAT",
                  "stage_complete": "TAHAP SELESAI", "complete": "SESI SELESAI", "failed": "SESI DITOLAK", "cancelled": "DIBATALKAN", "bench_complete": "UJI MEJA SELESAI"}
        label = labels.get(state.get("state"), state.get("state", ""))
        if state.get("state") in {"countdown", "capturing", "resting"}:
            label += f" · sisa {state.get('remaining_seconds', 0):.1f} detik"
        if self.paused:
            label += " · GRAFIK DIJEDA (rekaman tetap berjalan)"
        if (self.folder / "stop.request").exists() and state.get("state") not in TERMINAL:
            label = "PENGHENTIAN DIMINTA — bila wizard menunggu jawaban, tekan Enter di terminal"
        self.stop_button.configure(state="disabled" if state.get("state") in TERMINAL else "normal")
        self.status.configure(text=label + "\n" + state.get("instruction", ""))
        if self.rows:
            latest = self.rows[-1]
            self.metrics.configure(text=f"Saat ini: {latest[3]} mV  |  ADC: {latest[2]}  |  Clipping tahap: {self.clipped/self.total*100:.2f}%  |  Sampel diterima: {self.total:,}")
        else:
            self.metrics.configure(text="Belum ada sampel tahap ini. mV adalah keluaran modul, bukan µV input elektroda.")
        current_window = self.window.get()
        baseline = state.get("baseline_mv")
        if not self.paused and (fresh or self.dirty or current_window != self.last_window or baseline != self.last_baseline):
            span = float(self.window.get())
            end = max(span, self.rows[-1][1] / 1e6 if self.rows else span)
            start = end - span
            visible = [row for row in self.rows if row[1] / 1e6 >= start]
            times = [row[1] / 1e6 for row in visible]
            self.voltage_line.set_data(times, [row[3] for row in visible])
            self.adc_line.set_data(times, [row[2] for row in visible])
            self.adc.set_xlim(start, end)
            self.baseline_line.set_visible(isinstance(baseline, (float, int)))
            if isinstance(baseline, (float, int)):
                self.baseline_line.set_ydata([baseline, baseline])
            self.canvas.draw_idle()
            self.dirty = False
            self.last_window = current_window
            self.last_baseline = baseline
        self.root.after(100, self.poll)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("folder", type=Path)
    args = parser.parse_args()
    root = tk.Tk()
    LivePlot(root, args.folder.resolve())
    root.mainloop()


if __name__ == "__main__":
    main()
