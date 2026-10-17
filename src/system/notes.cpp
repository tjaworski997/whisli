#include "whisli/system/notes.hpp"

#include <LittleFS.h>

#include <ctime>

#include "whisli/system/config_store.hpp"
#include "whisli/system/log.hpp"

namespace whisli::system::notes {
namespace {

constexpr char kTextPath[] = "/notes.txt";
constexpr char kTempPath[] = "/notes.tmp";
constexpr char kModifiedPath[] = "/notes.modified";

bool mounted() { return mountFiles(); }

bool writeFile(const char* path, const String& text) {
  File file = LittleFS.open(path, "w");
  if (!file) return false;
  const bool ok = file.print(text) == text.length();
  file.close();
  return ok;
}

String readFile(const char* path) {
  File file = LittleFS.open(path, "r");
  if (!file) return String();
  String text = file.readString();
  file.close();
  return text;
}

}  // namespace

Note load() {
  Note note;
  if (!mounted()) return note;
  note.text = readFile(kTextPath);
  note.modified = static_cast<uint32_t>(readFile(kModifiedPath).toInt());
  return note;
}

bool save(const String& text, uint32_t modified) {
  if (text.length() > kMaxBytes || !mounted()) return false;
  if (modified == 0) {
    const time_t now = time(nullptr);
    if (now > 1700000000) modified = static_cast<uint32_t>(now);  // clock set from the network
  }
  // Written aside and renamed over the old file: a power cut never leaves half a note.
  if (!writeFile(kTempPath, text)) { write(LogLevel::Error, "system", "Notes could not be saved"); return false; }
  LittleFS.remove(kTextPath);
  if (!LittleFS.rename(kTempPath, kTextPath)) { write(LogLevel::Error, "system", "Notes could not be saved"); return false; }
  writeFile(kModifiedPath, String(modified));
  return true;
}

}  // namespace whisli::system::notes
