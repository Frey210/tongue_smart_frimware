# Kontrak Sinkronisasi HTTPS

Target produksi: `https://tongue-smart.farlabs.my.id/api/v1`.

Secret perangkat tidak boleh ditulis ke repository. Nilai `X-Device-Key` nantinya dimasukkan melalui proses provisioning/NVS dan tercatat di register kredensial lokal proyek.

## Siklus sesi

1. Dashboard membuat sesi berstatus `prepared`.
2. Operator/perangkat mengaktifkan sesi menjadi `active`.
3. Firmware tetap merekam ke penyimpanan lokal terlebih dahulu.
4. Task jaringan mengirim batch tanpa memblokir task pengukuran.
5. Receipt disimpan lokal; data baru boleh ditandai tersinkron setelah receipt diterima.
6. Retry memakai `message_id`, `sequence`, dan checksum yang sama agar idempoten.

## Kontrol tahap pengukuran

Perangkat/simulator mengambil sesi aktif melalui:

```http
GET /device/sessions/active?device_id=tongue-smart-v3
X-Device-Key: <secret dari NVS>
```

Respons membawa `control` terbaru: `measurement`, `phase`, `protocol_stage`, dan `fsr_point`. Akuisisi jaringan hanya menyinkronkan modul yang sedang dikontrol. Nilai `paused` atau belum adanya kontrol berarti task jaringan menunggu tanpa membuat sampel baru. Kontrol dashboard tidak boleh melewati interlock keselamatan lokal firmware.

## Ingest batch

```http
POST /sessions/{session_id}/batches
X-Device-Key: <secret dari NVS>
Content-Type: application/json
```

Maksimum 500 sampel. `checksum` adalah SHA-256 lowercase atas array `samples` dalam canonical JSON dengan key terurut dan tanpa whitespace. Timestamp memakai ISO 8601 UTC (`Z`).

Kanal Phase 1:

- `emg_1`, unit `uV`;
- `fsr_1`, unit `kPa`, titik tekan dicatat pada `protocol_stage` secara bergiliran;
- `lip_force_1`, unit `N`.

Respons `202` berisi `receipt_id`, `duplicate`, `sequence`, dan `received_at`. Respons jaringan gagal tidak boleh menghentikan pemeriksaan atau menghapus hasil lokal.

## Keamanan

- HTTPS wajib untuk hostname publik.
- Jangan log atau tampilkan device key pada serial monitor/LCD.
- Emergency stop, motor interlock, dan keputusan keselamatan tetap lokal.
- Upload firmware ditunda sampai kontrak dan uji host selesai.
