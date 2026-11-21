# EMG baseline workflow - firmware 0.7.2

## Processing assumptions

Implements the useful operational assumptions in the user's Uno experiment:
32-reading burst mean, a resting baseline captured after electrode placement,
fixed baseline during measurement, and a smoothed positive output indicator.
Voltage is read via the ESP32 ADC voltage API, not raw * 3.3 / 4095.
Then RMS over 25 averaged frames and time-based EMA (150 ms) are applied.
Actual sampling/timing and signal quality still need bench verification.

This changes amplitude semantics. It is neither clinical raw sEMG nor RMS of
the original unaveraged waveform. Gain remains an unverified assumption of
1000. No mapping to a target 150-250 uV or forced zero resting activity is used.
API unit remains uV for compatibility; LCD/web explicitly say estimate.
Baseline noise is recorded, not silently subtracted to hide interference.

## Per-reading spike rejection - 0.7.2

Fixes the previous any-spike-in-32 invalidates-whole-frame rule. Each burst now
averages only valid raw ADC readings and valid calibrated mV readings separately;
analogRead and analogReadMilliVolts are independent conversions, not a paired
raw/mV observation. A valid frame requires at least 24/32 readings in EACH stream.
The existing 10-second baseline averages the accepted frame means. The 0.7.1
capture-level safeguards (minimum frames/duration, mean bounds, at most 5% rejected
frames) remain. This is not an unlimited mean of saturated or missing input.

Individual raw readings <=5 or >=4090 and voltage readings outside 100-3050 mV
are excluded before averaging. If there are no valid readings, the observed
finite average is displayed for troubleshooting but remains invalid. Stable rail
input is not converted to a plausible baseline. DC-reference calibration still
requires a completely clean burst; spike tolerance does not relax DC calibration.

Serial emg_status adds valid_raw_readings and valid_mv_readings (out of 32).
emg_baseline_status reports candidate valid/rejected frames, mean and duration.
Failed reports now include rejection counts in reason, and LCD shows frame
counts/current ADC/mV instead of only 'no result'. Failed results offer retry,
not confirmation. This small error-recovery refinement preserves the LCD theme.
The web handshake/metrics schema remains compatible (mean-valid-frames-v2);
no backend/frontend deployment is required. Failed metrics remain absent from
the backend metrics object; use reason counts or serial for failed captures.

EMG raw_value uploaded by 0.7.2 is now the valid-readings ADC mean, matching the
voltage indicator; older firmware uploaded the last individual raw conversion.
Do not mix these raw representations without accounting for firmware version.
µV remains an unverified gain estimate, not clinical calibrated raw sEMG.
Four device self-tests include independent conversion spikes, exact 24/32
acceptance, insufficient valid readings, rail/nonfinite/short bursts, and a
1000-frame baseline with isolated spikes in every burst (mean stays 1500 mV).

## Relaxed resting baseline - 0.7.1

The reference is the arithmetic mean of valid averaged voltage frames captured
for 10 seconds: baseline = sum(valid mV) / valid frame count. Measurement uses
the existing voltage correction, subtracts this fixed baseline, then computes
the RMS/EMA indicator. This is offset referencing, not gain calibration or
mapping to an arbitrary normal clinical amplitude. No new sensor coefficient
is copied between devices.

ADC-clipped/non-finite/out-of-range frames are excluded from the mean and counted
separately. Up to 5% discarded frames is allowed. At least 800 valid frames,
both halves of the capture, 9.5-15 seconds, at most 3000 total frames, and a mean
inside 100-3000 mV remain required. Persistent clipping or an absent capture is
still rejected; arbitrary input cannot be made valid by normalization.

Noise above 25 mV, span above 100 mV and drift above 30 mV now generate review
warnings instead of rejection. Occasional discarded frames also generate a
warning. Operator confirmation and device acknowledgement remain mandatory.
If the subject moved or contracted during capture, repeat even if confirmation
is allowed. A variable baseline can reduce repeatability; warning is not proof
of acceptable electrode contact. API records estimator=mean-valid-frames-v2;
older reports default to mean-all-frames-v1. Matching backend validation must
be deployed before web use with 0.7.1. No frontend change is required because
the existing panel already displays reason and candidate statistics.

## Offline LCD

Home > Examination > Facial EMG > electrode preparation > rest instructions >
OK starts countdown 3 s + rest capture 10 s > review > OK saves > OK starts
measurement. The Calibration menu also offers standalone rest capture; after
saving there, return Home rather than starting another sensor accidentally.
BACK cancels or discards review. A failed capture retains old NVS settings but
does not authorize the new examination. Patient isolation must be verified
independently before any electrode-on-body test.

## Web authority and handshake

1. Prepare/start an EMG session with a fixed electrode site.
2. Monitoring > Persiapan EMG: operator confirms preparation and isolation.
3. Ambil baseline creates a control id with measurement=emg, phase=baseline.
4. Device polls active sessions, performs countdown/capture, reports review or
   failed with statistics. Baseline is NOT streamed as examination batches.
5. Operator reviews and confirms the exact control id in the web UI.
6. Backend returns accepted; device verifies the current session/control,
   writes the candidate to NVS, and acknowledges ready. Only then can the web
   start recording. Confirmation alone is not device acknowledgement.
7. Pause/resume retains this session's accepted baseline. Repositioning means
   take a new baseline; another session always needs its own capture.

GET /device/sessions/active adds emg_baseline with control_id/state/metrics,
processing=mean32-rms250-ema150-v1 and gain_verified=false.
Device-authenticated POST /device/sessions/{session_id}/emg-baseline/{control_id}
reports capturing/review/ready/failed. User-authenticated POST
/sessions/{session_id}/emg-baseline/confirm?control_id=... authorizes review.
Results responses include the current baseline. Backend creates the additive
emg_baselines table at startup; existing session/sample tables are not rewritten.

Web states: requested > capturing > review > accepted > ready, or failed /
cancelled. New controls supersede pending candidates. Stale ids, mismatched
device credentials, skipped confirmation and invalid metrics are rejected.
LCD cannot independently change DC/rest/reset during an active web session.
BACK during device-controlled EMG stops acquisition, reports failure and makes
the backend pause that EMG stage. A lost in-memory candidate/readiness after
reboot is not restored just because the backend still says ready.

## Deployment and verification

Deploy matching backend/frontend with firmware 0.7.0 as a coordinated release.
Old firmware cannot acknowledge this handshake; web EMG start stays blocked.
FSR and lip-force phase contracts remain unchanged.
Python device_simulator.py also handles the handshake using clearly synthetic
baseline statistics and requires operator confirmation; no sensor claim is made.

Run backend pytest, frontend build and the isolated Playwright fixture covering
preparation, capture, review, device ack, recording, cancel/retry and mobile layout.
Firmware emg_selftest adds rest_selftest (statistics, drift, clipping, duration).
Test commands never stimulate electrodes or command motor motion themselves.
Do not run a baseline capture on a person without independent electrical safety
verification. GUI/bench guards do not certify patient safety.

Reference: https://github.com/Seeed-Studio/Grove_EMG_detector_demo_code

## Verification record - 2026-10-30

- Production firmware code commit: 229ca9c, version 0.7.0. PlatformIO build and
  upload to COM20 completed with flash verification. Electrodes were removed;
  motor safety was confirmed by the operator before upload.
- Device serial self-tests: EMG, envelope and resting-baseline tests passed.
  Observed frame rate was 100 Hz; 12 status snapshots contained no reset marker.
  This is a short bench check, not a long-duration stability guarantee.
- Backend ea5994a: 12 tests passed, including confirmation/device-ack gates,
  wrong electrode site, stale controls, cancellation and ingestion guards.
- Frontend 4156485: production build passed. Playwright used isolated mocked
  API responses to test preparation, review, device acknowledgement, recording,
  cancellation/retry and mobile overflow; desktop/tablet/mobile screenshots
  are in output/screenshots/emg-baseline-20261030 in the workspace.
- Deployment through Tailscale completed. Public HTTPS /health returned ok,
  /ready returned ready, and the new frontend asset was verified. The live
  backend exposes the baseline endpoint and has the emg_baselines table.
- No new electrode-on-body capture or live device-to-web examination was
  performed in this verification. Operator acceptance testing remains required.
- Frontend dependency audit reported three high-severity findings (nanoid,
  source-map-js, vite). Dependency remediation is outstanding; no automatic
  dependency upgrade was bundled into this sensor workflow change.

### Second device update - 2026-10-30

Device TS-0468FEF61B44 (ESP32-S3 MAC 44:1b:f6:fe:68:04) received the same
production firmware 0.7.0 through COM26. Build/upload and flash hash verification
passed; no full-chip/NVS erase was performed. Operator confirmed electrodes off
body and motor safe before upload. Serial confirmed WiFi connected, paired,
Home state, all three EMG software self-tests passed and no reset markers during
the 12-second observation. Frame counters advanced at 100 Hz. No baseline or
human measurement was initiated. Calibration flags were zero (default settings),
so this device still requires its own guided resting baseline before EMG use;
calibration was not copied from the first device. HX711 reported not ready
during this check; lip-force hardware was not validated.

### Relaxed baseline release verification - 2026-10-31

Firmware e9f010c (0.7.1) built and uploaded to TS-D4085CF93030 on COM20;
flash hash verification passed. Serial confirmed all three software self-tests
passed, 100 Hz across six snapshots, and no reset markers during a 12-second
observation. Existing calibration flags remained 2; WiFi and pairing remained
active. The self-test covers exact 5% discarded-frame acceptance, exclusion of
spikes from the mean, excessive clipping rejection, variable baseline warnings,
non-finite input, missing capture half and insufficient duration. No baseline
capture, human measurement or motor sequence was started. HX711 was not ready.

Backend 9df0939 passed 12 tests and was pushed/deployed through Tailscale.
Public HTTPS health/ready and live relaxed metric schema checks passed; both
containers were healthy. The pre-existing active session on the other device
was in failed baseline state, not recording, and was not deleted or modified.
Frontend code/layout stayed unchanged: the existing review panel displays the
baseline warning and metrics. Browser tests were not repeated for this backend
and firmware-only change. Other devices have not received 0.7.1 in this update;
each needs its own firmware upload and a newly captured resting baseline.

### TS-02 baseline correction verification - 2026-11-17

COM26 identified TS-0468FEF61B44 still running 0.7.0, not the relaxed 0.7.1
release. Before upload, off-body voltage snapshots were approximately
1485.8-1489.6 mV without clipping; these observations do not establish the
cause of earlier on-body capture failures. The old any-spike-per-burst rule
was an additional sensitivity defect corrected in 0.7.2.

Production 0.7.2 built successfully (RAM 48,800 bytes; flash 1,147,753 bytes)
and uploaded through COM26 with flash hashes verified. No full-chip/NVS erase
was performed. Operator confirmed electrodes off body and motor safe.
All four serial software self-tests passed, including the 1000-frame isolated
spike baseline test. Across a 17-second serial observation, the device remained
at Home with WiFi and pairing active; no reset markers were detected. Five
snapshots showed 100 Hz, valid raw/mV readings 32/32, no clipping and output
1484.2-1488.9 mV. Existing calibration flags 2 and baseline 1761.1 mV remained.
These off-body voltages/old-baseline estimates are not physiological readings.

No baseline capture, human measurement, motor sequence or visual LCD inspection
was performed. A newly placed-electrode resting baseline must still be captured
and reviewed by the operator; persistent saturation is intentionally rejected.
HX711 reported not ready, so lip-force hardware was not validated. TS-01 was
not updated in this operation. No web deployment was needed for this compatible
firmware-only change. Build retained the existing TFT touch-pin warning; the
device uses physical buttons rather than a configured touch controller.

### TS-01 baseline correction verification - 2026-11-21

COM20 identified TS-D4085CF93030 running 0.7.1. Operator confirmed electrodes
off body and motor safe, and explicitly selected upload of 0.7.2 for the EMG
baseline only; force calibration/new lip-force guards are deferred. The exact
production code at 69981aa built successfully (RAM 48,800 bytes; flash
1,147,753 bytes) and uploaded on COM20 with flash hashes verified. No full-chip
or NVS erase was performed; WiFi, pairing and calibration were preserved.

All four serial software self-tests passed. In a 17-second observation the
device stayed at Home; WiFi and pairing were active and no reset markers were
detected. Five EMG snapshots showed 100 Hz, raw/mV valid readings 32/32,
no clipping, and off-body ADC output 1461.6-1468.9 mV. Existing baseline
1569.8 mV and calibration flags 2 remained. No baseline capture, human
measurement, motor command or visual LCD inspection was performed. Operator
must acquire/review a fresh resting baseline after electrode placement.

HX711 was ready at the status snapshot and reported 63190 with the unchanged
placeholder scale/untared boot offset. This is not a verified force in Newton.
The selected update does not correct Newton/kPa calibration, historic data or
server statistics. README records source references and the known-force method
needed for a subsequent device-specific calibration. No web deployment was
needed or performed for this compatible firmware upload.
