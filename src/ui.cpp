#include "ui.h"
#include "config.h"
#include <math.h>
#include <string.h>

namespace {
constexpr uint16_t BG = TFT_BLACK;
constexpr uint16_t PANEL = 0x1082;
constexpr uint16_t ACCENT = 0x2E5F;
constexpr uint16_t MUTED = 0xA514;
}

bool UserInterface::begin() {
  pinMode(hw::TFT_LED, OUTPUT);
  digitalWrite(hw::TFT_LED, HIGH);
  tft_.init();
  tft_.setRotation(3);
  tft_.fillScreen(BG);
  tft_.setTextDatum(MC_DATUM);
  tft_.setTextColor(TFT_WHITE, BG);
  tft_.drawString("TONGUE SMART", 160, 92, 4);
  tft_.setTextColor(TFT_CYAN, BG);
  tft_.drawString("Clinical Measurement System", 160, 130, 2);
  plot_.setColorDepth(8);
  plotReady_ = plot_.createSprite(307, 132) != nullptr;
  return true;
}

const char* UserInterface::stateName(AppState s) {
  switch (s) {
    case AppState::Boot: return "BOOT";
    case AppState::SelfTest: return "SELF TEST";
    case AppState::Home: return "READY";
    case AppState::ExaminationMenu: return "SELECT TEST";
    case AppState::PatientReady: return "PATIENT READY";
    case AppState::Calibration: return "CALIBRATION";
    case AppState::Countdown: return "GET READY";
    case AppState::Measurement: return "MEASURING";
    case AppState::Processing: return "PROCESSING";
    case AppState::Result: return "RESULT";
    case AppState::Saving: return "SAVING";
    case AppState::History: return "HISTORY";
    case AppState::Settings: return "SETTINGS";
    case AppState::DevicePairing: return "PAIR DEVICE";
    case AppState::About: return "ABOUT";
    case AppState::Error: return "SENSOR ERROR";
    case AppState::Recovery: return "RECOVERY";
  }
  return "";
}

const char* UserInterface::examName(ExaminationType t) {
  switch (t) {
    case ExaminationType::TonguePressure: return "TONGUE PRESSURE";
    case ExaminationType::LipForce: return "LIP FORCE";
    case ExaminationType::Emg: return "FACIAL EMG";
    case ExaminationType::Complete: return "COMPLETE EXAM";
  }
  return "";
}

void UserInterface::drawFrame(const SharedStatus& s) {
  tft_.fillScreen(BG);
  tft_.fillRect(0, 0, 320, 36, PANEL);
  tft_.setTextDatum(ML_DATUM);
  tft_.setTextColor(TFT_WHITE, PANEL);
  tft_.drawString("TONGUE SMART v3", 10, 18, 2);
  tft_.setTextDatum(MR_DATUM);
  tft_.setTextColor(TFT_CYAN, PANEL);
  tft_.drawString(stateName(s.state), 310, 18, 2);
}

void UserInterface::drawMenu(const char* const items[], uint8_t count, uint8_t selected, int16_t y) {
  for (uint8_t i = 0; i < count; ++i) {
    const int16_t top = y + i * 31;
    const bool active = i == selected;
    tft_.fillRoundRect(18, top, 284, 26, 5, active ? ACCENT : PANEL);
    tft_.setTextDatum(ML_DATUM);
    tft_.setTextColor(active ? TFT_WHITE : MUTED, active ? ACCENT : PANEL);
    tft_.drawString(items[i], 30, top + 13, 2);
    if (active) tft_.drawString(">", 282, top + 13, 2);
  }
}

void UserInterface::drawHome(const SharedStatus& s) {
  tft_.setTextDatum(ML_DATUM);
  tft_.setTextColor(TFT_GREEN, BG);
  tft_.drawString("DEVICE READY", 18, 54, 2);
  tft_.setTextDatum(MR_DATUM);
  tft_.setTextColor(MUTED, BG);
  tft_.drawString(s.wifiConnected ? (s.devicePaired ? "WiFi   Paired" : "WiFi   Unpaired") : "Storage: OK   Offline", 302, 54, 2);
  static const char* items[] = {"Start Examination", "History", "Calibration", "Settings", "About"};
  drawMenu(items, 5, s.menuIndex, 78);
}

void UserInterface::drawExamMenu(const SharedStatus& s) {
  tft_.setTextDatum(ML_DATUM);
  tft_.setTextColor(TFT_WHITE, BG);
  tft_.drawString("Choose examination", 18, 55, 2);
  static const char* items[] = {"Tongue Pressure", "Lip Force", "EMG Activity", "Complete Examination"};
  drawMenu(items, 4, s.menuIndex, 78);
  tft_.setTextColor(MUTED, BG);
  tft_.drawString("BACK  Return", 18, 218, 2);
}

void UserInterface::drawSettings(const SharedStatus& s) {
  tft_.setTextDatum(ML_DATUM);
  tft_.setTextColor(TFT_WHITE, BG);
  tft_.drawString("Connectivity", 18, 55, 2);
  static const char* items[] = {"WiFi Setup Portal", "Register Device"};
  drawMenu(items, 2, s.menuIndex, 82);
  tft_.setTextColor(MUTED, BG);
  tft_.drawString(s.devicePaired ? s.deviceId : "Device not registered", 18, 170, 2);
  tft_.drawString("BACK  Return", 18, 218, 2);
}

void UserInterface::drawPairing(const SharedStatus& s) {
  tft_.fillRect(0, 36, 320, 204, BG);
  tft_.setTextDatum(MC_DATUM);
  tft_.setTextColor(TFT_CYAN, BG);
  tft_.drawString("REGISTER DEVICE", 160, 61, 4);
  tft_.setTextColor(TFT_WHITE, BG);
  tft_.drawString(s.pairingCode[0] ? s.pairingCode : "REQUESTING...", 160, 112, 4);
  tft_.setTextColor(MUTED, BG);
  tft_.drawString(s.message, 160, 151, 2);
  tft_.drawString(s.deviceId, 160, 180, 2);
  tft_.drawString("BACK  Return", 160, 218, 2);
}

void UserInterface::drawMessage(const SharedStatus& s, const char* title) {
  tft_.setTextDatum(MC_DATUM);
  tft_.setTextColor(TFT_CYAN, BG);
  tft_.drawString(title, 160, 68, 4);
  tft_.setTextColor(TFT_WHITE, BG);
  tft_.drawString(s.message, 160, 118, 2);
  if (s.state == AppState::Countdown) {
    char n[3]; snprintf(n, sizeof(n), "%u", s.countdown);
    tft_.setTextColor(TFT_GREEN, BG);
    tft_.drawString(n, 160, 164, 7);
  } else {
    tft_.setTextColor(MUTED, BG);
    tft_.drawString(s.state == AppState::Error ? "BACK  Return to Home" : "OK  Continue     BACK  Cancel", 160, 194, 2);
  }
}

void UserInterface::drawMeasurement(const SharedStatus& s, bool initialize) {
  if (initialize) {
    tft_.fillRect(0, 36, 320, 204, BG);
    tft_.setTextDatum(ML_DATUM);
    tft_.setTextColor(TFT_CYAN, BG);
    tft_.drawString(examName(s.examination), 12, 51, 2);
    tft_.setTextColor(TFT_GREEN, BG);
    tft_.drawString("LIVE", 5, 218, 1);
    tft_.setTextDatum(MC_DATUM);
    tft_.setTextColor(MUTED, BG);
    if (s.remoteControlled) tft_.drawString("CONTROLLED FROM WEB", 174, 218, 1);
    else if (s.examination != ExaminationType::LipForce)
      tft_.drawString("OK / BACK  STOP & SAVE", 174, 218, 1);
  }
  char value[24] = "---";
  const char* unit = "";
  float current = NAN;
  bool saturated = false;
  if (s.examination == ExaminationType::TonguePressure) {
    current = s.sample.pressureKpa;
    saturated = s.sample.fsrSaturated;
    if (isfinite(current)) snprintf(value, sizeof(value), saturated ? ">%.1f" : "%.1f", current);
    unit = "kPa";
  } else if (s.examination == ExaminationType::LipForce) {
    current = s.sample.hx711Ready ? s.sample.lipForce : NAN;
    if (isfinite(current)) snprintf(value, sizeof(value), "%.1f", current);
    unit = "N";
  } else {
    current = s.sample.emgMicrovolts;
    if (isfinite(current)) snprintf(value, sizeof(value), "%.0f", current);
    unit = "uV";
  }

  if (isfinite(current)) {
    if (traceCount_ < TRACE_POINTS) trace_[traceCount_++] = current;
    else {
      memmove(trace_, trace_ + 1, sizeof(float) * (TRACE_POINTS - 1));
      trace_[TRACE_POINTS - 1] = current;
    }
  }

  tft_.fillRect(190, 37, 130, 34, BG);
  tft_.setTextDatum(MR_DATUM);
  tft_.setTextColor(saturated ? TFT_ORANGE : TFT_WHITE, BG);
  tft_.drawString(value, 272, 51, 4);
  tft_.setTextColor(MUTED, BG);
  tft_.drawString(unit, 307, 51, 2);

  constexpr int16_t x0 = 40, y0 = 0, width = 267, height = 112;
  if (!plotReady_) return;
  plot_.fillSprite(BG);
  plot_.drawRect(x0, y0, width, height, TFT_DARKGREY);
  for (uint8_t i = 1; i < 4; ++i) {
    const int16_t y = y0 + (height * i) / 4;
    plot_.drawFastHLine(x0 + 1, y, width - 2, PANEL);
  }
  for (uint8_t i = 1; i < 5; ++i) {
    const int16_t x = x0 + (width * i) / 5;
    plot_.drawFastVLine(x, y0 + 1, height - 2, PANEL);
  }

  float scaleMax = 1.0F;
  for (uint8_t i = 0; i < traceCount_; ++i) scaleMax = max(scaleMax, trace_[i]);
  scaleMax *= 1.1F;
  plot_.setTextDatum(MR_DATUM);
  plot_.setTextColor(MUTED, BG);
  plot_.drawFloat(scaleMax, scaleMax < 10 ? 1 : 0, x0 - 4, y0 + 3, 1);
  plot_.drawString("0", x0 - 4, y0 + height - 3, 1);
  plot_.setTextDatum(ML_DATUM);
  plot_.drawString("-5s", x0, y0 + height + 8, 1);
  plot_.setTextDatum(MR_DATUM);
  plot_.drawString("now", x0 + width, y0 + height + 8, 1);
  for (uint8_t i = 1; i < traceCount_; ++i) {
    const int16_t x1 = x0 + 2 + ((i - 1) * (width - 4)) / (TRACE_POINTS - 1);
    const int16_t x2 = x0 + 2 + (i * (width - 4)) / (TRACE_POINTS - 1);
    const int16_t y1 = y0 + height - 2 - static_cast<int16_t>((trace_[i - 1] / scaleMax) * (height - 4));
    const int16_t y2 = y0 + height - 2 - static_cast<int16_t>((trace_[i] / scaleMax) * (height - 4));
    plot_.drawLine(x1, y1, x2, y2, TFT_CYAN);
  }
  plot_.pushSprite(0, 75);
  if (!s.remoteControlled && s.examination == ExaminationType::LipForce) {
    tft_.drawRoundRect(40, 213, 267, 10, 5, TFT_DARKGREY);
    tft_.fillRoundRect(42, 215, 263, 6, 3, BG);
    tft_.fillRoundRect(42, 215, (263 * s.progress) / 100, 6, 3, TFT_GREEN);
  }
}

void UserInterface::drawResult(const SharedStatus& s) {
  tft_.setTextDatum(MC_DATUM);
  tft_.setTextColor(TFT_GREEN, BG);
  tft_.drawString("MEASUREMENT COMPLETE", 160, 58, 2);
  tft_.setTextColor(TFT_CYAN, BG);
  tft_.drawString(examName(s.examination), 160, 83, 2);
  char line[48];
  if (s.examination == ExaminationType::TonguePressure)
    snprintf(line, sizeof(line), "Peak %.1f kPa", s.result.peakPressureKpa);
  else if (s.examination == ExaminationType::LipForce)
    snprintf(line, sizeof(line), "Peak %.1f N", s.result.peakLipForce);
  else
    snprintf(line, sizeof(line), "Mean %.0f", s.result.meanEmg);
  tft_.setTextColor(TFT_WHITE, BG);
  tft_.drawString(line, 160, 125, 4);
  tft_.setTextColor(MUTED, BG);
  tft_.drawString(s.wifiConnected && s.devicePaired ? "Saved Local  |  HTTPS Connected" : "Saved Local  |  Offline", 160, 165, 2);
  tft_.drawString("OK  Return to Home", 160, 210, 2);
}

void UserInterface::render(const SharedStatus& s) {
  const bool pageChanged = s.state != lastState_;
  const bool menuChanged = !pageChanged && s.menuIndex != lastMenuIndex_ &&
      (s.state == AppState::Home || s.state == AppState::ExaminationMenu || s.state == AppState::Settings);
  if (pageChanged) {
    if (s.state == AppState::Measurement) {
      traceCount_ = 0;
    }
    drawFrame(s);
    lastState_ = s.state;
    lastMenuIndex_ = s.menuIndex;
    if (s.state == AppState::Home) drawHome(s);
    else if (s.state == AppState::ExaminationMenu) drawExamMenu(s);
    else if (s.state == AppState::PatientReady) drawMessage(s, examName(s.examination));
    else if (s.state == AppState::Calibration) drawMessage(s, "ZEROING SENSOR");
    else if (s.state == AppState::Countdown) drawMessage(s, "STARTING IN");
    else if (s.state == AppState::Processing) drawMessage(s, "PROCESSING");
    else if (s.state == AppState::Saving) drawMessage(s, "SAVING");
    else if (s.state == AppState::Result) drawResult(s);
    else if (s.state == AppState::Error) drawMessage(s, "SENSOR ERROR");
    else if (s.state == AppState::History) drawMessage(s, "HISTORY");
    else if (s.state == AppState::Settings) drawSettings(s);
    else if (s.state == AppState::DevicePairing) {
      drawPairing(s);
      strlcpy(lastPairingCode_, s.pairingCode, sizeof(lastPairingCode_));
      strlcpy(lastMessage_, s.message, sizeof(lastMessage_));
    }
    else if (s.state == AppState::About) drawMessage(s, "TONGUE SMART v3");
  } else if (menuChanged) {
    lastMenuIndex_ = s.menuIndex;
    if (s.state == AppState::Home) drawHome(s);
    else if (s.state == AppState::ExaminationMenu) drawExamMenu(s);
    else drawSettings(s);
  }
  if (s.state == AppState::Measurement && (pageChanged || millis() - lastValuesDraw_ >= 100)) {
    lastValuesDraw_ = millis();
    drawMeasurement(s, pageChanged);
  }
  if (s.state == AppState::Countdown && s.countdown != lastCountdown_) {
    lastCountdown_ = s.countdown;
    tft_.fillRect(100, 137, 120, 57, BG);
    tft_.setTextDatum(MC_DATUM);
    tft_.setTextColor(TFT_GREEN, BG);
    char n[3]; snprintf(n, sizeof(n), "%u", s.countdown);
    tft_.drawString(n, 160, 164, 7);
  }
  if (!pageChanged && s.state == AppState::DevicePairing &&
      (strcmp(lastPairingCode_, s.pairingCode) != 0 || strcmp(lastMessage_, s.message) != 0)) {
    strlcpy(lastPairingCode_, s.pairingCode, sizeof(lastPairingCode_));
    strlcpy(lastMessage_, s.message, sizeof(lastMessage_));
    drawPairing(s);
  }
}
