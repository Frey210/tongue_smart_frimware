# Wizard karakterisasi EMG

Firmware khusus `emg-diagnostic` dan program Python memandu pengambilan data
bertahap. Pulsa gerak motor dan jaringan dinonaktifkan; driver motor masih dapat
bertegangan sesuai rangkaian dayanya. Profil kalibrasi, pairing, dan Wi-Fi
yang tersimpan tidak diubah. Saat image diagnostik dipakai, perangkat tidak
mengirim data ke web dan akhirnya tampak offline di dashboard.

## Build, upload dan jalankan

Pastikan seluruh sesi web sudah berhenti. Periksa ID perangkat sebelum upload.
Ganti COM20 sesuai port aktual. Jangan menjalankan serial monitor bersamaan.

```powershell
cd D:\Aerasea\tongue_smart\frimware
pio run -e emg-diagnostic -t upload --upload-port COM20
python -u tools\emg_wizard.py --port COM20 --site masseter_left
```

Gunakan `--site masseter_right`, `temporalis_left`, atau `temporalis_right`
sesuai pemasangan nyata. Jangan memindahkan elektroda di tengah satu sesi.
Ulang wizard untuk sisi atau lokasi lain. Simetri bilateral serentak tidak dapat
diukur dengan satu kanal yang dipindah-pindah.

Uji meja saja:

```powershell
python -u tools\emg_wizard.py --port COM20 --bench-only
python tools\emg_wizard.py --self-test
```

Dependensi host: Python 3 dan pyserial (sudah tersedia di workstation proyek).
Firmware menggunakan serial 460800 baud dan target sampling 1000 Hz.
ADC raw dan mV adalah dua konversi berurutan. Timing nyata, kehilangan sampel
dan clipping dicatat, sehingga target 1000 Hz tidak dianggap jaminan performa.
Tidak ada klaim bandwidth / filtering sEMG medis dari konfigurasi ini.

## Urutan

1. **Open, 10 detik:** semua elektroda lepas dari tubuh dan tidak disentuh.
   Tujuan: melihat input terbuka, clipping, dan gangguan. Bukan baseline otot.
2. **Rest, 10 detik:** elektroda terpasang sesuai protokol dokter, kontak telah
   stabil, rahang rileks, tanpa bicara/mengunyah. Operator mengonfirmasi isolasi
   listrik sebelum tahap manusia. Jawaban tersebut adalah catatan operator,
   bukan verifikasi keselamatan oleh software.
3. **Clench, 3 x 3 detik:** kontraksi nyaman yang dipandu dokter; bukan instruksi
   menggigit maksimal. Ada jeda pemulihan 30 detik antar-ulangan. Hentikan bila
   nyeri atau tidak nyaman. Protokol dapat perlu disesuaikan oleh dokter.
4. **Chew, 10 detik:** gerakan mengunyah ringan sesuai instruksi dokter, tanpa
   kebutuhan makanan. Artefak gerakan tetap mungkin terjadi.
5. **Recovery, 10 detik:** kembali rileks.

Setiap tahap menunggu Enter sebelum countdown 3 detik. `q` membatalkan pada
prompt; Ctrl+C atau tombol BACK menghentikan saat rekaman. BACK mengakhiri capture; capture pendek ditolak dan
sesi berhenti. Kegagalan/koneksi putus menyimpan data parsial tetapi tidak
menerima profil relatif. Jangan menjadwalkan langkah fisik tanpa kesiapan operator.

Durasi open/rest/chew/recovery dapat diubah dengan `--seconds 3` sampai `30`.
Clench tetap singkat 3 detik. Tidak ada program yang menstimulasi otot.

## Hasil otomatis

Folder baru per sesi pada `output/emg_wizard/<device>_<UTC timestamp>` berisi:

- CSV terpisah tiap tahap: sequence, waktu relatif, ADC count, keluaran mV.
- SVG pratinjau grafik dengan jumlah titik dikurangi, skala 0–3300 mV konsisten;
  CSV tetap menyimpan seluruh sampel yang diterima.
- `hasil.json`: metadata, kualitas akuisisi, statistik dan kandidat profil relatif.
- `hasil.md`: ringkasan yang mudah dibaca.

Pemeriksaan teknis menolak tahap manusia bila clipping >1%, ada sampel hilang,
timing tidak teratur (jeda >10 ms atau laju di luar 900–1100 Hz), atau capture
berhenti sebelum 95% durasi. Ini batas pemeriksaan awal, bukan standar klinis.
Open tetap disimpan walau buruk; tahap rest dan seterusnya harus lolos sebelum
melanjutkan. Jangan menafsirkan open yang tenang sebagai bukti keselamatan.

Analisis menghitung mean DC dan RMS keluaran modul terhadap mean rest. Noise
rest dikurangi secara kuadrat dari amplitudo tahap aktivitas (nilai negatif
menjadi nol). Referensi adalah median amplitudo tiga kontraksi nyaman. Profil
ditolak bila referensi tidak terpisah dari noise (<=max(5 mV, 2 x noise rest))
atau variasi antar-ulangan CV >30%. Ini heuristik untuk menandai data yang perlu
ditinjau, bukan metode kalibrasi amplitudo input yang tervalidasi.

Persentase yang dihasilkan adalah **% referensi keluaran**, bukan %MVC. Nilai
di atas 100% tidak dipotong. Nilai mV yang disajikan bukan uV input elektroda.
Koefisien relatif `relative_scale_percent_per_mv = 100 / reference_output_mv`
disimpan bersama baseline dan noise rest, dengan satuan persen per mV keluaran.
Kandidat profil tidak otomatis ditulis ke firmware produksi. Hasil lengkap
perlu ditinjau sebelum menentukan kompensasi atau perubahan pengolahan.
Mode produksi 0.6.0 menggunakan sampling 100 Hz dan jaringan aktif. Profil dari
mode diagnostik 1000 Hz tanpa jaringan tidak boleh langsung diasumsikan setara;
pengolahan, noise dan baseline harus diperiksa ulang pada kondisi penggunaan.

## Referensi client dan batas interpretasi

`refrensi.txt` menyebut angka istirahat 1,5–3 uV, batas 5 uV, kontraksi
40–150+ uV dan rasio simetri. File itu tidak menyertakan sumber/protokol,
definisi amplitudo (RMS/peak/envelope), gain, atau bandwidth. Angka tersebut
tidak digunakan sebagai target kalibrasi atau ambang diagnosis.

Mengatur gain agar istirahat selalu 2 uV atau menggigit selalu 100 uV akan
menghilangkan perbedaan biologis dan tidak membuktikan nilai sebenarnya.
Kalibrasi uV absolut tetap memerlukan sinyal input diferensial terukur atau
pembanding EMG yang tervalidasi dengan kondisi pengukuran setara.

- Seeed: https://wiki.seeedstudio.com/Grove-EMG_Detector/ (modul bukan untuk penggunaan medis).
- CEDE amplitude normalization: https://pubmed.ncbi.nlm.nih.gov/32569878/
- Pengaruh metode referensi pada otot pengunyahan: https://pubmed.ncbi.nlm.nih.gov/27552696/

## Tinjauan interferensi setelah sesi lengkap

```powershell
python tools\emg_review.py ..\output\emg_wizard\<folder-sesi>
```

Menambahkan `review.md` dan `review.json`, tanpa mengubah CSV/hasil.json asli.
Fit least-squares DC dan satu sinusoid mencari komponen 49–51 Hz. Persentase
variasi yang dijelaskan fit bukan persentase noise yang diketahui pasti, bukan
amplitudo input EMG, dan bukan diagnosis. Sumber gangguan tetap perlu diperiksa.
Tes: `python -m unittest discover -s tools -p "test_emg_*.py"`.

## Kembali ke firmware aplikasi

Setelah tes selesai dan elektroda dilepas dari tubuh:

```powershell
pio run -e esp32-s3-devkitc-1 -t upload --upload-port COM20
```

Image produksi kembali ke serial 115200 baud. Wi-Fi/pairing dan profil lama
tetap tersimpan. Tidak ada kalibrasi palsu yang ditambahkan oleh wizard.
