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
constexpr bool STEPPER_DIR_INVERTED = true;
}

namespace cfg {
constexpr char FIRMWARE_VERSION[] = "0.4.3";
constexpr char DEFAULT_API_BASE[] = "https://tongue-smart.farlabs.my.id/api/v1";
constexpr uint32_t SERIAL_BAUD = 115200;
constexpr uint32_t SENSOR_PERIOD_MS = 10;   // 100 Hz baseline
constexpr uint32_t GUI_PERIOD_MS = 50;      // 20 FPS
constexpr uint32_t HTTP_SYNC_TASK_STACK_BYTES = 16384; // TLS handshake needs substantially more than 6 KB
constexpr uint32_t EXAM_DURATION_MS = 5000;
constexpr uint32_t CONTROL_POLL_MS = 500;
constexpr uint32_t REMOTE_SAMPLE_MS = 50;
constexpr uint8_t REMOTE_BATCH_SAMPLES = 5;
constexpr long LIP_FORCE_TRAVEL_STEPS = 2000;
constexpr float LIP_FORCE_MOTOR_SPEED = 300.0F;
constexpr float LIP_FORCE_MOTOR_ACCELERATION = 300.0F;
constexpr uint32_t MOTOR_RETURN_TIMEOUT_MS = 15000;
constexpr float ADC_MAX_COUNT = 4095.0F;
constexpr float ADC_REFERENCE_UV = 3300000.0F;
constexpr float EMG_ADC_BIAS = 2047.5F;      // Tune from the sensor's zero-signal baseline
constexpr float EMG_FRONTEND_GAIN = 1000.0F; // Replace with the measured analog front-end gain
constexpr float EMG_ENVELOPE_ALPHA = 0.12F;
constexpr float HX711_COUNTS_PER_NEWTON = 1.0F; // MUST be calibrated with a known force
constexpr long HX711_OFFSET = 0;
constexpr float FSR_ZERO_ADC = 0.0F;
constexpr float FSR_KPA_PER_COUNT = 100.0F / ADC_MAX_COUNT; // Replace with fitted sensor calibration
constexpr uint16_t ADC_SATURATION_COUNT = 4090;
}
