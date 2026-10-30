#pragma once
#include <math.h>
#include <stdint.h>

struct EmgRestCapture {
  uint32_t samples = 0, clippedFrames = 0, firstCount = 0, lastCount = 0;
  float meanMv = 0, m2 = 0, minimum = 10000, maximum = -10000;
  float firstSum = 0, lastSum = 0;
  uint32_t durationMs = 0;
  void add(float mv, bool clipped, uint32_t elapsedMs) {
    if (!isfinite(mv)) { ++clippedFrames; return; }
    ++samples;
    if (clipped || mv < 100 || mv > 3050) ++clippedFrames;
    const float delta = mv - meanMv;
    meanMv += delta / samples;
    m2 += delta * (mv - meanMv);
    minimum = fminf(minimum, mv); maximum = fmaxf(maximum, mv);
    if (elapsedMs < 5000) { firstSum += mv; ++firstCount; }
    else { lastSum += mv; ++lastCount; }
  }
  float noiseMv() const { return samples ? sqrtf(fmaxf(0, m2) / samples) : 0; }
  float spanMv() const { return samples ? maximum - minimum : 0; }
  float driftMv() const { return firstCount && lastCount ? fabsf(firstSum / firstCount - lastSum / lastCount) : 10000; }
  const char* rejection() const {
    if (clippedFrames) return "ADC clipping / range: check signal";
    if (samples < 800 || samples > 3000 || durationMs < 9500 || durationMs > 15000) return "Missing samples: repeat baseline";
    if (meanMv < 100 || meanMv > 3000) return "Baseline voltage out of range";
    if (noiseMv() > 25 || spanMv() > 100 || driftMv() > 30) return "Signal unstable: relax, check contact";
    return nullptr;
  }
};

inline bool emgRestSelftest() {
  EmgRestCapture good, clipped, drift;
  for (unsigned i = 0; i < 1000; ++i) {
    good.add(1500 + (i % 2 ? 5 : -5), false, i * 10);
    clipped.add(1500, i == 9, i * 10);
    drift.add(i < 500 ? 1400 : 1600, false, i * 10);
  }
  good.durationMs = clipped.durationMs = drift.durationMs = 10000;
  if (good.rejection() || !clipped.rejection() || !drift.rejection()) return false;
  if (fabsf(good.meanMv - 1500) > .01F || fabsf(good.noiseMv() - 5) > .01F) return false;
  good.durationMs = 2000;
  return good.rejection() != nullptr;
}
