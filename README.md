# Tongue Smart v3 — Phase 1 Firmware

Firmware offline-first berbasis PlatformIO untuk ESP32-S3. Root proyek ini sengaja memakai nama folder `frimware` sesuai permintaan.

## Fitur baseline

- FreeRTOS dual-core dengan task sensor, motor, tombol, GUI, storage, USB, dan aplikasi.
- State machine boot → self-test → ready → examination → processing → save/result.
- Sampling sensor EMG (aktivitas dalam uV) dan FSR lidah (tekanan dalam kPa) 100 Hz, serta load cell HX711 (gaya bibir dalam N).
- UI TFT 320×240 dengan grafik live bersumbu, tombol aktif-low dengan debounce, dan stepper non-blocking.
- Pengukuran gaya bibir otomatis mengembalikan carriage ke posisi awal sebelum sesi selesai.
- Hasil JSON offline di LittleFS `/results`, selalu diawali status `pending`.
- USB CDC dengan command `help`, `status`, dan `results`.
- Device ID unik dari eFuse MAC, secret acak per perangkat, dan credential tersimpan di NVS.
- Pairing code dari menu LCD **Settings > Register Device** atau command serial `pair_device`.
- Polling sesi/kontrol dashboard melalui HTTPS dan batch live 250 ms yang idempoten dengan checksum SHA-256.
- Perangkat legacy tetap dapat dipakai backend selama migrasi, tetapi firmware v0.3.0 memakai registry baru.

## Flow registrasi perangkat

1. Buka **Settings > WiFi Setup Portal** dan hubungkan perangkat ke internet.
2. Buka **Settings > Register Device**.
3. Masukkan kode `TS-XXXXXX` dari LCD ke halaman **Perangkat** pada dashboard.
4. Setelah claim berhasil, LCD kembali ke Home dengan status `Paired`.
5. Buat sesi di dashboard dan pilih device ID yang tampil pada LCD.

Secret tidak dicetak ke LCD/serial dan backend hanya menyimpan hash. NVS menyimpan `dev_id`, `hw_uid`, `dev_key`, status pairing, dan API base URL.

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

1. `HX711_COUNTS_PER_NEWTON` dan offset HX711;
2. `FSR_ZERO_ADC` serta kurva ADC-ke-kPa FSR (bukan sekadar skala linear);
3. `EMG_ADC_BIAS` dan `EMG_FRONTEND_GAIN` sesuai rangkaian analog;
4. baseline, filtering, sample rate, dan satuan EMG;
5. batas gerak, homing, `STEPPER_DIR_INVERTED`, kecepatan, dan emergency stop stepper;
6. target akurasi, prosedur kalibrasi, serta acceptance test perangkat.

Build saat ini menggunakan TLS terenkripsi tetapi verifikasi CA masih dinonaktifkan untuk prototipe Phase 1. Root CA harus dipin atau memakai certificate bundle sebelum validasi klinis. Baseline ini bukan perangkat medis tervalidasi dan tidak boleh menghasilkan keputusan klinis sebelum proses verifikasi tersebut selesai.

Skala FSR bawaan masih berupa placeholder linear: ADC 0–4095 dipetakan ke 0–100 kPa. Pembacaan ADC ≥4090 ditampilkan sebagai saturasi (`>100.0`) dan dikirim dengan kualitas `invalid`; nilai di atas rentang ADC tidak dapat ditaksir sampai rangkaian dan kurva kalibrasi FSR divalidasi.
