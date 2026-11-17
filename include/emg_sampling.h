#pragma once
#include <math.h>
#include <stdint.h>
#include "emg_rest.h"

struct EmgBurstMean {
  uint16_t count = 0, validRaw = 0, validMv = 0, finiteMv = 0;
  uint32_t rawSum = 0, validRawSum = 0;
  float mvSum = 0, validMvSum = 0;
  void add(uint16_t raw, float mv) {
    ++count; rawSum += raw;
    if (raw > 5 && raw < 4090) { ++validRaw; validRawSum += raw; }
    // Raw and mV API calls are separate ADC conversions; filter each independently.
    if (isfinite(mv)) {
      ++finiteMv; mvSum += mv;
      if (mv >= 100 && mv <= 3050) { ++validMv; validMvSum += mv; }
    }
  }
  uint16_t rawMean() const { return validRaw ? validRawSum / validRaw : count ? rawSum / count : 0; }
  float mvMean() const { return validMv ? validMvSum / validMv : finiteMv ? mvSum / finiteMv : 0; }
  bool usable() const { return count >= 32 && validRaw * 4 >= count * 3 && validMv * 4 >= count * 3; }
};

inline bool emgSamplingSelftest() {
  EmgBurstMean spike, boundary, excessive, rails, mismatched, invalid, shortBurst;
  for (unsigned i = 0; i < 32; ++i) {
    spike.add(i == 0 ? 4095 : 1800, i == 1 ? 3300 : 1500);
    boundary.add(i < 8 ? 4095 : 1800, i < 8 ? 3300 : 1500);
    excessive.add(i < 9 ? 4095 : 1800, i < 9 ? 3300 : 1500);
    rails.add(4095, 3300);
    mismatched.add(0, 1500);
    invalid.add(1800, NAN);
    if (i < 20) shortBurst.add(1800, 1500);
  }
  EmgRestCapture baseline;
  for (unsigned frame = 0; frame < 1000; ++frame) {
    EmgBurstMean burst;
    for (unsigned i = 0; i < 32; ++i) burst.add(i == 0 ? 4095 : 1800, i == 1 ? 3300 : 1500);
    baseline.add(burst.mvMean(), !burst.usable(), frame * 10);
  }
  baseline.durationMs = 10000;
  return !baseline.rejection() && baseline.samples == 1000 && fabsf(baseline.meanMv - 1500) < .01F &&
      spike.usable() && spike.validRaw == 31 && spike.validMv == 31 &&
      spike.rawMean() == 1800 && fabsf(spike.mvMean() - 1500) < .01F &&
      boundary.usable() && !excessive.usable() && !rails.usable() &&
      !mismatched.usable() && !invalid.usable() && !shortBurst.usable();
}
