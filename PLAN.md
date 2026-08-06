# Firmware Plan

> Nama folder `frimware` dipertahankan agar sesuai path proyek dan remote repository yang telah ditentukan.

## Tujuan

Firmware ESP32-S3 yang deterministic, offline-first, dan dapat menyelesaikan workflow pemeriksaan tanpa backend.

## Kondisi awal

- PlatformIO + Arduino framework sudah dapat dibangun.
- FreeRTOS task untuk sensor, motor, tombol, GUI, storage, aplikasi, dan serial tersedia.
- Menu klinis, countdown, validasi HX711 dasar, LittleFS, dan USB serial dasar tersedia.
- TFT memakai FSPI 10 MHz dan orientasi landscape rotation 3.

## Milestone

### FW0 — Hardware confirmation

- Pastikan board/flash/PSRAM dan controller TFT aktual.
- Bench test setiap GPIO.
- Tambahkan pin enable/limit/emergency stop motor bila tersedia.
- Rekam `hardware_revision` dan capability aktual.

### FW1 — Driver validation

- HX711: timeout, tare stability, calibration factor, range/error state.
- FSR: kurva kalibrasi per sensor dan valid range.
- EMG: baseline, sample rate, anti-aliasing/filtering, saturation detection.
- Stepper: steps/mm, 45 mm/min, homing, travel limit, hard stop.
- TFT/button soak test tanpa flicker atau missed input.

### FW2 — Deterministic workflow

- Pisahkan application state machine dari task bootstrap.
- Workflow per pemeriksaan dan complete examination.
- Stage EMG membawa kode posisi elektroda dari sesi dashboard atau pilihan LCD untuk operasi offline; posisi aktif ditampilkan sebelum countdown dan disimpan bersama hasil.
- Perubahan posisi elektroda setelah pengukuran dimulai wajib menghasilkan stage baru, bukan menimpa metadata hasil sebelumnya.
- Safety task/watchdog dan motor interlock.
- Hasil menyimpan peak/mean/duration/quality serta calibration reference.

### FW3 — Storage and protocol

- Atomic result write dan boot recovery.
- Versioned NDJSON USB protocol.
- `hello/capabilities`, command ACK/NACK, sequence, checksum, dan replay.
- Result list/read/mark-acknowledged tanpa menghapus sebelum ACK.

### FW4 — Connectivity

- Wi-Fi provisioning lokal.
- Portal WiFiManager dipicu dari menu Settings pada LCD.
- Sinkronisasi HTTP/HTTPS dengan retry dan exponential backoff.
- MQTT/EMQX ditunda sampai kontrak REST dan operasional lapangan stabil.
- Measurement task tidak pernah menunggu jaringan.

### FW5 — Verification

- Unit test logic murni di host.
- Hardware-in-loop untuk sensor timeout, disconnect, cancel, reboot, storage full.
- Endurance test dan pengukuran latency/jitter.
- Traceability acceptance criteria PRD ke test result.

## Kontrak capability minimum

```json
{
  "type": "hello",
  "schema_version": 1,
  "device_id": "TS-<eFuse MAC>",
  "hardware_revision": "unknown",
  "capabilities": {
    "emg_channels": 1,
    "tongue_pressure_channels": 1,
    "lip_force": true,
    "motorized_traction": true
  }
}
```

## Safety gates sebelum uji manusia

- steps/mm dan limit travel tervalidasi;
- emergency stop fisik tersedia dan diuji;
- sensor memiliki calibration record yang berlaku;
- nilai invalid tidak dapat menjadi hasil PASS;
- cancel/error/reboot membuat output stepper inactive;
- protokol dan informed consent disetujui pihak berwenang.

## Definition of done Phase 1

- `pio run` lolos tanpa error.
- Satu sesi tiap jenis pemeriksaan selesai offline dan survive reboot.
- Storage full/sensor disconnect/cancel menghasilkan state aman.
- USB replay hasil tidak menggandakan data di penerima.
- Timing sensor dan motor tercatat serta memenuhi batas yang disetujui.
