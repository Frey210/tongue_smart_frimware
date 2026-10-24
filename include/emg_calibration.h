#pragma once
#include <math.h>
#include <stdint.h>

struct EmgCalibration {
  uint32_t version = 1;
  float dcOffsetMv = 0;
  float baselineMv = 1650;
  float referenceMv = 1500;
  uint32_t flags = 0; // bit 0: DC checked; bit 1: resting baseline captured
  bool valid() const {
    return version == 1 && flags <= 3 && isfinite(dcOffsetMv) && fabsf(dcOffsetMv) <= 500 &&
        isfinite(baselineMv) && baselineMv >= 100 && baselineMv <= 3100 &&
        isfinite(referenceMv) && referenceMv >= 100 && referenceMv <= 3000;
  }
  float correctedMv(float millivolts) const { return millivolts + dcOffsetMv; }
};

struct EmgCapture {
  uint32_t count = 0, clipped = 0;
  float sum = 0, minimum = 10000, maximum = -10000;
  void add(uint16_t raw, float millivolts) {
    ++count;
    if (raw <= 5 || raw >= 4090 || !isfinite(millivolts) || millivolts < 100 || millivolts > 3050) ++clipped;
    sum += millivolts;
    if (millivolts < minimum) minimum = millivolts;
    if (millivolts > maximum) maximum = millivolts;
  }
  float mean() const { return count ? sum / count : 0; }
  bool stable() const { return count >= 200 && clipped == 0 && maximum - minimum <= 50; }
};
