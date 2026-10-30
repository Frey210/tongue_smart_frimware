# EMG calibration and measurement reference

Firmware 0.6.0 adds **Home > Calibration** on the LCD. Calibration belongs to the
physical ESP32 and is stored in its `emg-cal` NVS namespace. It survives restart
and ordinary firmware upload. It does not replace Wi-Fi or device credentials.
Firmware 0.7.0 adds a guided rest-baseline review. Standalone LCD input works
offline; web-controlled acquisition requires the matching backend/frontend
baseline handshake (see EMG_BASELINE_FLOW.md).

## Supplied reference data, reviewed 24 October 2026

The source files are preserved unchanged in the project root.

| CSV label | Device ID recorded inside CSV | Samples | At ADC limits (<=5 or >=4090) | Recording date (UTC) |
| --- | --- | ---: | ---: | --- |
| TS-01 | TS-D4085CF93030 | 495 | 278 (56.16%) | 24 October 2026 |
| TS-02 | TS-0468FEF61B44 | 395 | 218 (55.19%) | 17 October 2026 |

`Template_Pengukuran_EMG_Multimeter(TS-01).xlsx` contains BOTH
`Ukur Multimeter Alat 1` and `Ukur Multimeter Alat 2`. The former repeats the
approximately 3.338 V supply / 1.10–1.22 V SIG measurements also found in the
TS-02 workbook. The latter records supply around 3.356–3.358 V and SIG ranging
from 0.1 to 3.2 V across conditions. Device identification fields are blank.
`Banding ADC` contains no matched ADC/DMM readings in either workbook.

These files establish a **rejected/unstable reference**, not an accepted numerical
calibration profile. No gain, DC correction or resting baseline was fitted or
installed from these files. File naming alone does not establish correspondence
between workbook tab, ESP32 hardware ID, and measurement interval.

COM20 was identified before upload as TS-D4085CF93030, in Home, paired.

## What changed in conversion

The sensor task reads raw ADC counts and `analogReadMilliVolts(GPIO4)`, which uses
the ESP32 ADC voltage calibration. They are successive conversions, NOT the
same instantaneous sample. Since 0.7.0, each sensor frame averages 32 voltage
conversions and separately averages raw counts. The exported raw value remains
the last unaveraged ADC count; `emgAverageRaw` is a separate diagnostic field.
Every conversion is checked for clipping before averaging. This mirrors the
Seeed demonstration's burst-mean concept, not its timing or clinical validity.

```
corrected output mV = ADC output mV + saved DC correction mV
centered mV = corrected averaged output mV - saved rest output mV
estimated input uV = smoothed RMS(centered mV) * 1000 / assumed gain
```

Since 0.6.1 the centered output uses a rolling RMS over 25 samples (nominal
250 ms at 100 Hz), followed by time-based EMA with a 150 ms time constant.
The formula is RMS = sqrt(mean(centered output mV squared)), then
EMA += (1 - exp(-elapsed_ms / 150)) * (RMS - EMA).
The window averages available samples during startup. Calibration changes,
non-finite input and acquisition gaps over 100 ms reset the envelope.
This reduces indicator jitter, adds response delay and changes amplitude
semantics from the former rectified EMA. It does not reject mains interference,
repair saturation or compensate unstable electrode contact. Raw ADC/mV and
validity checks are preserved. No baseline is learned automatically. In 0.7.0
RMS is applied to the burst-mean sequence: it is NOT RMS of the original
unaveraged electrode waveform. Do not compare amplitudes across processing versions.
This is not raw signed sEMG or a verified input-referred uV measurement.
The gain remains unverified at 1000. LCD measurement units say `uV est`.
For API compatibility, web packets still use `uV`; historical records are not
recalculated. Do not compare pre-0.6.0 and post-0.6.0 values as one calibrated series.

Without a saved profile, DC correction is 0 mV and resting output is 1650 mV,
marked unset. Defaults do NOT mean the sensor has been calibrated.

## LCD: DC reference and capture

1. Stop measurements in web and on device. Open **Home > Calibration**.
2. Choose **DC reference & capture**. All electrodes must be off the person.
   Connect DMM black to module GND, red to SIG. Read DC millivolts.
3. Enter the **current, simultaneous** DMM result, not an old workbook average.
   Example: 1.167 V means `1167 mV`.
4. UP/DOWN changes the selected digit (thousands, hundreds, tens, units).
   OK advances digit. On the last digit, OK starts **Capture & save**.
   Allowed reference is 100–3000 mV. BACK cancels without changing saved values.
5. Keep voltage stable for 4 seconds. The firmware computes the correction from
   DMM reference minus mean ADC millivolts. This is a one-point DC check, not gain calibration.
6. Save is rejected if fewer than 200 samples arrive, any ADC sample clips,
   measured mV leaves 100–3050 mV, span exceeds 50 mV, correction exceeds
   +/-500 mV, or flash write fails. These are conservative firmware capture
   guards, NOT clinical thresholds or a calibration accuracy specification.
7. A successful DC change clears the previous rest baseline. Capture rest again.

Open electrodes often produce unstable voltage and therefore may be rejected.
Do not defeat rejection or force the nominal 1.5 V standby value. Obtain a stable
electrical reference/test fixture and check wiring first if necessary.

## LCD: resting baseline

Choose **Capture rest baseline**. Read the preparation screen, then OK to capture
and begin a 3-second countdown followed by 10 seconds of capture. BACK cancels.
Review the mean, variation, drift and sample count; OK saves only after passing
the checks. BACK discards the candidate and retains the previous stored profile.
Repeat after changing electrode placement. A new local EMG examination asks for
a fresh baseline; a web session has its own baseline handshake.

This option is for a correctly installed, resting signal. Do NOT capture open
electrodes as a physiological baseline. Before using electrodes on a person,
the device's patient isolation and supply/USB connections must have been verified.
A DMM supply reading does not establish electrical safety.

Engineering rest guards (not clinical thresholds): 800-3000 sensor frames,
9.5-15 seconds acquisition, mean 100-3000 mV, no clipped/out-of-range frame,
standard deviation <=25 mV, span <=100 mV, difference between first-half and
last-half means <=30 mV. DC-check guards above stay separate and stricter.
Previous stored profile is retained on failure/cancel. Web control supersedes
local capture; local baseline/DC/reset cannot modify an active web session.

## LCD: reset

**Reset EMG settings** opens a confirmation page. OK restores local defaults;
BACK retains the profile. Reset affects neither pairing nor Wi-Fi.

## USB diagnostics (115200 baud)

- `status`: existing device status, unchanged contract.
- `emg_status`: raw count, ADC output mV, corrected mV, correction, baseline,
  saved DMM reference, flags, device ID, firmware and estimated uV.
- `emg_selftest`: non-mutating checks of conversion, rejected values, stable,
  clipped, noisy and insufficient-sample capture, plus envelope steady-state,
  decay, acquisition-gap reset and timestamp-wrap checks. Does not move the motor.

Flags: 0 = unset, 1 = DC saved, 2 = rest saved, 3 = both saved.
Flags describe what was stored, never certify gain, accuracy or patient safety.

## Sources

- User-supplied TS-01 / TS-02 workbooks and CSV exports in project root.
- Seeed Grove EMG: https://wiki.seeedstudio.com/Grove-EMG_Detector/
- ESP32 ADC API: https://docs.espressif.com/projects/arduino-esp32/en/latest/api/adc.html
- ESP32 Preferences: https://docs.espressif.com/projects/arduino-esp32/en/latest/tutorials/preferences.html

## Verification on COM20

- PlatformIO build/upload passed; flash image hash verified by esptool.
- Device reported firmware 0.6.0 and `emg_selftest: passed`.
- 25 serial diagnostic replies over 12.8 seconds: no observed reset messages,
  raw ADC 1763–1781, output 1464–1481 mV (mean 1472.9 mV).
- Device remained Home; Wi-Fi and pairing reported connected after upload.
- Calibration flags remained 0: no unverified reference was installed.
- Physical LCD appearance, button walkthrough, accepted DMM capture and profile
  persistence after a successful real capture still require hands-on verification.
  A test capture was not fabricated from the old workbook averages.
