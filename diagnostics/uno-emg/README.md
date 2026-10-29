# Standalone Uno R3 EMG bench test

Disconnect the sensor completely from the ESP32 device before rewiring, with
power off. Keep all electrodes off the body for this bench test. USB-connected
Uno is not patient-isolated. Prior ESP32 isolation confirmation does not cover
the new Uno wiring. Never connect 12 V to the module or A0, and do not connect
electrodes directly to Uno pins. Leave AREF unconnected (DEFAULT AVcc reference).

| Grove EMG module pin | Uno R3 |
|---|---|
| VCC | 5V |
| GND | GND |
| SIG / OUT | A0 |
| NC, if present | Unconnected |

Use printed pin labels; wire colors alone do not establish identity. The three
electrode leads stay connected to the module's electrode jack, off the body.
Power Uno from USB for bench testing; no external 12 V adaptor is needed.

From the firmware repository:

```powershell
pio run -d diagnostics/uno-emg -e uno -t upload --upload-port COM33
python tools/uno_emg_capture.py --port COM33 --bench-ready
```

`info` identifies uno-emg-1. `start` captures ten seconds, then stops; `stop`
aborts. Serial 230400 baud. Target sampling 500 Hz, no filtering/baseline/gain
conversion. Raw ADC is 10-bit (0-1023). Host saves original samples, min/max,
mean, AC RMS about stage mean, actual rate, timing gaps, two plot views and
acquisition checks in a new output/uno_emg session. Timing failures are reported,
not silently accepted. Open inputs can pick up interference; this is not muscle
baseline or a pass/fail sensor-health test.

Voltage estimate = raw * Vref / 1023. Default Vref=5000 mV is an assumption,
not calibration. For a simultaneous DMM reading of Uno 5V-to-GND, use e.g.
`--vref-mv 4960`; measure only with electrodes off the body. Around 3.3 V output
is not the Uno ADC rail; the report separately counts estimated output >=3250 mV.
That count is a warning, not proof of module saturation. Do not compare ADC counts
directly with ESP32 12-bit readings. No input-uV or clinical claim is made.

Sources: https://wiki.seeedstudio.com/Grove-EMG_Detector/
https://docs.arduino.cc/hardware/uno-rev3/
