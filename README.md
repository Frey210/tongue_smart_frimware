# Tongue Smart v3 — Phase 1 Firmware

Firmware offline-first berbasis PlatformIO untuk ESP32-S3. Root proyek ini sengaja memakai nama folder `frimware` sesuai permintaan.

## Fitur baseline

- FreeRTOS dual-core dengan task sensor, motor, tombol, GUI, storage, USB, dan aplikasi.
- State machine boot → self-test → ready → examination → processing → save/result.
- Sampling EMG dan FSR 100 Hz, HX711, filter EMA sederhana, dan agregasi hasil.
- UI TFT 320×240, tombol aktif-low dengan debounce, dan stepper non-blocking.
- Hasil JSON offline di LittleFS `/results`, selalu diawali status `pending`.
- USB CDC dengan command `help`, `status`, dan `results`.
- Device ID unik dari eFuse MAC, secret acak per perangkat, dan credential tersimpan di NVS.
- Pairing code dari menu LCD **Settings > Register Device** atau command serial `pair_device`.
- Polling sesi/kontrol dashboard melalui HTTPS dan batch sensor idempoten dengan checksum SHA-256.
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

1. faktor kalibrasi dan offset HX711;
2. kurva konversi ADC-ke-tekanan FSR (bukan sekadar skala linear);
3. baseline, filtering, sample rate, dan satuan EMG;
4. batas gerak, homing, arah, kecepatan, dan emergency stop stepper;
5. target akurasi, prosedur kalibrasi, serta acceptance test perangkat.

Build saat ini menggunakan TLS terenkripsi tetapi verifikasi CA masih dinonaktifkan untuk prototipe Phase 1. Root CA harus dipin atau memakai certificate bundle sebelum validasi klinis. Baseline ini bukan perangkat medis tervalidasi dan tidak boleh menghasilkan keputusan klinis sebelum proses verifikasi tersebut selesai.
