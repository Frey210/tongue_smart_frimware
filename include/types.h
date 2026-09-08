#pragma once
#include <Arduino.h>

enum class AppState : uint8_t {
  Boot, SelfTest, Home, ExaminationMenu, PatientReady, Calibration,
  Countdown, Measurement, Processing, Result, Saving, History,
  Settings, DevicePairing, About, Error, Recovery
};
enum class ExaminationType : uint8_t { TonguePressure, LipForce, Emg, Complete };
enum class ButtonId : uint8_t { Up, Down, Ok, Back };
enum class SensorCommandType : uint8_t { TareHx711 };
enum class WifiCommandType : uint8_t { StartPortal, StartPairing, Disconnect };

struct SensorSample {
  uint32_t timestampMs;
  uint16_t emgRaw;
  uint16_t fsrRaw;
  float emgMicrovolts;
  float pressureKpa;
  float lipForce;
  bool hx711Ready;
};

struct ButtonEvent { ButtonId id; uint32_t timestampMs; };
struct MotorCommand { long targetSteps; float maxSpeed; float acceleration; };
struct SensorCommand { SensorCommandType type; };
struct WifiCommand { WifiCommandType type; };
struct SyncEvent { char event[24]; char messageId[64]; uint32_t uptimeMs; };

struct ExaminationResult {
  char id[40];
  uint32_t startedMs;
  uint32_t durationMs;
  float peakPressureKpa;
  float peakLipForce;
  float meanEmg;
  uint32_t sampleCount;
};

struct SharedStatus {
  AppState state;
  ExaminationType examination;
  uint8_t menuIndex;
  uint8_t countdown;
  uint8_t progress;
  bool wifiConnected;
  bool devicePaired;
  SensorSample sample;
  ExaminationResult result;
  char message[64];
  char deviceId[32];
  char pairingCode[16];
};

extern QueueHandle_t gSensorQueue;
extern QueueHandle_t gButtonQueue;
extern QueueHandle_t gMotorQueue;
extern QueueHandle_t gStorageQueue;
extern QueueHandle_t gSensorCommandQueue;
extern QueueHandle_t gWifiCommandQueue;
extern QueueHandle_t gSyncQueue;
extern EventGroupHandle_t gSystemEvents;
extern SemaphoreHandle_t gStatusMutex;
extern SharedStatus gStatus;

constexpr EventBits_t EVT_SENSOR_OK = BIT0;
constexpr EventBits_t EVT_STORAGE_OK = BIT1;
constexpr EventBits_t EVT_DISPLAY_OK = BIT2;
constexpr EventBits_t EVT_HX_TARE_OK = BIT3;
constexpr EventBits_t EVT_HX_TARE_FAILED = BIT4;
constexpr EventBits_t EVT_WIFI_CONNECTED = BIT5;
constexpr EventBits_t EVT_WIFI_PORTAL_ACTIVE = BIT6;
constexpr EventBits_t EVT_FATAL_ERROR = BIT7;
