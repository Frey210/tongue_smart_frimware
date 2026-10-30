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
#include "emg_calibration_selftest.h"
#include "emg_envelope.h"

QueueHandle_t gSensorQueue, gButtonQueue, gMotorQueue, gStorageQueue, gSensorCommandQueue, gWifiCommandQueue, gSyncQueue,
    gRemoteMeasurementQueue;
EventGroupHandle_t gSystemEvents;
SemaphoreHandle_t gStatusMutex;
SharedStatus gStatus{};

static HX711 scale;
static AccelStepper stepper(AccelStepper::DRIVER, hw::STEPPER_STEP, hw::STEPPER_DIR);
static StorageManager storage;
static UserInterface ui;
static Preferences preferences;
static Preferences emgPreferences;
static DeviceApi deviceApi;
static uint8_t lipForceDurationSeconds = cfg::LIP_FORCE_DURATION_DEFAULT_SECONDS;

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

static void updateWifiSetup(uint8_t step, const char* message) {
  xSemaphoreTake(gStatusMutex, portMAX_DELAY);
  gStatus.state = AppState::WifiSetup;
  gStatus.wifiSetupStep = step;
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
  EmgEnvelope emgEnvelope;
  EmgCalibration lastCalibration;
  uint32_t emgFrameSequence = 0;
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
    sample.emgFrameSequence = ++emgFrameSequence;
    uint32_t emgRawSum = 0, emgMvSum = 0;
    for (unsigned i = 0; i < cfg::EMG_AVERAGE_SAMPLES; ++i) {
      sample.emgRaw = analogRead(hw::EMG_ADC);
      const uint32_t mv = analogReadMilliVolts(hw::EMG_ADC);
      emgRawSum += sample.emgRaw;
      emgMvSum += mv;
      sample.emgClipped |= sample.emgRaw <= 5 || sample.emgRaw >= cfg::ADC_SATURATION_COUNT || mv < 100 || mv > 3050;
    }
    sample.emgAverageRaw = emgRawSum / cfg::EMG_AVERAGE_SAMPLES;
    sample.emgOutputMv = static_cast<float>(emgMvSum) / cfg::EMG_AVERAGE_SAMPLES;
    sample.fsrRaw = analogRead(hw::FSR_ADC);
    xSemaphoreTake(gStatusMutex, portMAX_DELAY);
    const EmgCalibration calibration = gStatus.emgCalibration;
    xSemaphoreGive(gStatusMutex);
    if (memcmp(&calibration, &lastCalibration, sizeof(calibration)) != 0) emgEnvelope.reset();
    lastCalibration = calibration;
    const float centeredMv = calibration.correctedMv(sample.emgOutputMv) - calibration.baselineMv;
    sample.emgMicrovolts = emgEnvelope.update(centeredMv, sample.timestampMs) *
                          1000.0F / cfg::EMG_FRONTEND_GAIN;
    sample.pressureKpa = max(0.0F, (static_cast<float>(sample.fsrRaw) - cfg::FSR_ZERO_ADC) * cfg::FSR_KPA_PER_COUNT);
    sample.fsrSaturated = sample.fsrRaw >= cfg::ADC_SATURATION_COUNT;
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

static bool saveEmgCalibration(const EmgCalibration& calibration) {
  if (!calibration.valid() ||
      emgPreferences.putBytes("profile", &calibration, sizeof(calibration)) != sizeof(calibration)) return false;
  xSemaphoreTake(gStatusMutex, portMAX_DELAY);
  gStatus.emgCalibration = calibration;
  gStatus.localBaselineReady = false;
  gStatus.emgReferenceMv = static_cast<uint16_t>(calibration.referenceMv);
  xSemaphoreGive(gStatusMutex);
  return true;
}

static void baselineState(const char* state, const char* reason) {
  xSemaphoreTake(gStatusMutex, portMAX_DELAY);
  strlcpy(gStatus.baselineState, state, sizeof(gStatus.baselineState));
  strlcpy(gStatus.baselineReason, reason, sizeof(gStatus.baselineReason));
  xSemaphoreGive(gStatusMutex);
}

static void captureRestBaseline(const RemoteMeasurementCommand* remote = nullptr) {
  xSemaphoreTake(gStatusMutex, portMAX_DELAY);
  gStatus.localBaselineReady = false;
  gStatus.restCandidate = {};
  strlcpy(gStatus.baselineSessionId, remote ? remote->sessionId : "", sizeof(gStatus.baselineSessionId));
  strlcpy(gStatus.baselineControlId, remote ? remote->controlId : "", sizeof(gStatus.baselineControlId));
  gStatus.progress = 0;
  xSemaphoreGive(gStatusMutex);
  baselineState("capturing", "Countdown: relax jaw, keep contact still");
  updateStatus(AppState::EmgCapture, "Relax; no talking or chewing");
  const uint32_t started = millis();
  uint32_t previous = 0;
  EmgRestCapture capture;
  for (;;) {
    ButtonEvent button{};
    const bool cancelled = xQueueReceive(gButtonQueue, &button, 0) == pdTRUE && button.id == ButtonId::Back;
    xSemaphoreTake(gStatusMutex, portMAX_DELAY);
    const bool superseded = remote
        ? strcmp(gStatus.webSessionId, remote->sessionId) != 0 || strcmp(gStatus.webControlId, remote->controlId) != 0
        : gStatus.webSessionActive;
    const SensorSample sample = gStatus.sample;
    const uint32_t elapsed = millis() - started;
    gStatus.progress = elapsed < 3000 ? 0 : min<uint32_t>(100, (elapsed - 3000) / 100);
    gStatus.countdown = elapsed < 3000 ? 3 - elapsed / 1000 : 0;
    xSemaphoreGive(gStatusMutex);
    if (cancelled || superseded) {
      baselineState("failed", cancelled ? "Cancelled; old baseline kept" : "Control changed; baseline discarded");
      updateStatus(AppState::EmgReview, "Baseline not saved");
      return;
    }
    if (elapsed >= 3000 && sample.timestampMs != previous) {
      capture.add(sample.emgOutputMv, sample.emgClipped, elapsed - 3000);
      previous = sample.timestampMs;
    }
    if (elapsed >= 13000) { capture.durationMs = elapsed - 3000; break; }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  xSemaphoreTake(gStatusMutex, portMAX_DELAY);
  gStatus.restCandidate = capture;
  xSemaphoreGive(gStatusMutex);
  const char* rejection = capture.rejection();
  baselineState(rejection ? "failed" : "review", rejection ? rejection : "Review baseline before saving");
  updateStatus(AppState::EmgReview, remote ? "Confirm on dashboard" : "OK Save  BACK Discard");
}

static void acceptRestBaseline() {
  xSemaphoreTake(gStatusMutex, portMAX_DELAY);
  const EmgRestCapture capture = gStatus.restCandidate;
  const bool review = strcmp(gStatus.baselineState, "review") == 0;
  EmgCalibration calibration = gStatus.emgCalibration;
  const bool web = gStatus.baselineSessionId[0];
  xSemaphoreGive(gStatusMutex);
  if (!review || capture.rejection()) return;
  calibration.baselineMv = calibration.correctedMv(capture.meanMv);
  calibration.flags |= 2;
  const bool saved = saveEmgCalibration(calibration);
  baselineState(saved ? "ready" : "failed", saved ? "Baseline saved; uV remains estimated" : "Flash write failed; retry baseline");
  xSemaphoreTake(gStatusMutex, portMAX_DELAY);
  gStatus.localBaselineReady = saved && !web;
  xSemaphoreGive(gStatusMutex);
  updateStatus(AppState::EmgReview, web ? "Continue from dashboard" : "OK Continue  BACK Return");
}

static void captureEmgCalibration(bool dc) {
  if (gStatus.webSessionActive) {
    updateStatus(AppState::EmgCalibrationMenu, "Active web session: use dashboard", true);
    return;
  }
  updateStatus(AppState::EmgCapture, dc ? "DC check: hold SIG voltage steady" : "Rest: keep muscle relaxed");
  EmgCapture capture;
  uint32_t lastTimestamp = 0;
  const uint32_t started = millis();
  for (;;) {
    ButtonEvent button{};
    if (xQueueReceive(gButtonQueue, &button, 0) == pdTRUE && button.id == ButtonId::Back) {
      updateStatus(AppState::EmgCalibrationMenu, "Cancelled; old values kept", true);
      return;
    }
    RemoteMeasurementCommand remote{};
    if (xQueuePeek(gRemoteMeasurementQueue, &remote, 0) == pdTRUE && remote.start) {
      updateStatus(AppState::EmgCalibrationMenu, "Cancelled: web measurement starting", true);
      return;
    }
    SensorSample sample{};
    if (xQueuePeek(gSensorQueue, &sample, 0) == pdTRUE && sample.timestampMs != lastTimestamp) {
      capture.add(sample.emgClipped ? 4095 : sample.emgRaw, sample.emgOutputMv);
      lastTimestamp = sample.timestampMs;
    }
    if (millis() - started >= 4000) break;
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  if (!capture.stable()) {
    updateStatus(AppState::EmgCalibrationMenu,
      capture.clipped ? "Rejected: ADC clipping / out of range" : "Rejected: unstable or missing signal", true);
    return;
  }
  xSemaphoreTake(gStatusMutex, portMAX_DELAY);
  EmgCalibration calibration = gStatus.emgCalibration;
  const float reference = gStatus.emgReferenceMv;
  xSemaphoreGive(gStatusMutex);
  if (dc) {
    calibration.dcOffsetMv = reference - capture.mean();
    calibration.referenceMv = reference;
    calibration.flags = 1;
    calibration.baselineMv = 1650;
  } else {
    calibration.baselineMv = calibration.correctedMv(capture.mean());
    calibration.flags |= 2;
  }
  const bool saved = saveEmgCalibration(calibration);
  updateStatus(AppState::EmgCalibrationMenu, saved ?
      (dc ? "DC saved; capture rest again" : "Rest saved; gain still estimated") :
      "Not saved: invalid value / flash error", true);
}

static void runMeasurement(ExaminationType type, bool remoteControlled = false) {
  if (type == ExaminationType::Emg) {
    xSemaphoreTake(gStatusMutex, portMAX_DELAY);
    const bool ready = remoteControlled
        ? strcmp(gStatus.baselineState, "ready") == 0 && strcmp(gStatus.baselineSessionId, gStatus.webSessionId) == 0
        : gStatus.localBaselineReady && !gStatus.webSessionActive;
    xSemaphoreGive(gStatusMutex);
    if (!ready) {
      updateStatus(AppState::EmgBaseline, "Capture rest baseline before measuring");
      return;
    }
  }
  if (type == ExaminationType::LipForce && !calibrateLipForce()) return;
  if (!remoteControlled && !countdown()) return;
  if (!remoteControlled && gStatus.webSessionActive) {
    updateStatus(AppState::Home, "Active web session: use dashboard", true);
    return;
  }

  ExaminationResult result{};
  const uint32_t started = millis();
  const uint32_t lipForceDurationMs = static_cast<uint32_t>(lipForceDurationSeconds) * 1000U;
  result.startedMs = started;
  float emgSum = 0;
  const long lipForceStartPosition = stepper.currentPosition();
  xSemaphoreTake(gStatusMutex, portMAX_DELAY);
  gStatus.examination = type;
  gStatus.remoteControlled = remoteControlled;
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
  while (type != ExaminationType::LipForce || millis() - started < lipForceDurationMs) {
    if (xQueueReceive(gSensorQueue, &sample, pdMS_TO_TICKS(20)) == pdTRUE) {
      xSemaphoreTake(gStatusMutex, portMAX_DELAY);
      gStatus.sample = sample;
      gStatus.progress = type == ExaminationType::LipForce
          ? min<uint32_t>(100, ((millis() - started) * 100) / lipForceDurationMs)
          : 0;
      xSemaphoreGive(gStatusMutex);
      if (type == ExaminationType::TonguePressure && isfinite(sample.pressureKpa))
        result.peakPressureKpa = max(result.peakPressureKpa, sample.pressureKpa);
      else if (type == ExaminationType::LipForce && sample.hx711Ready && isfinite(sample.lipForce))
        result.peakLipForce = max(result.peakLipForce, fabsf(sample.lipForce));
      else if (type == ExaminationType::Emg && isfinite(sample.emgMicrovolts))
        emgSum += sample.emgMicrovolts;
      ++result.sampleCount;
    }
    RemoteMeasurementCommand remote{};
    if (xQueueReceive(gRemoteMeasurementQueue, &remote, 0) == pdTRUE) {
      if ((remote.start || remote.confirmBaseline) &&
          (strcmp(remote.sessionId, gStatus.webSessionId) != 0 || strcmp(remote.controlId, gStatus.webControlId) != 0)) continue;
      if (!remote.start && !remote.confirmBaseline) break;
      if (!remoteControlled || remote.type != type || remote.baseline || remote.confirmBaseline) {
        xQueueSendToFront(gRemoteMeasurementQueue, &remote, 0);
        break;
      }
    }
    if (xQueueReceive(gButtonQueue, &button, 0) == pdTRUE) {
      if (type != ExaminationType::LipForce && !remoteControlled &&
          (button.id == ButtonId::Ok || button.id == ButtonId::Back)) break;
      if (button.id == ButtonId::Back) {
        cancelled = true;
        break;
      }
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
    if (remoteControlled && type == ExaminationType::Emg)
      baselineState("failed", "Stopped on device; repeat baseline from web");
    xSemaphoreTake(gStatusMutex, portMAX_DELAY);
    gStatus.remoteControlled = false;
    xSemaphoreGive(gStatusMutex);
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
  xSemaphoreTake(gStatusMutex, portMAX_DELAY);
  gStatus.remoteControlled = false;
  if (!remoteControlled) gStatus.localBaselineReady = false;
  xSemaphoreGive(gStatusMutex);
}

static void appTask(void*) {
  updateStatus(AppState::SelfTest, "Checking hardware...");
  const EventBits_t required = EVT_SENSOR_OK | EVT_STORAGE_OK | EVT_DISPLAY_OK;
  const EventBits_t bits = xEventGroupWaitBits(gSystemEvents, required, pdFALSE, pdTRUE, pdMS_TO_TICKS(5000));
  if ((bits & required) != required) updateStatus(AppState::Error, "Self-test failed");
  else updateStatus(AppState::Home, "Ready", true);

  ButtonEvent event{};
  for (;;) {
    RemoteMeasurementCommand remote{};
    if (xQueueReceive(gRemoteMeasurementQueue, &remote, 0) == pdTRUE) {
      if (remote.confirmBaseline) {
        if (strcmp(remote.controlId, gStatus.baselineControlId) == 0 &&
            strcmp(remote.controlId, gStatus.webControlId) == 0 &&
            strcmp(remote.sessionId, gStatus.webSessionId) == 0) acceptRestBaseline();
      } else if (remote.baseline) captureRestBaseline(&remote);
      else if (remote.start) runMeasurement(remote.type, true);
      continue;
    }
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
        if (gStatus.webSessionActive && (index == 0 || index == 2)) {
          updateStatus(AppState::Home, "Active web session: use dashboard");
          continue;
        }
        if (index == 0) updateStatus(AppState::ExaminationMenu, "Choose examination", true);
        else if (index == 1) updateStatus(AppState::History, "Use USB command: results");
        else if (index == 2) updateStatus(AppState::EmgCalibrationMenu, "EMG: gain not calibrated", true);
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
        gStatus.localBaselineReady = false;
        xSemaphoreGive(gStatusMutex);
        updateStatus(AppState::PatientReady,
          type == ExaminationType::LipForce ? "Install mouthpiece, then press OK" :
          type == ExaminationType::Emg ? "Attach electrodes, then press OK" :
          "Position the probe, then press OK");
      }
    } else if (state == AppState::PatientReady && event.id == ButtonId::Ok) {
      if (gStatus.webSessionActive) updateStatus(AppState::Home, "Active web session: use dashboard", true);
      else if (gStatus.examination == ExaminationType::Emg && !gStatus.localBaselineReady) {
        gStatus.baselineForMeasurement = true;
        updateStatus(AppState::EmgBaseline, "Attach electrodes; relax and keep still");
      }
      else runMeasurement(gStatus.examination);
    } else if (state == AppState::EmgCalibrationMenu) {
      if (event.id == ButtonId::Up || event.id == ButtonId::Down) {
        xSemaphoreTake(gStatusMutex, portMAX_DELAY);
        gStatus.menuIndex = event.id == ButtonId::Up ? (index + 3) % 4 : (index + 1) % 4;
        xSemaphoreGive(gStatusMutex);
      } else if (event.id == ButtonId::Back) {
        updateStatus(AppState::Home, "Ready", true);
      } else if (event.id == ButtonId::Ok) {
        if (gStatus.webSessionActive) {
          updateStatus(AppState::Home, "Active web session: use dashboard", true);
          continue;
        }
        if (index == 0) {
          xSemaphoreTake(gStatusMutex, portMAX_DELAY);
          gStatus.emgReferenceMv = static_cast<uint16_t>(gStatus.emgCalibration.referenceMv);
          gStatus.emgDigit = 0;
          xSemaphoreGive(gStatusMutex);
          updateStatus(AppState::EmgReference, "Enter simultaneous SIG-GND DC reading");
        } else if (index == 1) {
          gStatus.baselineForMeasurement = false;
          updateStatus(AppState::EmgBaseline, "Use resting signal, not open electrodes");
        } else if (index == 2) {
          updateStatus(AppState::EmgReset, "OK clears DC and rest on this device");
        } else {
          updateStatus(AppState::Home, "Ready", true);
        }
      }
    } else if (state == AppState::EmgReference) {
      if (event.id == ButtonId::Up || event.id == ButtonId::Down) {
        xSemaphoreTake(gStatusMutex, portMAX_DELAY);
        const int places[] = {1000, 100, 10, 1};
        const int delta = (event.id == ButtonId::Up ? 1 : -1) * places[gStatus.emgDigit];
        gStatus.emgReferenceMv = constrain(static_cast<int>(gStatus.emgReferenceMv) + delta, 100, 3000);
        xSemaphoreGive(gStatusMutex);
      } else if (event.id == ButtonId::Ok) {
        if (gStatus.emgDigit < 3) {
          xSemaphoreTake(gStatusMutex, portMAX_DELAY);
          ++gStatus.emgDigit;
          xSemaphoreGive(gStatusMutex);
        } else captureEmgCalibration(true);
      }
      else if (event.id == ButtonId::Back) updateStatus(AppState::EmgCalibrationMenu, "No changes saved", true);
    } else if (state == AppState::EmgBaseline) {
      if (event.id == ButtonId::Ok && !gStatus.webSessionActive) captureRestBaseline();
      else if (event.id == ButtonId::Back) updateStatus(AppState::EmgCalibrationMenu, "No changes saved", true);
    } else if (state == AppState::EmgReview) {
      if (event.id == ButtonId::Back) {
        if (!gStatus.baselineSessionId[0] && strcmp(gStatus.baselineState, "review") == 0)
          baselineState("failed", "Discarded; old baseline kept");
        updateStatus(AppState::Home, "Ready", true);
      } else if (event.id == ButtonId::Ok && !gStatus.webSessionActive) {
        if (strcmp(gStatus.baselineState, "review") == 0) acceptRestBaseline();
        else if (gStatus.localBaselineReady) updateStatus(gStatus.baselineForMeasurement ? AppState::PatientReady : AppState::Home,
            gStatus.baselineForMeasurement ? "Baseline ready. OK starts measurement" : "Baseline ready", true);
        else updateStatus(AppState::EmgBaseline, "Check contact, then retry");
      }
    } else if (state == AppState::EmgReset) {
      if (event.id == ButtonId::Ok) {
        if (gStatus.webSessionActive) { updateStatus(AppState::Home, "Active web session: use dashboard", true); continue; }
        const bool saved = saveEmgCalibration(EmgCalibration{});
        updateStatus(AppState::EmgCalibrationMenu, saved ? "Defaults restored on this device" : "Reset failed: flash error", true);
      } else if (event.id == ButtonId::Back) updateStatus(AppState::EmgCalibrationMenu, "No changes saved", true);
    } else if (state == AppState::Settings) {
      if (event.id == ButtonId::Up || event.id == ButtonId::Down) {
        index = event.id == ButtonId::Up ? (index + 2) % 3 : (index + 1) % 3;
        xSemaphoreTake(gStatusMutex, portMAX_DELAY);
        gStatus.menuIndex = index;
        xSemaphoreGive(gStatusMutex);
      } else if (event.id == ButtonId::Back) {
        updateStatus(AppState::Home, "Ready", true);
      } else if (event.id == ButtonId::Ok) {
        if (index == 2) {
          xSemaphoreTake(gStatusMutex, portMAX_DELAY);
          gStatus.lipForceDurationSeconds = lipForceDurationSeconds;
          xSemaphoreGive(gStatusMutex);
          updateStatus(AppState::LipForceSetup, "Adjust pull duration");
        } else {
          const WifiCommand command{index == 0 ? WifiCommandType::StartPortal : WifiCommandType::StartPairing};
          if (xQueueSend(gWifiCommandQueue, &command, 0) != pdTRUE) {
            updateStatus(AppState::Settings, "Connectivity request busy");
          } else if (index == 0) {
            updateWifiSetup(0, "Preparing access point...");
          } else {
            updateStatus(AppState::DevicePairing, "Requesting pairing code...");
          }
        }
      }
    } else if (state == AppState::WifiSetup && event.id == ButtonId::Back) {
      const WifiCommand command{WifiCommandType::StopPortal};
      xQueueSend(gWifiCommandQueue, &command, 0);
      updateWifiSetup(4, "Closing setup portal...");
    } else if (state == AppState::LipForceSetup) {
      if (event.id == ButtonId::Up || event.id == ButtonId::Down) {
        xSemaphoreTake(gStatusMutex, portMAX_DELAY);
        uint8_t& seconds = gStatus.lipForceDurationSeconds;
        if (event.id == ButtonId::Up && seconds < cfg::LIP_FORCE_DURATION_MAX_SECONDS) ++seconds;
        if (event.id == ButtonId::Down && seconds > cfg::LIP_FORCE_DURATION_MIN_SECONDS) --seconds;
        xSemaphoreGive(gStatusMutex);
      } else if (event.id == ButtonId::Ok) {
        xSemaphoreTake(gStatusMutex, portMAX_DELAY);
        lipForceDurationSeconds = gStatus.lipForceDurationSeconds;
        xSemaphoreGive(gStatusMutex);
        preferences.putUChar("lip_secs", lipForceDurationSeconds);
        updateStatus(AppState::Settings, "Lip force duration saved", true);
      } else if (event.id == ButtonId::Back) {
        xSemaphoreTake(gStatusMutex, portMAX_DELAY);
        gStatus.lipForceDurationSeconds = lipForceDurationSeconds;
        xSemaphoreGive(gStatusMutex);
        updateStatus(AppState::Settings, "Duration unchanged", true);
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
        if (line == "emg_selftest") {
          Serial.printf("{\"emg_selftest\":\"%s\",\"envelope_selftest\":\"%s\",\"rest_selftest\":\"%s\"}\n",
            emgCalibrationSelftest() ? "passed" : "failed", emgEnvelopeSelftest() ? "passed" : "failed", emgRestSelftest() ? "passed" : "failed");
        } else if (line == "emg_status") {
          xSemaphoreTake(gStatusMutex, portMAX_DELAY);
          const SensorSample sample = gStatus.sample;
          const EmgCalibration calibration = gStatus.emgCalibration;
          const AppState state = gStatus.state;
          xSemaphoreGive(gStatusMutex);
          Serial.printf("{\"device_id\":\"%s\",\"firmware\":\"%s\",\"state\":%u,\"emg_adc\":%u,"
            "\"emg_adc_mv\":%.1f,\"emg_corrected_mv\":%.1f,\"emg_offset_mv\":%.1f,"
            "\"emg_baseline_mv\":%.1f,\"emg_reference_mv\":%.1f,\"calibration_flags\":%lu,"
            "\"gain_verified\":false,\"emg_uv_estimate\":%.2f,\"sample_timestamp_ms\":%lu,\"emg_frame_sequence\":%lu,\"emg_avg_adc\":%u,\"emg_clipped\":%s}\n",
            deviceApi.deviceId(), cfg::FIRMWARE_VERSION, static_cast<unsigned>(state), sample.emgRaw,
            sample.emgOutputMv, calibration.correctedMv(sample.emgOutputMv), calibration.dcOffsetMv,
            calibration.baselineMv, calibration.referenceMv, static_cast<unsigned long>(calibration.flags), sample.emgMicrovolts,
            static_cast<unsigned long>(sample.timestampMs), static_cast<unsigned long>(sample.emgFrameSequence),
            sample.emgAverageRaw, sample.emgClipped ? "true" : "false");
        } else if (line == "status") {
          xSemaphoreTake(gStatusMutex, portMAX_DELAY);
          Serial.printf("{\"state\":%u,\"exam\":%u,\"sensor_ready\":%s,\"fsr_adc\":%u,\"fsr_saturated\":%s,\"pressure_kpa\":%.2f,\"lip_force\":",
            static_cast<unsigned>(gStatus.state), static_cast<unsigned>(gStatus.examination),
            gStatus.sample.hx711Ready ? "true" : "false", gStatus.sample.fsrRaw,
            gStatus.sample.fsrSaturated ? "true" : "false", gStatus.sample.pressureKpa);
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
        } else if (line == "help") Serial.println("commands: help, status, emg_status, emg_selftest, results, wifi_setup, pair_device");
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
  WiFiManager manager;
  char configuredBase[160]{};
  WiFiManagerParameter apiParameter("api_base", "API base URL", "", sizeof(configuredBase) - 1);
  bool portalActive = false;
  uint32_t portalStarted = 0;
  WifiCommand command{};
  uint32_t lastCheck = 0;
  uint32_t lastHeartbeat = 0;
  bool wasConnected = false;
  bool timeConfigured = false;

  manager.addParameter(&apiParameter);
  manager.setConfigPortalBlocking(false);
  manager.setConnectTimeout(20);
  manager.setSaveConfigCallback([]() { updateWifiSetup(2, "Credentials saved, connecting..."); });

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

    if (portalActive) {
      const bool connected = manager.process();
      if (connected) {
        portalActive = false;
        manager.stopConfigPortal();
        xEventGroupClearBits(gSystemEvents, EVT_WIFI_PORTAL_ACTIVE);
        updateWifiSetup(2, "Validating network connection...");
        vTaskDelay(pdMS_TO_TICKS(500));
        const char* configuredUrl = apiParameter.getValue();
        if (configuredUrl && configuredUrl[0] != '\0') deviceApi.setBaseUrl(configuredUrl);
        setWifiConnected(true);
        char message[64];
        snprintf(message, sizeof(message), "Connected to %.32s", WiFi.SSID().c_str());
        updateWifiSetup(3, message);
        enqueueSyncEvent("online", "wifi-setup");
        vTaskDelay(pdMS_TO_TICKS(1500));
        updateStatus(AppState::Home, "WiFi connected", true);
      } else if (millis() - portalStarted >= 180000U) {
        portalActive = false;
        manager.stopConfigPortal();
        xEventGroupClearBits(gSystemEvents, EVT_WIFI_PORTAL_ACTIVE);
        updateWifiSetup(4, "Setup timed out");
        vTaskDelay(pdMS_TO_TICKS(1500));
        updateStatus(AppState::Settings, "WiFi setup timed out", true);
      }
    }

    if (xQueueReceive(gWifiCommandQueue, &command, pdMS_TO_TICKS(100)) != pdTRUE) continue;
    if (command.type == WifiCommandType::StopPortal) {
      if (portalActive) manager.stopConfigPortal();
      portalActive = false;
      xEventGroupClearBits(gSystemEvents, EVT_WIFI_PORTAL_ACTIVE);
      updateStatus(AppState::Settings, "WiFi setup cancelled", true);
      continue;
    }
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

    if (portalActive) continue;
    strlcpy(configuredBase, deviceApi.baseUrl(), sizeof(configuredBase));
    apiParameter.setValue(configuredBase, sizeof(configuredBase) - 1);
    updateWifiSetup(0, "Starting TongueSmart-Setup...");
    manager.startConfigPortal("TongueSmart-Setup");
    portalActive = true;
    portalStarted = millis();
    xEventGroupSetBits(gSystemEvents, EVT_WIFI_PORTAL_ACTIVE);
    updateWifiSetup(1, "Portal ready");
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
    snprintf(destination, size, "2026-01-01T00:00:00.%03lu000Z",
             static_cast<unsigned long>(millis() % 1000));
    return;
  }
  char seconds[24]{};
  strftime(seconds, sizeof(seconds), "%Y-%m-%dT%H:%M:%S", &utc);
  snprintf(destination, size, "%s.%03lu000Z", seconds,
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
  char activePhase[16]{};
  char activeStage[64]{};
  char activeFsrPoint[32]{};
  char baselineSentId[40]{}, acceptSentId[40]{}, lastReportId[40]{}, lastReportState[16]{};
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
      RemoteSessionControl next{};
      if (deviceApi.fetchActiveControl(next)) {
        xSemaphoreTake(gStatusMutex, portMAX_DELAY);
        gStatus.webSessionActive = next.sessionAvailable;
        strlcpy(gStatus.webSessionId, next.sessionId, sizeof(gStatus.webSessionId));
        strlcpy(gStatus.webControlId, next.controlId, sizeof(gStatus.webControlId));
        xSemaphoreGive(gStatusMutex);
        const bool wasEnabled = control.acquisitionEnabled;
        const bool sourceChanged = strcmp(activeSession, next.sessionId) != 0 ||
                                   strcmp(activeMeasurement, next.measurement) != 0;
        const bool changed = strcmp(activeSession, next.sessionId) != 0 ||
                             strcmp(activeMeasurement, next.measurement) != 0 ||
                             strcmp(activePhase, next.phase) != 0 ||
                             strcmp(activeStage, next.protocolStage) != 0 ||
                             strcmp(activeFsrPoint, next.fsrPoint) != 0;
        control = next;
        const bool emgBaseline = strcmp(control.measurement, "emg") == 0 && strcmp(control.phase, "baseline") == 0;
        if (emgBaseline && strcmp(baselineSentId, control.controlId) != 0 &&
            (strcmp(control.baselineState, "requested") == 0 || strcmp(control.baselineState, "capturing") == 0)) {
          RemoteMeasurementCommand command{ExaminationType::Emg, true};
          command.baseline = true;
          strlcpy(command.sessionId, control.sessionId, sizeof(command.sessionId));
          strlcpy(command.controlId, control.controlId, sizeof(command.controlId));
          if (xQueueSend(gRemoteMeasurementQueue, &command, 0) == pdTRUE)
            strlcpy(baselineSentId, control.controlId, sizeof(baselineSentId));
        }
        if (emgBaseline && strcmp(control.baselineState, "accepted") == 0 && strcmp(acceptSentId, control.controlId) != 0) {
          RemoteMeasurementCommand command{ExaminationType::Emg, false};
          command.confirmBaseline = true;
          strlcpy(command.sessionId, control.sessionId, sizeof(command.sessionId));
          strlcpy(command.controlId, control.controlId, sizeof(command.controlId));
          if (xQueueSend(gRemoteMeasurementQueue, &command, 0) == pdTRUE)
            strlcpy(acceptSentId, control.controlId, sizeof(acceptSentId));
        }
        // Persisted server readiness cannot restore a lost in-RAM candidate after reboot.
        if ((strcmp(control.baselineState, "ready") == 0 || strcmp(control.baselineState, "review") == 0 ||
             strcmp(control.baselineState, "accepted") == 0) &&
            (strcmp(gStatus.baselineControlId, control.baselineControlId) != 0 ||
             (strcmp(control.baselineState, "ready") == 0 && strcmp(gStatus.baselineState, "ready") != 0))) {
          xSemaphoreTake(gStatusMutex, portMAX_DELAY);
          strlcpy(gStatus.baselineSessionId, control.sessionId, sizeof(gStatus.baselineSessionId));
          strlcpy(gStatus.baselineControlId, control.baselineControlId, sizeof(gStatus.baselineControlId));
          xSemaphoreGive(gStatusMutex);
          baselineState("failed", "Device restarted; take a new baseline");
        }
        if (control.acquisitionEnabled && (!wasEnabled || sourceChanged)) {
          const ExaminationType type = strcmp(control.measurement, "emg") == 0
              ? ExaminationType::Emg
              : strcmp(control.measurement, "lip_force") == 0
                  ? ExaminationType::LipForce : ExaminationType::TonguePressure;
          RemoteMeasurementCommand command{type, true};
          strlcpy(command.sessionId, control.sessionId, sizeof(command.sessionId));
          strlcpy(command.controlId, control.controlId, sizeof(command.controlId));
          xQueueSend(gRemoteMeasurementQueue, &command, 0);
        } else if (!control.acquisitionEnabled && wasEnabled) {
          const RemoteMeasurementCommand command{ExaminationType::TonguePressure, false};
          xQueueSend(gRemoteMeasurementQueue, &command, 0);
        }
        if (changed) {
          strlcpy(activeSession, control.sessionId, sizeof(activeSession));
          strlcpy(activeMeasurement, control.measurement, sizeof(activeMeasurement));
          strlcpy(activePhase, control.phase, sizeof(activePhase));
          strlcpy(activeStage, control.protocolStage, sizeof(activeStage));
          strlcpy(activeFsrPoint, control.fsrPoint, sizeof(activeFsrPoint));
          sequence = control.nextSequence;
          batchCount = 0;
          Serial.printf("{\"remote_control\":\"%s\",\"phase\":\"%s\",\"stage\":\"%s\"}\n",
                        control.measurement, control.phase, control.protocolStage);
        } else if (batchCount == 0) {
          sequence = control.nextSequence;
        }
      }
      // Measure the interval after the blocking HTTPS call, otherwise a slow
      // handshake causes another poll before samples can fill the batch.
      lastControlPoll = millis();
    }

    SharedStatus report{};
    xSemaphoreTake(gStatusMutex, portMAX_DELAY);
    report = gStatus;
    xSemaphoreGive(gStatusMutex);
    if (report.baselineControlId[0] &&
        (strcmp(lastReportId, report.baselineControlId) != 0 || strcmp(lastReportState, report.baselineState) != 0) &&
        deviceApi.reportBaseline(report)) {
      strlcpy(lastReportId, report.baselineControlId, sizeof(lastReportId));
      strlcpy(lastReportState, report.baselineState, sizeof(lastReportState));
    }

    const bool emgCanStream = strcmp(control.measurement, "emg") != 0 ||
        (report.state == AppState::Measurement && report.remoteControlled && report.examination == ExaminationType::Emg &&
         strcmp(report.baselineState, "ready") == 0 && strcmp(report.baselineSessionId, control.sessionId) == 0);
    if (!control.acquisitionEnabled || !emgCanStream) {
      batchCount = 0;
    } else if (batchCount < cfg::REMOTE_BATCH_SAMPLES &&
               millis() - lastRemoteSample >= cfg::REMOTE_SAMPLE_MS) {
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
        point.signalValid = !sample.emgClipped && strcmp(report.baselineState, "ready") == 0 &&
            strcmp(report.baselineSessionId, control.sessionId) == 0;
      } else if (strcmp(control.measurement, "lip_force") == 0) {
        point.rawValue = sample.lipForce;
        point.calibratedValue = sample.lipForce;
        point.signalValid = sample.hx711Ready;
      } else {
        point.rawValue = sample.fsrRaw;
        point.calibratedValue = sample.pressureKpa;
        point.signalValid = !sample.fsrSaturated;
      }
    }

    if (batchCount >= cfg::REMOTE_BATCH_SAMPLES) {
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
  gRemoteMeasurementQueue = xQueueCreate(4, sizeof(RemoteMeasurementCommand));
  gSystemEvents = xEventGroupCreate();
  gStatusMutex = xSemaphoreCreateMutex();
  gStatus.state = AppState::Boot;
  strlcpy(gStatus.message, "Starting...", sizeof(gStatus.message));
  preferences.begin("tongue-smart", false);
  emgPreferences.begin("emg-cal", false);
  EmgCalibration loadedCalibration;
  if (emgPreferences.getBytesLength("profile") == sizeof(loadedCalibration)) {
    emgPreferences.getBytes("profile", &loadedCalibration, sizeof(loadedCalibration));
    if (!loadedCalibration.valid()) loadedCalibration = EmgCalibration{};
  }
  gStatus.emgCalibration = loadedCalibration;
  gStatus.emgReferenceMv = static_cast<uint16_t>(loadedCalibration.referenceMv);
  lipForceDurationSeconds = constrain(
      preferences.getUChar("lip_secs", cfg::LIP_FORCE_DURATION_DEFAULT_SECONDS),
      cfg::LIP_FORCE_DURATION_MIN_SECONDS, cfg::LIP_FORCE_DURATION_MAX_SECONDS);
  gStatus.lipForceDurationSeconds = lipForceDurationSeconds;
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
