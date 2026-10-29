#pragma once
#include <math.h>
#include <stdint.h>

// Output envelope only: no notch, automatic baseline tracking or gain fitting.
class EmgEnvelope {
 public:
  static constexpr unsigned WINDOW = 25; // 250 ms at the nominal 100 Hz sensor rate
  void reset() { *this = EmgEnvelope(); }
  float update(float centeredMv, uint32_t timestampMs) {
    if (!isfinite(centeredMv)) { reset(); return 0; }
    const uint32_t elapsed = timestampMs - previousMs;
    if (count && elapsed > 100) reset(); // Never blend across a stalled acquisition.
    const bool first = count == 0;
    previousMs = timestampMs;
    const float square = centeredMv * centeredMv;
    sum += square - squares[index];
    squares[index] = square;
    index = (index + 1) % WINDOW;
    if (count < WINDOW) ++count;
    const float rms = sqrtf(fmaxf(0, sum) / count);
    const float alpha = first ? 1.0F : 1.0F - expf(-static_cast<float>(elapsed) / 150.0F);
    envelope += alpha * (rms - envelope);
    return envelope;
  }
 private:
  float squares[WINDOW]{};
  float sum = 0, envelope = 0;
  unsigned index = 0, count = 0;
  uint32_t previousMs = 0;
};

inline bool emgEnvelopeSelftest() {
  EmgEnvelope filter;
  for (unsigned i = 0; i < 300; ++i)
    if (fabsf(filter.update(i % 2 ? 20 : -20, i * 10) - 20) > 0.01F) return false;
  // Sustained activity must remain visible, not be learned away as a new baseline.
  for (unsigned i = 300; i < 600; ++i) filter.update(100, i * 10);
  if (fabsf(filter.update(100, 6000) - 100) > 0.01F) return false;
  for (unsigned i = 601; i < 900; ++i) filter.update(0, i * 10);
  if (filter.update(0, 9000) > 0.01F) return false;
  if (filter.update(NAN, 9010) != 0 || filter.update(30, 9020) != 30) return false;
  if (filter.update(10, 10000) != 10) return false;
  filter.reset();
  filter.update(20, UINT32_MAX - 4);
  return fabsf(filter.update(-20, 5) - 20) < 0.01F;
}
