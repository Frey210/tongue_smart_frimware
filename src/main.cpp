#include <Arduino.h>
#include <ArduinoJson.h>
#include <AccelStepper.h>
#include <HX711.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <math.h>
#include <time.h>
#include "config.h"
#include "device_api.h"
#include "storage_manager.h"
#include "types.h"
#include "ui.h"

QueueHandle_t gSensorQueue, gButtonQueue, gMotorQueue, gStorageQueue, gSensorCommandQueue, gWifiCommandQueue, gSyncQueue;
EventGroupHandle_t gSystemEvents;
SemaphoreHandle_t gStatusMutex;
SharedStatus gStatus{};

static HX711 scale;
static AccelStepper stepper(AccelStepper::DRIVER, hw::STEPPER_STEP, hw::STEPPER_DIR);
static StorageManager storage;
static UserInterface ui;
static Preferences preferences;
static DeviceApi deviceApi;

static void enqueueSyncEvent(const char* event, const char* suffix) {
  SyncEvent sync{};
  strlcpy(sync.event, event, sizeof(sync.event));
  sync.uptimeMs = millis();
  snprintf(sync.messageId, sizeof(sync.messageId), "%s-%08lx-%s", deviceApi.deviceId(),
    static_cast<unsigned long>(esp_random()), suffix);
  xQueueSend(gSyncQueue, &sync, 0);
}

static void setWifiConnected(bool connected) {
  xSemaphoreTake(gStatusMutex, portMAX_DELAY);
  gStatus.wifiConnected = connected;
  xSemaphoreGive(gStatusMutex);
  if (connected) xEventGroupSetBits(gSystemEvents, EVT_WIFI_CONNECTED);
  else xEventGroupClearBits(gSystemEvents, EVT_WIFI_CONNECTED);
}

static void updateStatus(AppState state, const char* message, bool resetMenu = false) {
  xSemaphoreTake(gStatusMutex, portMAX_DELAY);
  gStatus.state = state;
  if (resetMenu) gStatus.menuIndex = 0;
  strlcpy(gStatus.message, message, sizeof(gStatus.message));
  xSemaphoreGive(gStatusMutex);
}

static void stopMotor() {
  MotorCommand stop{stepper.currentPosition(), 1, 1};
  xQueueOverwrite(gMotorQueue, &stop);
}

static void sensorTask(void*) {
  analogReadResolution(12);
  analogSetPinAttenuation(hw::EMG_ADC, ADC_11db);
  analogSetPinAttenuation(hw::FSR_ADC, ADC_11db);
  scale.begin(hw::HX711_DT, hw::HX711_SCK);
  scale.set_scale(cfg::HX711_COUNTS_PER_NEWTON);
  scale.set_offset(cfg::HX711_OFFSET);
  float emgEma = 0;
  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    SensorCommand command{};
    if (xQueueReceive(gSensorCommandQueue, &command, 0) == pdTRUE &&
        command.type == SensorCommandType::TareHx711) {
      xEventGroupClearBits(gSystemEvents, EVT_HX_TARE_OK | EVT_HX_TARE_FAILED);
      bool valid = scale.wait_ready_timeout(1200);
      if (valid) {
        scale.tare(10);
        valid = scale.wait_ready_timeout(500);
        if (valid) {
          const float check = scale.get_units(3);
          valid = isfinite(check) && fabsf(check) < 100000.0F;
        }
      }
      xEventGroupSetBits(gSystemEvents, valid ? EVT_HX_TARE_OK : EVT_HX_TARE_FAILED);
    }

    SensorSample sample{};
    sample.timestampMs = millis();
    sample.emgRaw = analogRead(hw::EMG_ADC);
    sample.fsrRaw = analogRead(hw::FSR_ADC);
    const float emgUv = fabsf(static_cast<float>(sample.emgRaw) - cfg::EMG_ADC_BIAS) *
                        cfg::ADC_REFERENCE_UV / cfg::ADC_MAX_COUNT / cfg::EMG_FRONTEND_GAIN;
    emgEma += cfg::EMG_ENVELOPE_ALPHA * (emgUv - emgEma);
    sample.emgMicrovolts = isfinite(emgEma) ? emgEma : 0;
    sample.pressureKpa = max(0.0F, (static_cast<float>(sample.fsrRaw) - cfg::FSR_ZERO_ADC) * cfg::FSR_KPA_PER_COUNT);
    sample.hx711Ready = scale.is_ready();
    sample.lipForce = 0;
    if (sample.hx711Ready) {
      const float force = scale.get_units(1);
      if (isfinite(force) && fabsf(force) < 100000.0F) sample.lipForce = fabsf(force);
      else sample.hx711Ready = false;
    }
    xSemaphoreTake(gStatusMutex, portMAX_DELAY);
    gStatus.sample = sample;
    xSemaphoreGive(gStatusMutex);
    xQueueOverwrite(gSensorQueue, &sample);
    xEventGroupSetBits(gSystemEvents, EVT_SENSOR_OK);
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(cfg::SENSOR_PERIOD_MS));
  }
}

static void buttonTask(void*) {
  const uint8_t pins[] = {hw::BUTTON_UP, hw::BUTTON_DOWN, hw::BUTTON_OK, hw::BUTTON_BACK};
  const ButtonId ids[] = {ButtonId::Up, ButtonId::Down, ButtonId::Ok, ButtonId::Back};
  uint8_t stable[4] = {HIGH, HIGH, HIGH, HIGH};
  uint8_t last[4] = {HIGH, HIGH, HIGH, HIGH};
  uint32_t changed[4]{};
  for (uint8_t pin : pins) pinMode(pin, INPUT_PULLUP);
  for (;;) {
    for (int i = 0; i < 4; ++i) {
      const uint8_t now = digitalRead(pins[i]);
      if (now != last[i]) { last[i] = now; changed[i] = millis(); }
      if (now != stable[i] && millis() - changed[i] >= 30) {
        stable[i] = now;
        if (now == LOW) {
          ButtonEvent event{ids[i], millis()};
          xQueueSend(gButtonQueue, &event, 0);
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

static void motorTask(void*) {
  stepper.setPinsInverted(hw::STEPPER_DIR_INVERTED, false, false);
  stepper.setMaxSpeed(1000);
  stepper.setAcceleration(500);
  xEventGroupSetBits(gSystemEvents, EVT_MOTOR_IDLE);
  MotorCommand command{};
  bool moving = false;
  for (;;) {
    if (xQueueReceive(gMotorQueue, &command, 0) == pdTRUE) {
      xEventGroupClearBits(gSystemEvents, EVT_MOTOR_IDLE);
      stepper.setMaxSpeed(command.maxSpeed);
      stepper.setAcceleration(command.acceleration);
      stepper.moveTo(command.targetSteps);
      moving = stepper.distanceToGo() != 0;
      if (!moving) xEventGroupSetBits(gSystemEvents, EVT_MOTOR_IDLE);
    }
    stepper.run();
    if (moving && stepper.distanceToGo() == 0) {
      moving = false;
      xEventGroupSetBits(gSystemEvents, EVT_MOTOR_IDLE);
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

static void storageTask(void*) {
  if (storage.begin()) xEventGroupSetBits(gSystemEvents, EVT_STORAGE_OK);
  else xEventGroupSetBits(gSystemEvents, EVT_FATAL_ERROR);
  ExaminationResult result{};
  for (;;) {
    if (xQueueReceive(gStorageQueue, &result, portMAX_DELAY) == pdTRUE) {
      if (!storage.saveResult(result)) updateStatus(AppState::Error, "Failed to save result");
      else enqueueSyncEvent("measurement_saved", result.id);
    }
  }
}

static void guiTask(void*) {
  if (ui.begin()) xEventGroupSetBits(gSystemEvents, EVT_DISPLAY_OK);
  SharedStatus snapshot{};
  for (;;) {
    xSemaphoreTake(gStatusMutex, portMAX_DELAY);
    snapshot = gStatus;
    xSemaphoreGive(gStatusMutex);
    ui.render(snapshot);
    vTaskDelay(pdMS_TO_TICKS(cfg::GUI_PERIOD_MS));
  }
}

static bool awaitButton(ButtonId wanted, bool allowBack = true) {
  ButtonEvent event{};
  for (;;) {
    if (xQueueReceive(gButtonQueue, &event, portMAX_DELAY) == pdTRUE) {
      if (event.id == wanted) return true;
      if (allowBack && event.id == ButtonId::Back) {
        stopMotor();
        updateStatus(AppState::Home, "Ready", true);
        return false;
      }
    }
  }
}

static bool countdown() {
  updateStatus(AppState::Countdown, "Measurement starts shortly");
  for (int n = 3; n >= 1; --n) {
    xSemaphoreTake(gStatusMutex, portMAX_DELAY);
    gStatus.countdown = n;
    xSemaphoreGive(gStatusMutex);
    const uint32_t until = millis() + 1000;
    ButtonEvent event{};
    while (millis() < until) {
      if (xQueueReceive(gButtonQueue, &event, pdMS_TO_TICKS(20)) == pdTRUE &&
          event.id == ButtonId::Back) {
        stopMotor();
        updateStatus(AppState::Home, "Measurement cancelled", true);
        return false;
      }
    }
  }
  return true;
}

static bool calibrateLipForce() {
  updateStatus(AppState::Calibration, "Waiting for stable load cell...");
  SensorCommand command{SensorCommandType::TareHx711};
  xQueueSend(gSensorCommandQueue, &command, portMAX_DELAY);
  const EventBits_t bits = xEventGroupWaitBits(
      gSystemEvents, EVT_HX_TARE_OK | EVT_HX_TARE_FAILED, pdTRUE, pdFALSE, pdMS_TO_TICKS(5000));
  if (!(bits & EVT_HX_TARE_OK)) {
    updateStatus(AppState::Error, "Load cell unavailable");
    return false;
  }
  return true;
}

static void runMeasurement(ExaminationType type) {
  if (type == ExaminationType::LipForce && !calibrateLipForce()) return;
  if (!countdown()) return;

  ExaminationResult result{};
  const uint32_t started = millis();
  result.startedMs = started;
  float emgSum = 0;
  const long lipForceStartPosition = stepper.currentPosition();
  xSemaphoreTake(gStatusMutex, portMAX_DELAY);
  gStatus.examination = type;
  gStatus.progress = 0;
  xSemaphoreGive(gStatusMutex);
  updateStatus(AppState::Measurement, "Measuring");

  if (type == ExaminationType::LipForce) {
    // Placeholder speed until steps/mm is calibrated. Movement is only
    // permitted in Measurement and is stopped on completion/BACK.
    MotorCommand move{lipForceStartPosition + cfg::LIP_FORCE_TRAVEL_STEPS,
                      cfg::LIP_FORCE_MOTOR_SPEED, cfg::LIP_FORCE_MOTOR_ACCELERATION};
    xQueueOverwrite(gMotorQueue, &move);
  }

  SensorSample sample{};
  ButtonEvent button{};
  bool cancelled = false;
  while (millis() - started < cfg::EXAM_DURATION_MS) {
    if (xQueueReceive(gSensorQueue, &sample, pdMS_TO_TICKS(20)) == pdTRUE) {
      xSemaphoreTake(gStatusMutex, portMAX_DELAY);
      gStatus.sample = sample;
      gStatus.progress = min<uint32_t>(100, ((millis() - started) * 100) / cfg::EXAM_DURATION_MS);
      xSemaphoreGive(gStatusMutex);
      if (type == ExaminationType::TonguePressure && isfinite(sample.pressureKpa))
        result.peakPressureKpa = max(result.peakPressureKpa, sample.pressureKpa);
      else if (type == ExaminationType::LipForce && sample.hx711Ready && isfinite(sample.lipForce))
        result.peakLipForce = max(result.peakLipForce, fabsf(sample.lipForce));
      else if (type == ExaminationType::Emg && isfinite(sample.emgMicrovolts))
        emgSum += sample.emgMicrovolts;
      ++result.sampleCount;
    }
    if (xQueueReceive(gButtonQueue, &button, 0) == pdTRUE && button.id == ButtonId::Back) {
      cancelled = true;
      break;
    }
  }
  if (type == ExaminationType::LipForce) {
    updateStatus(AppState::Processing, "Returning lip force carriage...");
    xEventGroupClearBits(gSystemEvents, EVT_MOTOR_IDLE);
    MotorCommand reset{lipForceStartPosition, cfg::LIP_FORCE_MOTOR_SPEED,
                       cfg::LIP_FORCE_MOTOR_ACCELERATION};
    xQueueOverwrite(gMotorQueue, &reset);
    const EventBits_t motorBits = xEventGroupWaitBits(
        gSystemEvents, EVT_MOTOR_IDLE, pdFALSE, pdTRUE, pdMS_TO_TICKS(cfg::MOTOR_RETURN_TIMEOUT_MS));
    if (!(motorBits & EVT_MOTOR_IDLE)) {
      stopMotor();
      updateStatus(AppState::Error, "Motor failed to return home");
      return;
    }
  } else {
    stopMotor();
  }
  if (cancelled) {
    updateStatus(AppState::Home, "Measurement cancelled", true);
    return;
  }

  result.durationMs = millis() - started;
  result.meanEmg = result.sampleCount ? emgSum / result.sampleCount : 0;
  snprintf(result.id, sizeof(result.id), "result-%lu", static_cast<unsigned long>(started));
  updateStatus(AppState::Processing, "Calculating result...");
  vTaskDelay(pdMS_TO_TICKS(300));
  xSemaphoreTake(gStatusMutex, portMAX_DELAY);
  gStatus.result = result;
  xSemaphoreGive(gStatusMutex);
  updateStatus(AppState::Saving, "Saving result locally...");
  xQueueSend(gStorageQueue, &result, portMAX_DELAY);
  vTaskDelay(pdMS_TO_TICKS(250));
  updateStatus(AppState::Result, "Saved locally");
}

static void appTask(void*) {
  updateStatus(AppState::SelfTest, "Checking hardware...");
  const EventBits_t required = EVT_SENSOR_OK | EVT_STORAGE_OK | EVT_DISPLAY_OK;
  const EventBits_t bits = xEventGroupWaitBits(gSystemEvents, required, pdFALSE, pdTRUE, pdMS_TO_TICKS(5000));
  if ((bits & required) != required) updateStatus(AppState::Error, "Self-test failed");
  else updateStatus(AppState::Home, "Ready", true);

  ButtonEvent event{};
  for (;;) {
    if (xQueueReceive(gButtonQueue, &event, pdMS_TO_TICKS(50)) != pdTRUE) continue;
    AppState state;
    uint8_t index;
    xSemaphoreTake(gStatusMutex, portMAX_DELAY);
    state = gStatus.state;
    index = gStatus.menuIndex;
    xSemaphoreGive(gStatusMutex);

    if (state == AppState::Home) {
      if (event.id == ButtonId::Up || event.id == ButtonId::Down) {
        index = event.id == ButtonId::Up ? (index + 4) % 5 : (index + 1) % 5;
        xSemaphoreTake(gStatusMutex, portMAX_DELAY);
        gStatus.menuIndex = index;
        xSemaphoreGive(gStatusMutex);
      } else if (event.id == ButtonId::Ok) {
        if (index == 0) updateStatus(AppState::ExaminationMenu, "Choose examination", true);
        else if (index == 1) updateStatus(AppState::History, "Use USB command: results");
        else if (index == 2) updateStatus(AppState::Calibration, "Calibration menu");
        else if (index == 3) updateStatus(AppState::Settings, "Choose connectivity action", true);
        else updateStatus(AppState::About, "Offline clinical firmware");
      }
    } else if (state == AppState::ExaminationMenu) {
      if (event.id == ButtonId::Up || event.id == ButtonId::Down) {
        index = event.id == ButtonId::Up ? (index + 3) % 4 : (index + 1) % 4;
        xSemaphoreTake(gStatusMutex, portMAX_DELAY);
        gStatus.menuIndex = index;
        xSemaphoreGive(gStatusMutex);
      } else if (event.id == ButtonId::Back) updateStatus(AppState::Home, "Ready", true);
      else if (event.id == ButtonId::Ok) {
        const ExaminationType type = static_cast<ExaminationType>(index);
        xSemaphoreTake(gStatusMutex, portMAX_DELAY);
        gStatus.examination = type;
        xSemaphoreGive(gStatusMutex);
        updateStatus(AppState::PatientReady,
          type == ExaminationType::LipForce ? "Install mouthpiece, then press OK" :
          type == ExaminationType::Emg ? "Attach electrodes, then press OK" :
          "Position the probe, then press OK");
      }
    } else if (state == AppState::PatientReady && event.id == ButtonId::Ok) {
      runMeasurement(gStatus.examination);
    } else if (state == AppState::Settings) {
      if (event.id == ButtonId::Up || event.id == ButtonId::Down) {
        index = (index + 1) % 2;
        xSemaphoreTake(gStatusMutex, portMAX_DELAY);
        gStatus.menuIndex = index;
        xSemaphoreGive(gStatusMutex);
      } else if (event.id == ButtonId::Ok) {
        const WifiCommand command{index == 0 ? WifiCommandType::StartPortal : WifiCommandType::StartPairing};
        xQueueSend(gWifiCommandQueue, &command, 0);
        updateStatus(index == 0 ? AppState::Settings : AppState::DevicePairing,
                     index == 0 ? "Starting WiFi portal..." : "Requesting pairing code...");
      }
    } else if (state == AppState::Result && event.id == ButtonId::Ok) {
      updateStatus(AppState::Home, "Ready", true);
    } else if (event.id == ButtonId::Back) {
      stopMotor();
      updateStatus(AppState::Home, "Ready", true);
    }
  }
}

static void communicationTask(void*) {
  String line;
  for (;;) {
    while (Serial.available()) {
      const char c = Serial.read();
      if (c == '\n') {
        line.trim();
        if (line == "status") {
          xSemaphoreTake(gStatusMutex, portMAX_DELAY);
          Serial.printf("{\"state\":%u,\"exam\":%u,\"sensor_ready\":%s,\"pressure_kpa\":%.2f,\"lip_force\":",
            static_cast<unsigned>(gStatus.state), static_cast<unsigned>(gStatus.examination),
            gStatus.sample.hx711Ready ? "true" : "false", gStatus.sample.pressureKpa);
          if (gStatus.sample.hx711Ready && isfinite(gStatus.sample.lipForce)) Serial.print(gStatus.sample.lipForce, 2);
          else Serial.print("null");
          Serial.printf(",\"emg\":%.2f,\"wifi\":%s,\"paired\":%s,\"device_id\":\"%s\"}\n",
            gStatus.sample.emgMicrovolts, gStatus.wifiConnected ? "true" : "false",
            gStatus.devicePaired ? "true" : "false", gStatus.deviceId);
          xSemaphoreGive(gStatusMutex);
        } else if (line == "results") storage.listResults(Serial);
        else if (line == "wifi_setup") {
          const WifiCommand command{WifiCommandType::StartPortal};
          xQueueSend(gWifiCommandQueue, &command, 0);
          Serial.println("{\"wifi_portal\":\"starting\"}");
        } else if (line == "pair_device") {
          const WifiCommand command{WifiCommandType::StartPairing};
          xQueueSend(gWifiCommandQueue, &command, 0);
          Serial.println("{\"pairing\":\"starting\"}");
        } else if (line == "help") Serial.println("commands: help, status, results, wifi_setup, pair_device");
        else Serial.println("{\"error\":\"unknown command\"}");
        line = "";
      } else if (c != '\r' && line.length() < 96) line += c;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

static void wifiTask(void*) {
  WiFi.mode(WIFI_STA);
  WiFi.begin();
  WifiCommand command{};
  uint32_t lastCheck = 0;
  uint32_t lastHeartbeat = 0;
  bool wasConnected = false;
  bool timeConfigured = false;

  for (;;) {
    if (millis() - lastCheck >= 1000) {
      lastCheck = millis();
      const bool connected = WiFi.status() == WL_CONNECTED;
      setWifiConnected(connected);
      if (connected && !wasConnected) {
        enqueueSyncEvent("online", "wifi");
        if (!timeConfigured) {
          configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
          timeConfigured = true;
        }
      }
      if (connected && millis() - lastHeartbeat >= 15000) {
        lastHeartbeat = millis();
        enqueueSyncEvent("online", "heartbeat");
      }
      wasConnected = connected;
    }

    if (xQueueReceive(gWifiCommandQueue, &command, pdMS_TO_TICKS(100)) != pdTRUE) continue;
    if (command.type == WifiCommandType::Disconnect) {
      WiFi.disconnect(false, false);
      setWifiConnected(false);
      continue;
    }

    if (command.type == WifiCommandType::StartPairing) {
      if (WiFi.status() != WL_CONNECTED) {
        updateStatus(AppState::DevicePairing, "Connect WiFi before pairing");
        continue;
      }
      String pairingCode;
      if (!deviceApi.startPairing(pairingCode)) {
        updateStatus(AppState::DevicePairing, "Pairing request failed");
        continue;
      }
      xSemaphoreTake(gStatusMutex, portMAX_DELAY);
      strlcpy(gStatus.pairingCode, pairingCode.c_str(), sizeof(gStatus.pairingCode));
      strlcpy(gStatus.message, "Enter code in dashboard", sizeof(gStatus.message));
      xSemaphoreGive(gStatusMutex);
      Serial.printf("{\"pairing_code\":\"%s\",\"device_id\":\"%s\"}\n",
                    pairingCode.c_str(), deviceApi.deviceId());
      for (;;) {
        String pairingStatus;
        if (deviceApi.pollPairing(pairingStatus)) {
          if (pairingStatus == "claimed") {
            xSemaphoreTake(gStatusMutex, portMAX_DELAY);
            gStatus.devicePaired = true;
            gStatus.pairingCode[0] = '\0';
            xSemaphoreGive(gStatusMutex);
            updateStatus(AppState::Home, "Device registered", true);
            enqueueSyncEvent("online", "paired");
            break;
          }
          if (pairingStatus == "expired") {
            updateStatus(AppState::DevicePairing, "Pairing code expired");
            break;
          }
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
      }
      continue;
    }

    xEventGroupSetBits(gSystemEvents, EVT_WIFI_PORTAL_ACTIVE);
    updateStatus(AppState::Settings, "AP: TongueSmart-Setup");
    WiFiManager manager;
    char configuredBase[160]{};
    strlcpy(configuredBase, deviceApi.baseUrl(), sizeof(configuredBase));
    WiFiManagerParameter apiParameter("api_base", "API base URL", configuredBase, sizeof(configuredBase) - 1);
    manager.addParameter(&apiParameter);
    manager.setConfigPortalTimeout(180);
    manager.setConnectTimeout(20);
    const bool connected = manager.startConfigPortal("TongueSmart-Setup");
    const char* configuredUrl = apiParameter.getValue();
    if (configuredUrl && configuredUrl[0] != '\0') {
      deviceApi.setBaseUrl(configuredUrl);
    }
    setWifiConnected(connected && WiFi.status() == WL_CONNECTED);
    xEventGroupClearBits(gSystemEvents, EVT_WIFI_PORTAL_ACTIVE);
    updateStatus(AppState::Home, connected ? "WiFi connected" : "WiFi setup ended", true);
  }
}

static bool postSyncEvent(const SyncEvent& sync) {
  return deviceApi.postEvent(sync.event, sync.messageId, sync.uptimeMs);
}

static void utcTimestamp(char* destination, size_t size) {
  const time_t now = time(nullptr);
  struct tm utc{};
  gmtime_r(&now, &utc);
  if (now < 1700000000) {
    snprintf(destination, size, "2026-01-01T00:00:00.%03luZ",
             static_cast<unsigned long>(millis() % 1000));
    return;
  }
  char seconds[24]{};
  strftime(seconds, sizeof(seconds), "%Y-%m-%dT%H:%M:%S", &utc);
  snprintf(destination, size, "%s.%03luZ", seconds,
           static_cast<unsigned long>(millis() % 1000));
}

static void syncTask(void*) {
  SyncEvent sync{};
  uint32_t retryMs = 1000;
  uint32_t lastControlPoll = 0;
  uint32_t lastRemoteSample = 0;
  uint32_t sequence = 0;
  RemoteSessionControl control{};
  RemoteSample batch[cfg::REMOTE_BATCH_SAMPLES]{};
  size_t batchCount = 0;
  char activeSession[40]{};
  char activeMeasurement[24]{};
  for (;;) {
    if (xQueuePeek(gSyncQueue, &sync, 0) == pdTRUE) {
      if (postSyncEvent(sync)) {
        xQueueReceive(gSyncQueue, &sync, 0);
        retryMs = 1000;
      } else {
        vTaskDelay(pdMS_TO_TICKS(retryMs));
        retryMs = min<uint32_t>(retryMs * 2, 30000);
      }
    }

    if (deviceApi.isPaired() && WiFi.status() == WL_CONNECTED &&
        millis() - lastControlPoll >= cfg::CONTROL_POLL_MS) {
      lastControlPoll = millis();
      RemoteSessionControl next{};
      if (deviceApi.fetchActiveControl(next)) {
        const bool changed = strcmp(activeSession, next.sessionId) != 0 ||
                             strcmp(activeMeasurement, next.measurement) != 0;
        control = next;
        if (changed) {
          strlcpy(activeSession, control.sessionId, sizeof(activeSession));
          strlcpy(activeMeasurement, control.measurement, sizeof(activeMeasurement));
          sequence = control.nextSequence;
          batchCount = 0;
        } else if (batchCount == 0) {
          sequence = control.nextSequence;
        }
      }
    }

    if (!control.acquisitionEnabled) {
      batchCount = 0;
    } else if (millis() - lastRemoteSample >= cfg::REMOTE_SAMPLE_MS) {
      lastRemoteSample = millis();
      SensorSample sample{};
      xSemaphoreTake(gStatusMutex, portMAX_DELAY);
      sample = gStatus.sample;
      xSemaphoreGive(gStatusMutex);
      RemoteSample& point = batch[batchCount++];
      utcTimestamp(point.timestamp, sizeof(point.timestamp));
      if (strcmp(control.measurement, "emg") == 0) {
        point.rawValue = sample.emgRaw;
        point.calibratedValue = sample.emgMicrovolts;
      } else if (strcmp(control.measurement, "lip_force") == 0) {
        point.rawValue = sample.lipForce;
        point.calibratedValue = sample.lipForce;
      } else {
        point.rawValue = sample.fsrRaw;
        point.calibratedValue = sample.pressureKpa;
      }
    }

    if (batchCount == cfg::REMOTE_BATCH_SAMPLES) {
      uint32_t acknowledged = sequence;
      if (deviceApi.sendBatch(control, batch, batchCount, sequence, acknowledged)) {
        sequence = acknowledged + 1;
        batchCount = 0;
      } else {
        vTaskDelay(pdMS_TO_TICKS(1000));
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void setup() {
  Serial.begin(cfg::SERIAL_BAUD);
  delay(300);
  gSensorQueue = xQueueCreate(1, sizeof(SensorSample));
  gButtonQueue = xQueueCreate(12, sizeof(ButtonEvent));
  gMotorQueue = xQueueCreate(1, sizeof(MotorCommand));
  gStorageQueue = xQueueCreate(4, sizeof(ExaminationResult));
  gSensorCommandQueue = xQueueCreate(2, sizeof(SensorCommand));
  gWifiCommandQueue = xQueueCreate(2, sizeof(WifiCommand));
  gSyncQueue = xQueueCreate(8, sizeof(SyncEvent));
  gSystemEvents = xEventGroupCreate();
  gStatusMutex = xSemaphoreCreateMutex();
  gStatus.state = AppState::Boot;
  strlcpy(gStatus.message, "Starting...", sizeof(gStatus.message));
  preferences.begin("tongue-smart", false);
  deviceApi.begin(preferences);
  gStatus.devicePaired = deviceApi.isPaired();
  strlcpy(gStatus.deviceId, deviceApi.deviceId(), sizeof(gStatus.deviceId));
  enqueueSyncEvent("boot", "boot");

  xTaskCreatePinnedToCore(sensorTask, "sensor", 4096, nullptr, 5, nullptr, 0);
  xTaskCreatePinnedToCore(motorTask, "motor", 3072, nullptr, 5, nullptr, 0);
  xTaskCreatePinnedToCore(buttonTask, "button", 3072, nullptr, 4, nullptr, 1);
  xTaskCreatePinnedToCore(guiTask, "gui", 4096, nullptr, 3, nullptr, 1);
  xTaskCreatePinnedToCore(communicationTask, "usb", 4096, nullptr, 2, nullptr, 1);
  xTaskCreatePinnedToCore(wifiTask, "wifi", 6144, nullptr, 2, nullptr, 1);
  xTaskCreatePinnedToCore(syncTask, "http-sync", cfg::HTTP_SYNC_TASK_STACK_BYTES, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(storageTask, "storage", 6144, nullptr, 1, nullptr, 1);
  xTaskCreatePinnedToCore(appTask, "application", 6144, nullptr, 4, nullptr, 1);
  Serial.println("Tongue Smart v3 clinical workflow ready. Type 'help'.");
}

void loop() { vTaskDelay(pdMS_TO_TICKS(1000)); }
