# Tongue Smart v3 — Phase 1 Firmware

Firmware offline-first berbasis PlatformIO untuk ESP32-S3. Root proyek ini sengaja memakai nama folder `frimware` sesuai permintaan.

## Fitur baseline

- FreeRTOS dual-core dengan task sensor, motor, tombol, GUI, storage, USB, dan aplikasi.
- State machine boot → self-test → ready → examination → processing → save/result.
- Sampling EMG dan FSR 100 Hz, HX711, filter EMA sederhana, dan agregasi hasil.
- UI TFT 320×240, tombol aktif-low dengan debounce, dan stepper non-blocking.
- Hasil JSON offline di LittleFS `/results`, selalu diawali status `pending`.
- USB CDC dengan command `help`, `status`, dan `results`.

## Build dan upload

```powershell
cd D:\Aerasea\tongue_smart\frimware
pio run
pio run -t upload
pio device monitor
```

Jika `pio` belum tersedia: `py -m pip install platformio`, lalu gunakan `py -m platformio run`.

## Konfigurasi hardware wajib diverifikasi

Default build memakai `esp32-s3-devkitc-1` dan ILI9341 landscape 320×240. Ubah environment/driver di `platformio.ini` bila board atau controller LCD berbeda. Semua GPIO dan konstanta sensor berada di `include/config.h`.

Sebelum dipakai secara klinis, tentukan dan validasi:

1. faktor kalibrasi dan offset HX711;
2. kurva konversi ADC-ke-tekanan FSR (bukan sekadar skala linear);
3. baseline, filtering, sample rate, dan satuan EMG;
4. batas gerak, homing, arah, kecepatan, dan emergency stop stepper;
5. target akurasi, prosedur kalibrasi, serta acceptance test perangkat.

Baseline ini bukan perangkat medis tervalidasi dan tidak boleh menghasilkan keputusan klinis sebelum proses verifikasi tersebut selesai.
