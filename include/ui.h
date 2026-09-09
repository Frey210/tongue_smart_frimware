#pragma once
#include <TFT_eSPI.h>
#include "types.h"

class UserInterface {
 public:
  bool begin();
  void render(const SharedStatus& status);
 private:
  TFT_eSPI tft_;
  TFT_eSprite plot_{&tft_};
  bool plotReady_ = false;
  AppState lastState_ = AppState::Error;
  uint8_t lastMenuIndex_ = 255;
  uint8_t lastCountdown_ = 255;
  char lastPairingCode_[16]{};
  char lastMessage_[64]{};
  uint32_t lastValuesDraw_ = 0;
  static constexpr uint8_t TRACE_POINTS = 51;
  float trace_[TRACE_POINTS]{};
  uint8_t traceCount_ = 0;
  void drawFrame(const SharedStatus& status);
  void drawHome(const SharedStatus& status);
  void drawExamMenu(const SharedStatus& status);
  void drawSettings(const SharedStatus& status);
  void drawPairing(const SharedStatus& status);
  void drawMessage(const SharedStatus& status, const char* title);
  void drawMeasurement(const SharedStatus& status, bool initialize);
  void drawResult(const SharedStatus& status);
  void drawMenu(const char* const items[], uint8_t count, uint8_t selected, int16_t y);
  static const char* stateName(AppState state);
  static const char* examName(ExaminationType type);
};
