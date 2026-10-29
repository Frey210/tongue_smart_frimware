#ifdef TONGUE_EMG_DIAGNOSTIC
#include <Arduino.h>
#include <TFT_eSPI.h>
#include "config.h"

// Diagnostic-only image: no network, motor commands or NVS writes.
struct Reading { uint32_t sequence, timeUs; uint16_t raw, millivolts; };
static QueueHandle_t samples;
static portMUX_TYPE guard = portMUX_INITIALIZER_UNLOCKED;
static volatile bool active = false;
static volatile bool finished = false;
static volatile bool stopRequested = false;
static uint32_t startUs, durationUs, acquired, lost;
static uint32_t completedCount, completedLost, completedUs;
static bool completedStopped = false;
static TFT_eSPI screen;
static char stage[24] = "idle";
static String command;
static uint32_t lastClock = 0;

static void sampler(void*) {
  analogReadResolution(12);
  analogSetPinAttenuation(hw::EMG_ADC, ADC_11db);
  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    bool enabled;
    uint32_t started, duration;
    portENTER_CRITICAL(&guard);
    enabled = active; started = startUs; duration = durationUs;
    portEXIT_CRITICAL(&guard);
    if (enabled) {
      const uint32_t elapsed = micros() - started;
      if (elapsed >= duration || stopRequested) {
        portENTER_CRITICAL(&guard);
        completedCount = acquired; completedLost = lost; completedUs = elapsed;
        completedStopped = stopRequested;
        active = false; finished = true;
        portEXIT_CRITICAL(&guard);
      } else {
        Reading reading{};
        reading.timeUs = elapsed;
        reading.raw = analogRead(hw::EMG_ADC);
        reading.millivolts = analogReadMilliVolts(hw::EMG_ADC);
        portENTER_CRITICAL(&guard);
        reading.sequence = acquired++;
        portEXIT_CRITICAL(&guard);
        if (xQueueSend(samples, &reading, 0) != pdTRUE) {
          portENTER_CRITICAL(&guard); ++lost; portEXIT_CRITICAL(&guard);
        }
      }
    }
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(1));
  }
}

static void info() {
  const uint64_t id = ESP.getEfuseMac();
  Serial.printf("{\"diagnostic\":\"emg-diag-1\",\"device_id\":\"TS-%04X%08X\","
                "\"sample_rate_target\":1000,\"baud\":460800,\"gain_verified\":false,"
                "\"motor_enabled\":false,\"network_enabled\":false}\n",
                static_cast<uint16_t>(id >> 32), static_cast<uint32_t>(id));
}

static void showStage() {
  screen.fillRect(0, 65, 320, 100, TFT_BLACK);
  screen.setTextDatum(MC_DATUM);
  screen.setTextColor(TFT_CYAN, TFT_BLACK);
  const char* label = strcmp(stage, "open") == 0 ? "ELEKTRODA LEPAS" :
      strcmp(stage, "rest") == 0 ? "RILEKS" :
      strncmp(stage, "clench_", 7) == 0 ? "GIGIT RINGAN" :
      strcmp(stage, "chew") == 0 ? "MENGUNYAH" :
      strcmp(stage, "recovery") == 0 ? "RILEKS KEMBALI" : "SIAP";
  screen.drawString(label, 160, 95, 4);
  screen.setTextColor(TFT_WHITE, TFT_BLACK);
  screen.drawString("Follow the PC wizard", 160, 137, 2);
}

static void handleCommand() {
  command.trim();
  if (command == "info") info();
  else if (command == "stop") {
    portENTER_CRITICAL(&guard);
    if (active) stopRequested = true;
    portEXIT_CRITICAL(&guard);
  } else if (command.startsWith("start ")) {
    char requested[24], extra;
    unsigned seconds;
    const int fields = sscanf(command.c_str(), "start %23s %u %c", requested, &seconds, &extra);
    const bool knownStage = fields == 2 && (strcmp(requested, "open") == 0 || strcmp(requested, "rest") == 0 ||
        strcmp(requested, "clench_1") == 0 || strcmp(requested, "clench_2") == 0 ||
        strcmp(requested, "clench_3") == 0 || strcmp(requested, "chew") == 0 || strcmp(requested, "recovery") == 0);
    if (!knownStage || seconds < 1 || seconds > 30 || active || finished) {
      Serial.println("{\"error\":\"invalid_start_or_busy\"}");
    } else {
      xQueueReset(samples);
      strlcpy(stage, requested, sizeof(stage));
      showStage();
      Serial.printf("{\"started\":\"%s\",\"seconds\":%u}\n", stage, seconds);
      portENTER_CRITICAL(&guard);
      acquired = lost = 0; stopRequested = false;
      durationUs = seconds * 1000000UL; startUs = micros(); active = true;
      portEXIT_CRITICAL(&guard);
    }
  } else Serial.println("{\"error\":\"commands: info, start <stage> <1-30 seconds>, stop\"}");
  command = "";
}

void setup() {
  pinMode(hw::STEPPER_STEP, OUTPUT); digitalWrite(hw::STEPPER_STEP, LOW);
  pinMode(hw::STEPPER_DIR, OUTPUT); digitalWrite(hw::STEPPER_DIR, LOW);
  pinMode(hw::BUTTON_BACK, INPUT_PULLUP);
  Serial.begin(460800);
  samples = xQueueCreate(512, sizeof(Reading));
  if (!samples) { Serial.println("{\"error\":\"sample_queue_allocation_failed\"}"); for (;;) delay(1000); }
  pinMode(hw::TFT_LED, OUTPUT); digitalWrite(hw::TFT_LED, HIGH);
  screen.init(); screen.setRotation(3); screen.fillScreen(TFT_BLACK);
  screen.setTextDatum(MC_DATUM); screen.setTextColor(TFT_WHITE, TFT_BLACK);
  screen.drawString("EMG DIAGNOSTIC", 160, 30, 4);
  screen.drawString("No motor motion / no WiFi", 160, 190, 2);
  screen.drawString("No calibration overwritten", 160, 218, 2);
  showStage();
  xTaskCreatePinnedToCore(sampler, "emg-diagnostic", 4096, nullptr, 4, nullptr, 0);
  info();
}

void loop() {
  while (Serial.available()) {
    const char ch = Serial.read();
    if (ch == '\n') handleCommand();
    else if (ch != '\r' && command.length() < 80) command += ch;
  }
  Reading sample;
  // Bound the drain so a stop command never waits behind a continuous stream.
  for (unsigned i = 0; i < 32 && xQueueReceive(samples, &sample, 0) == pdTRUE; ++i) {
    Serial.printf("S,%lu,%lu,%u,%u\n", static_cast<unsigned long>(sample.sequence),
      static_cast<unsigned long>(sample.timeUs), sample.raw, sample.millivolts);
  }
  bool complete;
  portENTER_CRITICAL(&guard); complete = finished; portEXIT_CRITICAL(&guard);
  if (complete && uxQueueMessagesWaiting(samples) == 0) {
    Serial.printf("{\"ended\":\"%s\",\"acquired\":%lu,\"lost\":%lu,\"duration_us\":%lu,\"stopped\":%s}\n",
      stage, static_cast<unsigned long>(completedCount), static_cast<unsigned long>(completedLost),
      static_cast<unsigned long>(completedUs), completedStopped ? "true" : "false");
    portENTER_CRITICAL(&guard); finished = false; portEXIT_CRITICAL(&guard);
    screen.fillRect(0, 65, 320, 110, TFT_BLACK);
    screen.setTextDatum(MC_DATUM); screen.setTextColor(TFT_GREEN, TFT_BLACK);
    screen.drawString("SELESAI - RILEKS", 160, 105, 4);
  }
  if (millis() - lastClock >= 250) {
    lastClock = millis();
    portENTER_CRITICAL(&guard);
    const bool running = active;
    const uint32_t elapsed = micros() - startUs;
    const uint32_t duration = durationUs;
    portEXIT_CRITICAL(&guard);
    if (running) {
      char text[32];
      const uint32_t remaining = elapsed < duration ? (duration - elapsed + 999999) / 1000000 : 0;
      snprintf(text, sizeof(text), "Sisa %lu detik", static_cast<unsigned long>(remaining));
      screen.fillRect(0, 150, 320, 26, TFT_BLACK);
      screen.setTextDatum(MC_DATUM); screen.setTextColor(TFT_WHITE, TFT_BLACK);
      screen.drawString(text, 160, 162, 2);
    }
  }
  if (digitalRead(hw::BUTTON_BACK) == LOW) {
    command = "stop"; handleCommand();
  }
  delay(1);
}
#endif
