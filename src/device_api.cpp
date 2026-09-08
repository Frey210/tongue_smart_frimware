#include "device_api.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <mbedtls/sha256.h>
#include "config.h"

namespace {
String sha256Hex(const String& value) {
  uint8_t digest[32]{};
  mbedtls_sha256_ret(reinterpret_cast<const unsigned char*>(value.c_str()), value.length(), digest, 0);
  char encoded[65]{};
  for (size_t i = 0; i < sizeof(digest); ++i) snprintf(encoded + i * 2, 3, "%02x", digest[i]);
  return String(encoded);
}

String randomSecret() {
  char secret[65]{};
  for (size_t i = 0; i < 8; ++i) {
    const uint32_t value = esp_random();
    snprintf(secret + i * 8, 9, "%08lx", static_cast<unsigned long>(value));
  }
  return String(secret);
}

String canonicalFloat(float value) {
  String encoded(value, 3);
  while (encoded.endsWith("0") && !encoded.endsWith(".0")) encoded.remove(encoded.length() - 1);
  return encoded;
}

void copyJsonString(JsonVariantConst source, char* destination, size_t size) {
  strlcpy(destination, source.is<const char*>() ? source.as<const char*>() : "", size);
}

template <typename ClientType>
int executeHttp(HTTPClient& http, ClientType& client, const String& url, const char* method,
                const String* body, String& response, bool authenticated,
                const String& deviceId, const String& deviceSecret) {
  if (!http.begin(client, url)) return -1;
  http.setConnectTimeout(8000);
  http.setTimeout(12000);
  http.addHeader("Accept", "application/json");
  if (body) http.addHeader("Content-Type", "application/json");
  if (authenticated) {
    http.addHeader("X-Device-ID", deviceId);
    http.addHeader("X-Device-Key", deviceSecret);
  }
  const int result = strcmp(method, "POST") == 0
      ? http.POST(body ? *body : String("{}"))
      : http.GET();
  if (result > 0) response = http.getString();
  http.end();
  return result;
}
}

void DeviceApi::generateIdentity() {
  const uint64_t mac = ESP.getEfuseMac();
  char uid[20]{};
  snprintf(uid, sizeof(uid), "%04X%08X", static_cast<uint16_t>(mac >> 32), static_cast<uint32_t>(mac));
  hardwareUid_ = String("ESP32S3:") + uid;
  deviceId_ = String("TS-") + uid;
}

void DeviceApi::begin(Preferences& preferences) {
  preferences_ = &preferences;
  generateIdentity();
  baseUrl_ = preferences.getString("api_base", cfg::DEFAULT_API_BASE);
  deviceId_ = preferences.getString("dev_id", deviceId_);
  hardwareUid_ = preferences.getString("hw_uid", hardwareUid_);
  deviceSecret_ = preferences.getString("dev_key", "");
  pairingToken_ = preferences.getString("pair_tok", "");
  pairingCode_ = preferences.getString("pair_code", "");
  paired_ = preferences.getBool("paired", false) && deviceSecret_.length() >= 32;
  if (deviceSecret_.length() < 32) {
    deviceSecret_ = randomSecret();
    preferences.putString("dev_key", deviceSecret_);
  }
  preferences.putString("dev_id", deviceId_);
  preferences.putString("hw_uid", hardwareUid_);
}

void DeviceApi::setBaseUrl(const char* value) {
  if (!value || value[0] == '\0') return;
  baseUrl_ = value;
  while (baseUrl_.endsWith("/")) baseUrl_.remove(baseUrl_.length() - 1);
  if (preferences_) preferences_->putString("api_base", baseUrl_);
}

int DeviceApi::request(const char* method, const String& path, const String* body,
                       String& response, bool authenticated) {
  if (WiFi.status() != WL_CONNECTED) return -1;
  const String url = baseUrl_ + path;
  HTTPClient http;
  int code = -1;
  if (url.startsWith("https://")) {
    WiFiClientSecure client;
    // Phase 1 uses the Cloudflare hostname. Replace with a pinned CA before clinical validation.
    client.setInsecure();
    code = executeHttp(http, client, url, method, body, response, authenticated, deviceId_, deviceSecret_);
  } else {
    WiFiClient client;
    code = executeHttp(http, client, url, method, body, response, authenticated, deviceId_, deviceSecret_);
  }
  return code;
}

bool DeviceApi::startPairing(String& pairingCode) {
  JsonDocument body;
  body["device_id"] = deviceId_;
  body["hardware_uid"] = hardwareUid_;
  body["device_secret"] = deviceSecret_;
  body["firmware_version"] = cfg::FIRMWARE_VERSION;
  JsonObject capabilities = body["capabilities"].to<JsonObject>();
  capabilities["transport"].to<JsonArray>().add("https");
  capabilities["emg_channels"] = 1;
  capabilities["tongue_pressure_channels"] = 1;
  capabilities["lip_force"] = true;
  capabilities["motorized_traction"] = true;
  capabilities["wifi_portal"] = true;
  capabilities["mqtt"] = false;
  String payload;
  serializeJson(body, payload);
  String response;
  if (request("POST", "/device/pairings", &payload, response, false) != 201) return false;
  JsonDocument decoded;
  if (deserializeJson(decoded, response)) return false;
  pairingToken_ = decoded["pairing_token"].as<String>();
  pairingCode_ = decoded["pairing_code"].as<String>();
  if (pairingToken_.isEmpty() || pairingCode_.isEmpty()) return false;
  preferences_->putString("pair_tok", pairingToken_);
  preferences_->putString("pair_code", pairingCode_);
  pairingCode = pairingCode_;
  return true;
}

bool DeviceApi::pollPairing(String& status) {
  if (pairingToken_.isEmpty()) return false;
  String response;
  if (request("GET", "/device/pairings/" + pairingToken_, nullptr, response, false) != 200) return false;
  JsonDocument decoded;
  if (deserializeJson(decoded, response)) return false;
  status = decoded["status"].as<String>();
  if (status == "claimed") {
    paired_ = true;
    preferences_->putBool("paired", true);
    preferences_->remove("pair_tok");
    preferences_->remove("pair_code");
    pairingToken_ = "";
    pairingCode_ = "";
  }
  return true;
}

bool DeviceApi::fetchActiveControl(RemoteSessionControl& control) {
  String response;
  const int code = request("GET", "/device/sessions/active?device_id=" + deviceId_, nullptr, response, true);
  if (code != 200) return false;
  JsonDocument decoded;
  if (deserializeJson(decoded, response)) return false;
  JsonArrayConst sessions = decoded.as<JsonArrayConst>();
  control = {};
  if (sessions.isNull() || sessions.size() == 0) return true;
  JsonObjectConst session = sessions[0];
  control.sessionAvailable = true;
  copyJsonString(session["id"], control.sessionId, sizeof(control.sessionId));
  control.nextSequence = session["next_sequence"] | 0;
  JsonVariantConst remote = session["control"];
  if (remote.isNull()) return true;
  copyJsonString(remote["measurement"], control.measurement, sizeof(control.measurement));
  copyJsonString(remote["phase"], control.phase, sizeof(control.phase));
  copyJsonString(remote["protocol_stage"], control.protocolStage, sizeof(control.protocolStage));
  copyJsonString(remote["fsr_point"], control.fsrPoint, sizeof(control.fsrPoint));
  control.acquisitionEnabled = strcmp(control.phase, "baseline") == 0 || strcmp(control.phase, "recording") == 0;
  return true;
}

bool DeviceApi::sendBatch(const RemoteSessionControl& control, const RemoteSample* samples,
                          size_t sampleCount, uint32_t sequence, uint32_t& acknowledgedSequence) {
  if (!control.sessionAvailable || !samples || sampleCount == 0) return false;
  const char* channel = strcmp(control.measurement, "emg") == 0 ? "emg_1" :
      strcmp(control.measurement, "lip_force") == 0 ? "lip_force_1" : "fsr_1";
  const char* unit = strcmp(control.measurement, "emg") == 0 ? "uV" :
      strcmp(control.measurement, "lip_force") == 0 ? "N" : "kPa";
  String stage = control.protocolStage;
  if (control.fsrPoint[0]) stage += String(":") + control.fsrPoint;

  JsonDocument sampleDocument;
  JsonArray sampleArray = sampleDocument.to<JsonArray>();
  for (size_t i = 0; i < sampleCount; ++i) {
    JsonObject item = sampleArray.add<JsonObject>();
    // Alphabetical insertion order matches the backend canonical JSON checksum contract.
    item["calibrated_value"] = serialized(canonicalFloat(samples[i].calibratedValue));
    item["measurement_unit"] = unit;
    item["protocol_stage"] = stage;
    item["raw_value"] = serialized(canonicalFloat(samples[i].rawValue));
    item["sensor_channel"] = channel;
    item["signal_quality"] = samples[i].signalValid ? "good" : "invalid";
    item["timestamp"] = samples[i].timestamp;
  }
  String canonicalSamples;
  serializeJson(sampleDocument, canonicalSamples);

  JsonDocument body;
  char messageId[96]{};
  // Deterministic per sequence so a lost HTTP response retries the same idempotency key.
  snprintf(messageId, sizeof(messageId), "%s-%s-%lu", deviceId_.c_str(), control.sessionId,
           static_cast<unsigned long>(sequence));
  body["message_id"] = messageId;
  body["device_id"] = deviceId_;
  body["sequence"] = sequence;
  body["checksum"] = sha256Hex(canonicalSamples);
  body["samples"] = serialized(canonicalSamples);
  String payload;
  serializeJson(body, payload);
  String response;
  const int code = request("POST", String("/sessions/") + control.sessionId + "/batches", &payload, response, true);
  if (code != 202) {
    Serial.printf("{\"sync_error\":%d,\"response\":%s}\n", code, response.c_str());
    return false;
  }
  JsonDocument decoded;
  if (deserializeJson(decoded, response)) return false;
  acknowledgedSequence = decoded["sequence"] | sequence;
  return true;
}

bool DeviceApi::postEvent(const char* event, const char* messageId, uint32_t uptimeMs) {
  JsonDocument body;
  body["schema_version"] = 1;
  body["message_id"] = messageId;
  body["device_id"] = deviceId_;
  body["event"] = event;
  body["firmware_version"] = cfg::FIRMWARE_VERSION;
  body["uptime_ms"] = uptimeMs;
  String payload;
  serializeJson(body, payload);
  String response;
  const int code = request("POST", "/device-events", &payload, response, paired_);
  return code >= 200 && code < 300;
}
