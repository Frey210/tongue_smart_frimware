#include "ui.h"
#include "config.h"
#include <math.h>

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
  static const char* items[] = {"Tongue Pressure", "Lip Force", "Facial EMG", "Complete Examination"};
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

void UserInterface::drawMeasurement(const SharedStatus& s) {
  tft_.setTextDatum(MC_DATUM);
  tft_.setTextColor(TFT_CYAN, BG);
  tft_.drawString(examName(s.examination), 160, 55, 2);
  char value[24] = "---";
  const char* unit = "";
  if (s.examination == ExaminationType::TonguePressure) {
    if (isfinite(s.sample.pressureKpa)) snprintf(value, sizeof(value), "%.1f", s.sample.pressureKpa);
    unit = "kPa";
  } else if (s.examination == ExaminationType::LipForce) {
    if (s.sample.hx711Ready && isfinite(s.sample.lipForce)) snprintf(value, sizeof(value), "%.1f", s.sample.lipForce);
    unit = "N";
  } else {
    if (isfinite(s.sample.emgFiltered)) snprintf(value, sizeof(value), "%.0f", s.sample.emgFiltered);
    unit = "raw";
  }
  tft_.setTextColor(TFT_WHITE, BG);
  tft_.drawString(value, 160, 105, 7);
  tft_.setTextColor(MUTED, BG);
  tft_.drawString(unit, 160, 143, 2);
  tft_.drawRoundRect(30, 174, 260, 16, 8, TFT_DARKGREY);
  tft_.fillRoundRect(32, 176, (256 * s.progress) / 100, 12, 6, TFT_GREEN);
  tft_.setTextColor(TFT_WHITE, BG);
  tft_.drawString("MEASURING", 160, 211, 2);
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
  tft_.drawString("Saved Local  |  Pending Sync", 160, 165, 2);
  tft_.drawString("OK  Return to Home", 160, 210, 2);
}

void UserInterface::render(const SharedStatus& s) {
  const bool pageChanged = s.state != lastState_ ||
      ((s.state == AppState::Home || s.state == AppState::ExaminationMenu || s.state == AppState::Settings) &&
       s.menuIndex != lastMenuIndex_);
  if (pageChanged) {
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
    else if (s.state == AppState::DevicePairing) drawPairing(s);
    else if (s.state == AppState::About) drawMessage(s, "TONGUE SMART v3");
  }
  if (s.state == AppState::Measurement && millis() - lastValuesDraw_ >= 250) {
    lastValuesDraw_ = millis();
    drawFrame(s);
    drawMeasurement(s);
  }
  if (s.state == AppState::DevicePairing && millis() - lastValuesDraw_ >= 500) {
    lastValuesDraw_ = millis();
    drawFrame(s);
    drawPairing(s);
  }
}
