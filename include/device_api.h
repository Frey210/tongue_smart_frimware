#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include "types.h"

struct RemoteSessionControl {
  bool sessionAvailable = false;
  bool acquisitionEnabled = false;
  char sessionId[40]{};
  char measurement[24]{};
  char phase[16]{};
  char protocolStage[64]{};
  char fsrPoint[48]{};
  uint32_t nextSequence = 0;
};

struct RemoteSample {
  char timestamp[32]{};
  float rawValue = 0;
  float calibratedValue = 0;
};

class DeviceApi {
 public:
  void begin(Preferences& preferences);
  void setBaseUrl(const char* value);
  const char* baseUrl() const { return baseUrl_.c_str(); }
  const char* deviceId() const { return deviceId_.c_str(); }
  const char* hardwareUid() const { return hardwareUid_.c_str(); }
  bool isPaired() const { return paired_; }

  bool startPairing(String& pairingCode);
  bool pollPairing(String& status);
  bool fetchActiveControl(RemoteSessionControl& control);
  bool sendBatch(const RemoteSessionControl& control, const RemoteSample* samples,
                 size_t sampleCount, uint32_t sequence, uint32_t& acknowledgedSequence);
  bool postEvent(const char* event, const char* messageId, uint32_t uptimeMs);

 private:
  Preferences* preferences_ = nullptr;
  String baseUrl_;
  String deviceId_;
  String hardwareUid_;
  String deviceSecret_;
  String pairingToken_;
  String pairingCode_;
  bool paired_ = false;

  void generateIdentity();
  int request(const char* method, const String& path, const String* body, String& response,
              bool authenticated);
};
