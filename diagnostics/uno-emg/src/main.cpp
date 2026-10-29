#include <Arduino.h>
#include <string.h>

// Standalone bench diagnostic. No electrode stimulation, EEPROM writes or motor.
static const uint32_t PERIOD_US = 2000; // 500 Hz, comfortably below serial bandwidth
static bool active = false;
static uint32_t startedUs, dueUs, samples, missed;
static char command[16];
static uint8_t commandLength = 0;
static bool commandOverflow = false;

static void info() {
  Serial.println(F("{\"diagnostic\":\"uno-emg-1\",\"pin\":\"A0\",\"sample_rate_target\":500,\"adc_max\":1023,\"vref_nominal_mv\":5000}"));
}

static void finish(bool stopped) {
  active = false;
  Serial.print(F("{\"ended\":true,\"samples\":")); Serial.print(samples);
  Serial.print(F(",\"missed_deadlines\":")); Serial.print(missed);
  Serial.print(F(",\"duration_us\":")); Serial.print(micros() - startedUs);
  Serial.print(F(",\"stopped\":")); Serial.print(stopped ? F("true") : F("false"));
  Serial.println('}');
}

static void handleCommand() {
  command[commandLength] = '\0';
  if (commandOverflow) Serial.println(F("{\"error\":\"command_too_long\"}"));
  else if (!strcmp(command, "info")) info();
  else if (!strcmp(command, "stop")) { if (active) finish(true); }
  else if (!strcmp(command, "start") && !active) {
    Serial.println(F("{\"started\":true,\"seconds\":10}"));
    samples = missed = 0;
    startedUs = dueUs = micros();
    active = true;
  } else Serial.println(F("{\"error\":\"invalid_command_or_busy\"}"));
  commandLength = 0;
  commandOverflow = false;
}

void setup() {
  analogReference(DEFAULT); // AVcc, nominal 5 V; not a precision voltage reference
  Serial.begin(230400);
  analogRead(A0); // Discard initial ADC conversion.
  info();
}

void loop() {
  while (Serial.available()) {
    const char c = Serial.read();
    if (c == '\n') handleCommand();
    else if (c != '\r') {
      if (commandLength < sizeof(command) - 1) command[commandLength++] = c;
      else commandOverflow = true;
    }
  }
  if (!active) return;
  const uint32_t now = micros();
  if (now - startedUs >= 10000000UL) { finish(false); return; }
  if (static_cast<int32_t>(now - dueUs) < 0) return;
  const uint32_t skipped = (now - dueUs) / PERIOD_US;
  missed += skipped;
  dueUs += (skipped + 1) * PERIOD_US;
  const uint16_t raw = analogRead(A0);
  Serial.print(F("S,")); Serial.print(samples++);
  Serial.print(','); Serial.print(now - startedUs);
  Serial.print(','); Serial.println(raw);
}
