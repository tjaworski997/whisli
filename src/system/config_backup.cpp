#include "whisli/system/config_backup.hpp"

#include <esp_random.h>
#include <mbedtls/base64.h>
#include <mbedtls/gcm.h>
#include <mbedtls/pkcs5.h>
#include <nvs.h>

#include <ctime>
#include <vector>

#include "whisli/net/json.hpp"
#include "whisli/version.generated.hpp"

namespace whisli::system::backup {
namespace {

constexpr int kFormat = 1;
constexpr const char* kNamespaces[] = {"whisli-board", "whisli-relay", "whisli-lights", "whisli-actions", "whisli-zones", "whisli-presets"};

bool backedUp(const String& name) {
  for (const char* candidate : kNamespaces) if (name == candidate) return true;
  return false;
}

// Network and MQTT settings: only these keys travel, and they are merged into what the controller
// has instead of replacing it, so a backup without passwords never wipes the ones in use. Secret
// keys travel only in the encrypted part; the Wi-Fi name goes with its password, since one is
// useless without the other.
struct MergedKey { const char* space; const char* key; bool secret; };
constexpr MergedKey kMergedKeys[] = {
  {"whisli-net", "apOn", false}, {"whisli-net", "ssid", true}, {"whisli-net", "password", true}, {"whisli-net", "apPass", true},
  {"whisli-mqtt", "enabled", false}, {"whisli-mqtt", "host", false}, {"whisli-mqtt", "port", false}, {"whisli-mqtt", "user", false},
  {"whisli-mqtt", "topic", false}, {"whisli-mqtt", "disc", false}, {"whisli-mqtt", "discPrefix", false}, {"whisli-mqtt", "pass", true},
};
constexpr const char* kMergedNamespaces[] = {"whisli-net", "whisli-mqtt"};

const MergedKey* mergedKey(const String& space, const String& key) {
  for (const auto& entry : kMergedKeys) if (space == entry.space && key == entry.key) return &entry;
  return nullptr;
}

// Encryption of the secret part: AES-256-GCM, the key derived from the backup password.
constexpr uint32_t kKdfIterations = 10000;

struct TypeName { nvs_type_t type; const char* name; };
constexpr TypeName kTypes[] = {
  {NVS_TYPE_U8, "u8"}, {NVS_TYPE_I8, "i8"}, {NVS_TYPE_U16, "u16"}, {NVS_TYPE_I16, "i16"},
  {NVS_TYPE_U32, "u32"}, {NVS_TYPE_I32, "i32"}, {NVS_TYPE_U64, "u64"}, {NVS_TYPE_I64, "i64"},
  {NVS_TYPE_STR, "str"}, {NVS_TYPE_BLOB, "blob"},
};

const char* typeName(nvs_type_t type) {
  for (const auto& entry : kTypes) if (entry.type == type) return entry.name;
  return nullptr;
}

bool typeFromName(const String& name, nvs_type_t& type) {
  for (const auto& entry : kTypes) if (name == entry.name) { type = entry.type; return true; }
  return false;
}

String base64(const uint8_t* data, size_t length) {
  size_t needed = 0;
  mbedtls_base64_encode(nullptr, 0, &needed, data, length);
  std::vector<unsigned char> output(needed + 1);
  if (mbedtls_base64_encode(output.data(), output.size(), &needed, data, length) != 0) return String();
  return String(reinterpret_cast<const char*>(output.data()));
}

bool fromBase64(const String& text, std::vector<uint8_t>& output) {
  size_t needed = 0;
  const auto* input = reinterpret_cast<const unsigned char*>(text.c_str());
  mbedtls_base64_decode(nullptr, 0, &needed, input, text.length());
  output.resize(needed);
  return mbedtls_base64_decode(output.data(), output.size(), &needed, input, text.length()) == 0 && (output.resize(needed), true);
}

// The value of one entry as a string, or false when it cannot be read.
bool readValue(nvs_handle_t handle, const char* key, nvs_type_t type, String& output) {
  switch (type) {
    case NVS_TYPE_U8: { uint8_t value; if (nvs_get_u8(handle, key, &value) != ESP_OK) return false; output = String(value); return true; }
    case NVS_TYPE_I8: { int8_t value; if (nvs_get_i8(handle, key, &value) != ESP_OK) return false; output = String(value); return true; }
    case NVS_TYPE_U16: { uint16_t value; if (nvs_get_u16(handle, key, &value) != ESP_OK) return false; output = String(value); return true; }
    case NVS_TYPE_I16: { int16_t value; if (nvs_get_i16(handle, key, &value) != ESP_OK) return false; output = String(value); return true; }
    case NVS_TYPE_U32: { uint32_t value; if (nvs_get_u32(handle, key, &value) != ESP_OK) return false; output = String(value); return true; }
    case NVS_TYPE_I32: { int32_t value; if (nvs_get_i32(handle, key, &value) != ESP_OK) return false; output = String(value); return true; }
    case NVS_TYPE_U64: { uint64_t value; if (nvs_get_u64(handle, key, &value) != ESP_OK) return false; char text[24]; snprintf(text, sizeof(text), "%llu", static_cast<unsigned long long>(value)); output = text; return true; }
    case NVS_TYPE_I64: { int64_t value; if (nvs_get_i64(handle, key, &value) != ESP_OK) return false; char text[24]; snprintf(text, sizeof(text), "%lld", static_cast<long long>(value)); output = text; return true; }
    case NVS_TYPE_STR: {
      size_t length = 0;
      if (nvs_get_str(handle, key, nullptr, &length) != ESP_OK) return false;
      std::vector<char> text(length);
      if (nvs_get_str(handle, key, text.data(), &length) != ESP_OK) return false;
      output = String(text.data());
      return true;
    }
    case NVS_TYPE_BLOB: {
      size_t length = 0;
      if (nvs_get_blob(handle, key, nullptr, &length) != ESP_OK) return false;
      std::vector<uint8_t> data(length);
      if (length && nvs_get_blob(handle, key, data.data(), &length) != ESP_OK) return false;
      output = base64(data.data(), length);
      return true;
    }
    default: return false;
  }
}

// --- Reading the backup file: a small JSON reader for exactly this format. ---

class Reader {
 public:
  Reader(const char* text, size_t length) : position_(text), end_(text + length) {}

  void space() { while (position_ < end_ && (*position_ == ' ' || *position_ == '\n' || *position_ == '\r' || *position_ == '\t')) ++position_; }
  bool take(char character) { space(); if (position_ < end_ && *position_ == character) { ++position_; return true; } return false; }
  bool peek(char character) { space(); return position_ < end_ && *position_ == character; }

  bool string(String& output) {
    if (!take('"')) return false;
    output = String();
    while (position_ < end_ && *position_ != '"') {
      char character = *position_++;
      if (character != '\\') { output += character; continue; }
      if (position_ >= end_) return false;
      switch (*position_++) {
        case '"': output += '"'; break;
        case '\\': output += '\\'; break;
        case '/': output += '/'; break;
        case 'b': output += '\b'; break;
        case 'f': output += '\f'; break;
        case 'n': output += '\n'; break;
        case 'r': output += '\r'; break;
        case 't': output += '\t'; break;
        case 'u': {
          if (end_ - position_ < 4) return false;
          uint32_t code = strtoul(String(position_, 4).c_str(), nullptr, 16);
          position_ += 4;
          // UTF-8; surrogate pairs do not occur in this format.
          if (code < 0x80) output += static_cast<char>(code);
          else if (code < 0x800) { output += static_cast<char>(0xc0 | code >> 6); output += static_cast<char>(0x80 | (code & 0x3f)); }
          else { output += static_cast<char>(0xe0 | code >> 12); output += static_cast<char>(0x80 | (code >> 6 & 0x3f)); output += static_cast<char>(0x80 | (code & 0x3f)); }
          break;
        }
        default: return false;
      }
    }
    return take('"');
  }

  bool number(long& output) {
    space();
    char* after = nullptr;
    output = strtol(position_, &after, 10);
    if (after == position_ || after > end_) return false;
    position_ = after;
    return true;
  }

  // Skips any value (for fields this firmware does not need).
  bool skip(uint8_t depth = 0) {
    if (depth > 16) return false;
    space();
    if (position_ >= end_) return false;
    String ignored;
    if (*position_ == '"') return string(ignored);
    if (*position_ == '{' || *position_ == '[') {
      const char close = *position_ == '{' ? '}' : ']';
      ++position_;
      if (take(close)) return true;
      do {
        if (close == '}' && !(string(ignored) && take(':'))) return false;
        if (!skip(depth + 1)) return false;
      } while (take(','));
      return take(close);
    }
    while (position_ < end_ && *position_ != ',' && *position_ != '}' && *position_ != ']') ++position_;
    return true;
  }

 private:
  const char* position_;
  const char* end_;
};

struct Entry {
  String space;
  String key;
  nvs_type_t type;
  String text;                // numbers and strings
  std::vector<uint8_t> blob;  // blobs, decoded
};

bool parseEntry(Reader& reader, Entry& entry, bool secretPart = false) {
  String type;
  if (!(reader.take('[') && reader.string(entry.space) && reader.take(',') && reader.string(entry.key) && reader.take(',') &&
        reader.string(type) && reader.take(',') && reader.string(entry.text) && reader.take(']'))) return false;
  const MergedKey* merged = mergedKey(entry.space, entry.key);
  const bool allowed = secretPart ? merged && merged->secret : backedUp(entry.space) || (merged && !merged->secret);
  if (!allowed || entry.key.isEmpty() || entry.key.length() > NVS_KEY_NAME_MAX_SIZE - 1) return false;
  if (!typeFromName(type, entry.type)) return false;
  if (entry.type == NVS_TYPE_BLOB) { if (!fromBase64(entry.text, entry.blob)) return false; entry.text = String(); }
  return true;
}

esp_err_t writeEntry(nvs_handle_t handle, const Entry& entry) {
  const char* key = entry.key.c_str();
  const char* text = entry.text.c_str();
  switch (entry.type) {
    case NVS_TYPE_U8: return nvs_set_u8(handle, key, static_cast<uint8_t>(strtoul(text, nullptr, 10)));
    case NVS_TYPE_I8: return nvs_set_i8(handle, key, static_cast<int8_t>(strtol(text, nullptr, 10)));
    case NVS_TYPE_U16: return nvs_set_u16(handle, key, static_cast<uint16_t>(strtoul(text, nullptr, 10)));
    case NVS_TYPE_I16: return nvs_set_i16(handle, key, static_cast<int16_t>(strtol(text, nullptr, 10)));
    case NVS_TYPE_U32: return nvs_set_u32(handle, key, static_cast<uint32_t>(strtoul(text, nullptr, 10)));
    case NVS_TYPE_I32: return nvs_set_i32(handle, key, static_cast<int32_t>(strtol(text, nullptr, 10)));
    case NVS_TYPE_U64: return nvs_set_u64(handle, key, strtoull(text, nullptr, 10));
    case NVS_TYPE_I64: return nvs_set_i64(handle, key, strtoll(text, nullptr, 10));
    case NVS_TYPE_STR: return nvs_set_str(handle, key, text);
    case NVS_TYPE_BLOB: return nvs_set_blob(handle, key, entry.blob.data(), entry.blob.size());
    default: return ESP_ERR_INVALID_ARG;
  }
}

// Appends the entries of `space` as JSON rows. which: 0 every key, 1 the plain merged keys, 2 the secret ones.
void appendEntries(String& body, bool& first, const char* space, int which) {
  nvs_handle_t handle;
  if (nvs_open(space, NVS_READONLY, &handle) != ESP_OK) return;  // nothing stored yet
  nvs_iterator_t iterator = nullptr;
  esp_err_t result = nvs_entry_find(NVS_DEFAULT_PART_NAME, space, NVS_TYPE_ANY, &iterator);
  while (result == ESP_OK) {
    nvs_entry_info_t info;
    nvs_entry_info(iterator, &info);
    const MergedKey* merged = mergedKey(space, info.key);
    const bool wanted = which == 0 || (merged && merged->secret == (which == 2));
    String value;
    const char* type = typeName(info.type);
    if (wanted && type && readValue(handle, info.key, info.type, value)) {
      body += first ? "\n" : ",\n";
      body += "[\"" + String(space) + "\",\"" + whisli::net::jsonEscape(info.key) + "\",\"" + type + "\",\"" + whisli::net::jsonEscape(value) + "\"]";
      first = false;
    }
    result = nvs_entry_next(&iterator);
  }
  nvs_release_iterator(iterator);
  nvs_close(handle);
}

bool deriveKey(const String& passphrase, const uint8_t* salt, size_t saltLength, uint32_t iterations, uint8_t key[32]) {
  return mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, reinterpret_cast<const unsigned char*>(passphrase.c_str()), passphrase.length(),
                                       salt, saltLength, iterations, 32, key) == 0;
}

// {"cipher":"aes-256-gcm","kdf":"pbkdf2-sha256","iterations":…,"salt":…,"iv":…,"data":…}; data ends with the 16-byte tag.
String encryptSecrets(const String& plaintext, const String& passphrase) {
  uint8_t salt[16], iv[12], key[32];
  esp_fill_random(salt, sizeof(salt));
  esp_fill_random(iv, sizeof(iv));
  if (!deriveKey(passphrase, salt, sizeof(salt), kKdfIterations, key)) return String();
  std::vector<uint8_t> data(plaintext.length() + 16);
  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  const bool ok = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256) == 0 &&
                  mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, plaintext.length(), iv, sizeof(iv), nullptr, 0,
                                            reinterpret_cast<const unsigned char*>(plaintext.c_str()), data.data(), 16, data.data() + plaintext.length()) == 0;
  mbedtls_gcm_free(&gcm);
  memset(key, 0, sizeof(key));
  if (!ok) return String();
  return "{\"cipher\":\"aes-256-gcm\",\"kdf\":\"pbkdf2-sha256\",\"iterations\":" + String(kKdfIterations) + ",\"salt\":\"" +
         base64(salt, sizeof(salt)) + "\",\"iv\":\"" + base64(iv, sizeof(iv)) + "\",\"data\":\"" + base64(data.data(), data.size()) + "\"}";
}

// False when the password is wrong or the data was changed (the tag does not match).
bool decryptSecrets(const String& salt64, const String& iv64, const String& data64, long iterations, const String& passphrase, String& plaintext) {
  std::vector<uint8_t> salt, iv, data;
  if (!fromBase64(salt64, salt) || !fromBase64(iv64, iv) || !fromBase64(data64, data) || data.size() < 16 || iterations < 1000 || iterations > 1000000) return false;
  uint8_t key[32];
  if (!deriveKey(passphrase, salt.data(), salt.size(), static_cast<uint32_t>(iterations), key)) return false;
  const size_t length = data.size() - 16;
  std::vector<char> output(length + 1, 0);
  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  const bool ok = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256) == 0 &&
                  mbedtls_gcm_auth_decrypt(&gcm, length, iv.data(), iv.size(), nullptr, 0, data.data() + length, 16, data.data(),
                                           reinterpret_cast<unsigned char*>(output.data())) == 0;
  mbedtls_gcm_free(&gcm);
  memset(key, 0, sizeof(key));
  if (ok) plaintext = String(output.data());
  return ok;
}

bool parseEntries(Reader& reader, std::vector<Entry>& entries, bool secretPart) {
  if (!reader.take('[')) return false;
  if (reader.take(']')) return true;
  do {
    Entry entry;
    if (!parseEntry(reader, entry, secretPart)) return false;
    entries.push_back(std::move(entry));
  } while (reader.take(','));
  return reader.take(']');
}

}  // namespace

String exportJson(const String& passphrase) {
  const time_t now = time(nullptr);
  String body = "{\"application\":\"Whisli\",\"format\":" + String(kFormat) + ",\"firmware\":\"" + String(kFirmwareVersion) +
                "\",\"created\":" + String(now > 1700000000 ? static_cast<long>(now) : 0) + ",\"entries\":[";
  bool first = true;
  for (const char* space : kNamespaces) appendEntries(body, first, space, 0);
  for (const char* space : kMergedNamespaces) appendEntries(body, first, space, 1);
  body += "\n]";
  if (!passphrase.isEmpty()) {
    String secret = "[";
    bool firstSecret = true;
    for (const char* space : kMergedNamespaces) appendEntries(secret, firstSecret, space, 2);
    const String encrypted = encryptSecrets(secret + "]", passphrase);
    if (encrypted.isEmpty()) return String();
    body += ",\"secrets\":" + encrypted;
  }
  return body + "}\n";
}

ImportResult import(const char* json, size_t length, const String& passphrase) {
  ImportResult result;
  const auto fail = [&](const char* message) { result.error = message; return result; };
  Reader reader(json, length);
  String application;
  long format = 0;
  bool hasEntries = false;
  std::vector<Entry> entries;
  String salt, iv, data;
  long iterations = 0;
  if (!reader.take('{')) return fail("To nie jest plik kopii konfiguracji Whisli.");
  if (!reader.peek('}')) {
    do {
      String field;
      if (!(reader.string(field) && reader.take(':'))) return fail("Plik jest uszkodzony.");
      if (field == "application") {
        if (!reader.string(application)) return fail("Plik jest uszkodzony.");
      } else if (field == "format") {
        if (!reader.number(format)) return fail("Plik jest uszkodzony.");
      } else if (field == "entries") {
        hasEntries = true;
        if (!parseEntries(reader, entries, false)) return fail("Plik zawiera nieprawidłowy wpis.");
      } else if (field == "secrets") {
        result.secretsPresent = true;
        if (!reader.take('{')) return fail("Plik jest uszkodzony.");
        do {
          String key, text;
          if (!(reader.string(key) && reader.take(':'))) return fail("Plik jest uszkodzony.");
          if (key == "iterations") { if (!reader.number(iterations)) return fail("Plik jest uszkodzony."); }
          else if (key == "salt") { if (!reader.string(salt)) return fail("Plik jest uszkodzony."); }
          else if (key == "iv") { if (!reader.string(iv)) return fail("Plik jest uszkodzony."); }
          else if (key == "data") { if (!reader.string(data)) return fail("Plik jest uszkodzony."); }
          else if (!reader.skip()) return fail("Plik jest uszkodzony.");
        } while (reader.take(','));
        if (!reader.take('}')) return fail("Plik jest uszkodzony.");
      } else if (!reader.skip()) {
        return fail("Plik jest uszkodzony.");
      }
    } while (reader.take(','));
  }
  if (!reader.take('}')) return fail("Plik jest uszkodzony.");
  if (application != "Whisli" || !hasEntries) return fail("To nie jest plik kopii konfiguracji Whisli.");
  if (format != kFormat) return fail("Ta wersja pliku kopii nie jest obsługiwana.");

  // The secret part needs the backup password; without it (or with a wrong one) the rest still loads.
  if (result.secretsPresent && !passphrase.isEmpty()) {
    String plaintext;
    if (decryptSecrets(salt, iv, data, iterations, passphrase, plaintext)) {
      Reader secretReader(plaintext.c_str(), plaintext.length());
      if (!parseEntries(secretReader, entries, true)) return fail("Zaszyfrowana część pliku jest uszkodzona.");
      result.secretsApplied = true;
    } else {
      result.wrongPassphrase = true;
    }
  }

  // Replace each namespace as a whole, so nothing from the old configuration lingers.
  for (const char* space : kNamespaces) {
    nvs_handle_t handle;
    if (nvs_open(space, NVS_READWRITE, &handle) != ESP_OK) return fail("Nie udało się zapisać konfiguracji.");
    esp_err_t written = nvs_erase_all(handle);
    for (const Entry& entry : entries) {
      if (written != ESP_OK) break;
      if (entry.space == space) written = writeEntry(handle, entry);
    }
    if (written == ESP_OK) written = nvs_commit(handle);
    nvs_close(handle);
    if (written != ESP_OK) return fail("Nie udało się zapisać konfiguracji.");
  }
  // Network and MQTT: only what the file has is written; everything else stays.
  for (const char* space : kMergedNamespaces) {
    nvs_handle_t handle;
    if (nvs_open(space, NVS_READWRITE, &handle) != ESP_OK) return fail("Nie udało się zapisać ustawień sieci.");
    esp_err_t written = ESP_OK;
    for (const Entry& entry : entries) {
      if (written != ESP_OK) break;
      if (entry.space == space) written = writeEntry(handle, entry);
    }
    if (written == ESP_OK) written = nvs_commit(handle);
    nvs_close(handle);
    if (written != ESP_OK) return fail("Nie udało się zapisać ustawień sieci.");
  }
  return result;
}

}  // namespace whisli::system::backup
