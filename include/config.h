#pragma once
#include <Arduino.h>

namespace hw {
constexpr uint8_t TFT_LED = 21;
constexpr uint8_t BUTTON_UP = 37;
constexpr uint8_t BUTTON_DOWN = 38;
constexpr uint8_t BUTTON_OK = 39;
constexpr uint8_t BUTTON_BACK = 40;
constexpr uint8_t EMG_ADC = 4;
constexpr uint8_t FSR_ADC = 5;
constexpr uint8_t HX711_DT = 6;
constexpr uint8_t HX711_SCK = 7;
constexpr uint8_t STEPPER_DIR = 35;
constexpr uint8_t STEPPER_STEP = 36;
}

namespace cfg {
constexpr char FIRMWARE_VERSION[] = "0.3.0";
constexpr char DEFAULT_API_BASE[] = "https://tongue-smart.farlabs.my.id/api/v1";
constexpr uint32_t SERIAL_BAUD = 115200;
constexpr uint32_t SENSOR_PERIOD_MS = 10;   // 100 Hz baseline
constexpr uint32_t GUI_PERIOD_MS = 50;      // 20 FPS
constexpr uint32_t EXAM_DURATION_MS = 5000;
constexpr uint32_t CONTROL_POLL_MS = 1500;
constexpr uint32_t REMOTE_SAMPLE_MS = 100;
constexpr uint8_t REMOTE_BATCH_SAMPLES = 10;
constexpr float HX711_SCALE = 1.0F;         // MUST be calibrated before clinical use
constexpr long HX711_OFFSET = 0;
constexpr float FSR_FULL_SCALE_KPA = 100.0F;
}
