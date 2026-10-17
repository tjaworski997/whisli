#pragma once

#include <Arduino.h>

namespace whisli::system::notes {

// One plain-text note for the installation (hardware, wiring, addresses). Kept as a file on the
// otherwise unused "spiffs" partition (LittleFS), not in the small settings storage (NVS), and
// travels in configuration backups. The panel limits it to 10 000 characters; the controller
// accepts up to kMaxBytes of UTF-8.
constexpr size_t kMaxCharacters = 10000;
constexpr size_t kMaxBytes = 4 * kMaxCharacters;

struct Note {
  String text;
  uint32_t modified = 0;  // Unix time of the last change, 0 when unknown
};

// The stored note; empty when there is none or the file system cannot be used.
Note load();
// Replaces the note. `modified` 0 takes the controller's clock. False when it could not be written.
bool save(const String& text, uint32_t modified = 0);

}  // namespace whisli::system::notes
