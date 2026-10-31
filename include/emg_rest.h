#pragma once
#include <math.h>
#include <stdint.h>

struct EmgRestCapture {
  uint32_t samples = 0, clippedFrames = 0, firstCount = 0, lastCount = 0;
  float meanMv = 0, m2 = 0, minimum = 10000, maximum = -10000;
  float firstSum = 0, lastSum = 0;
  uint32_t durationMs = 0;
  void add(float mv, bool clipped, uint32_t elapsedMs) {
    // Ignore contaminated frames instead of letting one spike bias the baseline.
    if (clipped || !isfinite(mv) || mv < 100 || mv > 3050) { ++clippedFrames; return; }
    ++samples;
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
    const uint32_t total = samples + clippedFrames;
    if (clippedFrames * 20 > total) return "Too much ADC clipping: check signal";
    if (samples < 800 || total > 3000 || !firstCount || !lastCount || durationMs < 9500 || durationMs > 15000) return "Missing samples: repeat baseline";
    if (meanMv < 100 || meanMv > 3000) return "Baseline voltage out of range";
    return nullptr;
  }
  const char* warning() const {
    if (noiseMv() > 25 || spanMv() > 100 || driftMv() > 30) return "High variation; confirm only if relaxed";
    if (clippedFrames) return "ADC spikes skipped; review mean";
    return nullptr;
  }
};

inline bool emgRestSelftest() {
  EmgRestCapture good, clipped, excessiveClipping, drift, invalid, missingHalf;
  for (unsigned i = 0; i < 1000; ++i) {
    good.add(1500 + (i % 2 ? 5 : -5), false, i * 10);
    clipped.add(i % 20 == 0 ? 3050 : 1500, i % 20 == 0, i * 10);
    excessiveClipping.add(1500, i % 10 == 0, i * 10);
    drift.add(i < 500 ? 1400 : 1600, false, i * 10);
    invalid.add(NAN, false, i * 10);
    missingHalf.add(1500, false, 1000);
  }
  good.durationMs = clipped.durationMs = excessiveClipping.durationMs = drift.durationMs = invalid.durationMs = missingHalf.durationMs = 10000;
  if (good.rejection() || clipped.rejection() || drift.rejection() || good.warning()) return false;
  if (!clipped.warning() || !drift.warning() || !excessiveClipping.rejection() || !invalid.rejection() || !missingHalf.rejection()) return false;
  if (clipped.samples != 950 || clipped.clippedFrames != 50 || fabsf(clipped.meanMv - 1500) > .01F) return false;
  if (fabsf(good.meanMv - 1500) > .01F || fabsf(good.noiseMv() - 5) > .01F) return false;
  good.durationMs = 2000;
  return good.rejection() != nullptr;
}
