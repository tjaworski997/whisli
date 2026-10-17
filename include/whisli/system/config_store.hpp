#pragma once

#include <Arduino.h>
#include <nvs.h>

#include <functional>
#include <vector>

namespace whisli::system {

// Mounts the file system on the "spiffs" partition (3.4 MB, LittleFS), formatting it once when
// empty. Safe to call again; false when it cannot be used.
bool mountFiles();

// Configuration storage with the subset of the Preferences API the modules use, kept as files on
// the file-system partition instead of the small settings storage (NVS, 20 KB): one file per key,
// "/cfg/<namespace>/<key>", holding a type byte (an nvs_type_t, for backups) and the value.
// Writes go to a temporary file renamed over the old one, so a power cut keeps the previous value,
// and unchanged values are not rewritten.
class ConfigStore {
 public:
  bool begin(const char* space, bool readOnly = false);
  void end() {}

  bool isKey(const char* key);
  bool remove(const char* key);
  bool clear();  // every key of the namespace

  size_t getBytesLength(const char* key);
  size_t getBytes(const char* key, void* buffer, size_t length);
  size_t putBytes(const char* key, const void* data, size_t length);
  uint8_t getUChar(const char* key, uint8_t fallback = 0);
  size_t putUChar(const char* key, uint8_t value);
  bool getBool(const char* key, bool fallback = false);
  size_t putBool(const char* key, bool value);
  String getString(const char* key, const String& fallback = String());
  size_t putString(const char* key, const String& value);

  // Raw access for configuration backups.
  bool read(const char* key, nvs_type_t& type, std::vector<uint8_t>& data);
  bool write(const char* key, nvs_type_t type, const void* data, size_t length);
  void forEach(const std::function<void(const String& key, nvs_type_t type, const std::vector<uint8_t>& data)>& visit);

 private:
  String path(const char* key) const { return directory_ + "/" + key; }
  String directory_;
  bool readOnly_ = false;
};

// Moves these namespaces from NVS to files: each one is copied whole, then erased from NVS.
// Runs at start-up before the modules load; namespaces already moved are skipped.
void migrateConfigFromNvs(const char* const* spaces, size_t count);

}  // namespace whisli::system
