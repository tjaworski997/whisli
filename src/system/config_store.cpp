#include "whisli/system/config_store.hpp"

#include <LittleFS.h>

#include <cstring>

#include "whisli/system/log.hpp"

namespace whisli::system {
namespace {

constexpr char kPartition[] = "spiffs";
constexpr char kRoot[] = "/cfg";

size_t integerSize(nvs_type_t type) {
  switch (type) {
    case NVS_TYPE_U8: case NVS_TYPE_I8: return 1;
    case NVS_TYPE_U16: case NVS_TYPE_I16: return 2;
    case NVS_TYPE_U32: case NVS_TYPE_I32: return 4;
    case NVS_TYPE_U64: case NVS_TYPE_I64: return 8;
    default: return 0;
  }
}

// One NVS entry as the bytes a file holds: integers little-endian, strings without their NUL.
bool readNvs(nvs_handle_t handle, const char* key, nvs_type_t type, std::vector<uint8_t>& data) {
  if (const size_t size = integerSize(type)) {
    uint64_t value = 0;
    esp_err_t result = ESP_FAIL;
    switch (type) {
      case NVS_TYPE_U8: result = nvs_get_u8(handle, key, reinterpret_cast<uint8_t*>(&value)); break;
      case NVS_TYPE_I8: result = nvs_get_i8(handle, key, reinterpret_cast<int8_t*>(&value)); break;
      case NVS_TYPE_U16: result = nvs_get_u16(handle, key, reinterpret_cast<uint16_t*>(&value)); break;
      case NVS_TYPE_I16: result = nvs_get_i16(handle, key, reinterpret_cast<int16_t*>(&value)); break;
      case NVS_TYPE_U32: result = nvs_get_u32(handle, key, reinterpret_cast<uint32_t*>(&value)); break;
      case NVS_TYPE_I32: result = nvs_get_i32(handle, key, reinterpret_cast<int32_t*>(&value)); break;
      case NVS_TYPE_U64: result = nvs_get_u64(handle, key, &value); break;
      case NVS_TYPE_I64: result = nvs_get_i64(handle, key, reinterpret_cast<int64_t*>(&value)); break;
      default: break;
    }
    data.assign(reinterpret_cast<uint8_t*>(&value), reinterpret_cast<uint8_t*>(&value) + size);
    return result == ESP_OK;
  }
  size_t length = 0;
  if (type == NVS_TYPE_STR) {
    if (nvs_get_str(handle, key, nullptr, &length) != ESP_OK) return false;
    data.resize(length);
    if (nvs_get_str(handle, key, reinterpret_cast<char*>(data.data()), &length) != ESP_OK) return false;
    if (!data.empty()) data.pop_back();  // the NUL
    return true;
  }
  if (type == NVS_TYPE_BLOB) {
    if (nvs_get_blob(handle, key, nullptr, &length) != ESP_OK) return false;
    data.resize(length);
    return length == 0 || nvs_get_blob(handle, key, data.data(), &length) == ESP_OK;
  }
  return false;
}

bool writeFile(const String& path, nvs_type_t type, const void* data, size_t length) {
  const String temporary = path + ".new";
  File file = LittleFS.open(temporary, "w");
  if (!file) return false;
  const uint8_t typeByte = static_cast<uint8_t>(type);
  bool ok = file.write(&typeByte, 1) == 1 && (length == 0 || file.write(static_cast<const uint8_t*>(data), length) == length);
  file.close();
  ok = ok && LittleFS.rename(temporary, path);  // replaces the old file atomically
  if (!ok) LittleFS.remove(temporary);
  return ok;
}

}  // namespace

bool mountFiles() {
  static int8_t state = -1;  // -1 not tried, 0 failed, 1 mounted
  if (state < 0) {
    state = LittleFS.begin(true, "/littlefs", 4, kPartition) ? 1 : 0;
    if (!state) write(LogLevel::Error, "system", "File storage (LittleFS) could not be mounted");
    else if (!LittleFS.exists(kRoot)) LittleFS.mkdir(kRoot);
  }
  return state == 1;
}

bool ConfigStore::begin(const char* space, bool readOnly) {
  directory_ = String(kRoot) + "/" + space;
  readOnly_ = readOnly;
  if (!mountFiles()) return false;
  if (!readOnly && !LittleFS.exists(directory_)) LittleFS.mkdir(directory_);
  return true;
}

bool ConfigStore::read(const char* key, nvs_type_t& type, std::vector<uint8_t>& data) {
  if (!mountFiles()) return false;
  File file = LittleFS.open(path(key), "r");
  if (!file) return false;
  const size_t size = file.size();
  if (size < 1) { file.close(); return false; }
  uint8_t typeByte = 0;
  file.read(&typeByte, 1);
  data.resize(size - 1);
  const bool ok = size == 1 || file.read(data.data(), size - 1) == size - 1;
  file.close();
  type = static_cast<nvs_type_t>(typeByte);
  return ok;
}

bool ConfigStore::write(const char* key, nvs_type_t type, const void* data, size_t length) {
  if (readOnly_ || !mountFiles()) return false;
  // Unchanged values are not rewritten: flash wears with every write.
  nvs_type_t storedType;
  std::vector<uint8_t> stored;
  if (read(key, storedType, stored) && storedType == type && stored.size() == length && (length == 0 || memcmp(stored.data(), data, length) == 0)) return true;
  return writeFile(path(key), type, data, length);
}

bool ConfigStore::isKey(const char* key) { return mountFiles() && LittleFS.exists(path(key)); }

bool ConfigStore::remove(const char* key) { return !readOnly_ && isKey(key) && LittleFS.remove(path(key)); }

bool ConfigStore::clear() {
  if (readOnly_ || !mountFiles()) return false;
  std::vector<String> keys;
  forEach([&](const String& key, nvs_type_t, const std::vector<uint8_t>&) { keys.push_back(key); });
  bool ok = true;
  for (const String& key : keys) ok = LittleFS.remove(path(key.c_str())) && ok;
  return ok;
}

void ConfigStore::forEach(const std::function<void(const String&, nvs_type_t, const std::vector<uint8_t>&)>& visit) {
  if (!mountFiles()) return;
  File directory = LittleFS.open(directory_);
  if (!directory || !directory.isDirectory()) return;
  std::vector<String> keys;
  for (File file = directory.openNextFile(); file; file = directory.openNextFile()) {
    String name = file.name();
    const int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    file.close();
    if (!name.endsWith(".new")) keys.push_back(name);
  }
  directory.close();
  for (const String& key : keys) {
    nvs_type_t type;
    std::vector<uint8_t> data;
    if (read(key.c_str(), type, data)) visit(key, type, data);
  }
}

size_t ConfigStore::getBytesLength(const char* key) {
  if (!mountFiles()) return 0;
  File file = LittleFS.open(path(key), "r");
  if (!file) return 0;
  const size_t size = file.size();
  file.close();
  return size > 0 ? size - 1 : 0;
}

size_t ConfigStore::getBytes(const char* key, void* buffer, size_t length) {
  nvs_type_t type;
  std::vector<uint8_t> data;
  if (!read(key, type, data) || data.size() > length) return 0;
  memcpy(buffer, data.data(), data.size());
  return data.size();
}

size_t ConfigStore::putBytes(const char* key, const void* data, size_t length) { return write(key, NVS_TYPE_BLOB, data, length) ? length : 0; }

uint8_t ConfigStore::getUChar(const char* key, uint8_t fallback) {
  nvs_type_t type;
  std::vector<uint8_t> data;
  return read(key, type, data) && data.size() >= 1 ? data[0] : fallback;
}

size_t ConfigStore::putUChar(const char* key, uint8_t value) { return write(key, NVS_TYPE_U8, &value, 1) ? 1 : 0; }

bool ConfigStore::getBool(const char* key, bool fallback) { return getUChar(key, fallback ? 1 : 0) != 0; }

size_t ConfigStore::putBool(const char* key, bool value) { return putUChar(key, value ? 1 : 0); }

String ConfigStore::getString(const char* key, const String& fallback) {
  nvs_type_t type;
  std::vector<uint8_t> data;
  if (!read(key, type, data) || type != NVS_TYPE_STR) return fallback;
  return String(reinterpret_cast<const char*>(data.data()), data.size());
}

size_t ConfigStore::putString(const char* key, const String& value) {
  return write(key, NVS_TYPE_STR, value.c_str(), value.length()) ? value.length() : 0;
}

void migrateConfigFromNvs(const char* const* spaces, size_t count) {
  if (!mountFiles()) return;
  for (size_t index = 0; index < count; ++index) {
    const char* space = spaces[index];
    const String directory = String(kRoot) + "/" + space;
    nvs_handle_t handle;
    const bool inNvs = nvs_open(space, NVS_READWRITE, &handle) == ESP_OK;
    if (!LittleFS.exists(directory)) {
      // Copied into a side directory and renamed once complete: a half-copied namespace never counts.
      const String staging = directory + ".new";
      ConfigStore store;
      store.begin((String(space) + ".new").c_str());
      store.clear();
      bool ok = true;
      size_t copied = 0;
      if (inNvs) {
        nvs_iterator_t iterator = nullptr;
        for (esp_err_t found = nvs_entry_find(NVS_DEFAULT_PART_NAME, space, NVS_TYPE_ANY, &iterator); found == ESP_OK; found = nvs_entry_next(&iterator)) {
          nvs_entry_info_t info;
          nvs_entry_info(iterator, &info);
          std::vector<uint8_t> data;
          ok = readNvs(handle, info.key, info.type, data) && store.write(info.key, info.type, data.data(), data.size()) && ok;
          ++copied;
        }
        nvs_release_iterator(iterator);
      }
      if (!ok || !LittleFS.rename(staging, directory)) {
        write(LogLevel::Error, "system", (String("Configuration could not be moved to files: ") + space).c_str());
        if (inNvs) nvs_close(handle);
        continue;  // NVS keeps it; the next start tries again
      }
      if (copied) write(LogLevel::Info, "system", (String("Configuration moved to files: ") + space + " (" + copied + " keys)").c_str());
    }
    // Moved (now or before an interrupted erase): free the settings storage.
    if (inNvs) {
      nvs_erase_all(handle);
      nvs_commit(handle);
      nvs_close(handle);
    }
  }
}

}  // namespace whisli::system
