#include "storage_manager.h"
#include <ArduinoJson.h>
#include <LittleFS.h>

bool StorageManager::begin() {
  if (!LittleFS.begin(true)) return false;
  if (!LittleFS.exists("/results")) LittleFS.mkdir("/results");
  if (!LittleFS.exists("/logs")) LittleFS.mkdir("/logs");
  if (!LittleFS.exists("/device.json")) {
    File f = LittleFS.open("/device.json", FILE_WRITE);
    if (f) { f.print("{\"name\":\"Tongue Smart v3\",\"offline\":true}"); f.close(); }
  }
  return true;
}

bool StorageManager::saveResult(const ExaminationResult& r) {
  char path[64];
  snprintf(path, sizeof(path), "/results/%s.json", r.id);
  File f = LittleFS.open(path, FILE_WRITE);
  if (!f) return false;
  JsonDocument doc;
  doc["id"] = r.id; doc["started_ms"] = r.startedMs; doc["duration_ms"] = r.durationMs;
  doc["peak_pressure_kpa"] = r.peakPressureKpa; doc["peak_lip_force"] = r.peakLipForce;
  doc["mean_emg"] = r.meanEmg; doc["sample_count"] = r.sampleCount;
  doc["sync_status"] = "pending";
  const bool ok = serializeJson(doc, f) > 0;
  f.close();
  return ok;
}

void StorageManager::listResults(Stream& out) {
  File dir = LittleFS.open("/results");
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    while (f.available()) out.write(f.read());
    out.println();
  }
}

bool StorageManager::clearResults() {
  File dir = LittleFS.open("/results");
  bool ok = true;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    String path = String("/results/") + f.name();
    f.close();
    ok = LittleFS.remove(path) && ok;
  }
  return ok;
}

