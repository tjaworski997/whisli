#pragma once

#include <Arduino.h>

namespace whisli::system::backup {

// Configuration backup: every stored entry of the devices, inputs, lights, actions, zones and presets,
// plus the network and MQTT settings that are not secret, as JSON. With a backup password the Wi-Fi
// network, the access point password and the MQTT password come along too, encrypted (AES-256-GCM,
// key from the password via PBKDF2). The panel password never does.
//
// {"application":"Whisli","format":1,"firmware":"…","entries":[["namespace","key","type","value"],…],
//  "secrets":{"cipher":"aes-256-gcm",…,"data":"…"}}
// Values are strings: numbers in decimal, text as is, blobs in base64.
String exportJson(const String& passphrase = String());

struct ImportResult {
  String error;               // empty on success (the caller then restarts)
  bool secretsPresent = false;
  bool secretsApplied = false;
  bool wrongPassphrase = false;
};

// Replaces the configuration with the one in `json`; network and MQTT settings are merged. Everything
// is checked before anything is written. The encrypted part needs `passphrase`; without it the rest
// still loads.
ImportResult import(const char* json, size_t length, const String& passphrase = String());

// Start-up, before the modules load: moves their configuration from the settings storage (NVS) to
// files once (see ConfigStore). The network, MQTT and password settings stay in NVS.
void moveConfigToFiles();

}  // namespace whisli::system::backup
