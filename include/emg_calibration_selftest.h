#pragma once
#include "emg_calibration.h"

// Pure checks: never read a sensor, move the motor or change saved settings.
inline bool emgCalibrationSelftest() {
  EmgCalibration calibration;
  if (!calibration.valid()) return false;
  calibration.dcOffsetMv = 17;
  if (fabsf(calibration.correctedMv(1150) - 1167) > 0.01F) return false;
  calibration.dcOffsetMv = 501;
  if (calibration.valid()) return false;
  calibration.dcOffsetMv = NAN;
  if (calibration.valid()) return false;
  EmgCapture empty;
  if (empty.stable()) return false;
  EmgCapture stable, clipped, noisy, shortCapture;
  for (unsigned i = 0; i < 300; ++i) {
    stable.add(1500, 1200);
    clipped.add(i == 150 ? 4095 : 1500, 1200);
    noisy.add(1500, i % 2 ? 1100 : 1300);
    if (i < 199) shortCapture.add(1500, 1200);
  }
  return stable.stable() && fabsf(stable.mean() - 1200) < 0.01F &&
      !clipped.stable() && !noisy.stable() && !shortCapture.stable();
}
