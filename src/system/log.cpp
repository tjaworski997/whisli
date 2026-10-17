#include "whisli/system/log.hpp"

#include <cstring>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"

namespace whisli::system {
namespace {

LogEntry fallbackEntries[kFallbackLogCapacity] = {};
LogEntry* entries = fallbackEntries;
uint16_t logCapacity = kFallbackLogCapacity;
uint16_t firstEntry = 0;
uint16_t entryCount = 0;
uint32_t nextId = 1;
uint32_t currentSessionId = 0;
portMUX_TYPE logMux = portMUX_INITIALIZER_UNLOCKED;

void pruneExpired(uint32_t now) {
  while (entryCount > 0) {
    const LogEntry& oldest = entries[firstEntry];
    if (now - oldest.uptimeMs <= kLogRetentionMs) break;
    firstEntry = static_cast<uint16_t>((firstEntry + 1) % logCapacity);
    --entryCount;
  }
}

void copyText(char* destination, size_t capacity, const char* source) {
  if (capacity == 0) return;
  if (!source) source = "";
  strlcpy(destination, source, capacity);
}

}  // namespace

const char* levelName(LogLevel level) {
  switch (level) {
    case LogLevel::Info: return "info";
    case LogLevel::Warning: return "warning";
    case LogLevel::Error: return "error";
  }
  return "info";
}

void write(LogLevel level, const char* category, const char* message) {
  const uint32_t now = millis();
  portENTER_CRITICAL(&logMux);
  pruneExpired(now);
  uint16_t position;
  if (entryCount < logCapacity) {
    position = static_cast<uint16_t>((firstEntry + entryCount) % logCapacity);
    ++entryCount;
  } else {
    position = firstEntry;
    firstEntry = static_cast<uint16_t>((firstEntry + 1) % logCapacity);
  }
  LogEntry& entry = entries[position];
  entry.id = nextId++;
  if (nextId == 0) nextId = 1;
  entry.uptimeMs = now;
  entry.level = level;
  copyText(entry.category, sizeof(entry.category), category);
  copyText(entry.message, sizeof(entry.message), message);
  portEXIT_CRITICAL(&logMux);

  const char* tag = "WHISLI";
  const char* categoryText = category ? category : "system";
  const char* messageText = message ? message : "";
  if (level == LogLevel::Error) ESP_LOGE(tag, "[%s] %s", categoryText, messageText);
  else if (level == LogLevel::Warning) ESP_LOGW(tag, "[%s] %s", categoryText, messageText);
  else ESP_LOGI(tag, "[%s] %s", categoryText, messageText);
}

uint16_t count() {
  const uint32_t now = millis();
  portENTER_CRITICAL(&logMux);
  pruneExpired(now);
  const uint16_t result = entryCount;
  portEXIT_CRITICAL(&logMux);
  return result;
}

uint16_t capacity() { return logCapacity; }
uint32_t sessionId() { return currentSessionId; }

bool get(uint16_t index, LogEntry& output) {
  const uint32_t now = millis();
  portENTER_CRITICAL(&logMux);
  pruneExpired(now);
  if (index >= entryCount) {
    portEXIT_CRITICAL(&logMux);
    return false;
  }
  output = entries[(firstEntry + index) % logCapacity];
  portEXIT_CRITICAL(&logMux);
  return true;
}

void begin() {
  currentSessionId = esp_random();
  if (currentSessionId == 0) currentSessionId = 1;
  auto* externalEntries = static_cast<LogEntry*>(heap_caps_calloc(
      kPreferredLogCapacity, sizeof(LogEntry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (externalEntries) {
    portENTER_CRITICAL(&logMux);
    entries = externalEntries;
    logCapacity = kPreferredLogCapacity;
    firstEntry = 0;
    entryCount = 0;
    nextId = 1;
    portEXIT_CRITICAL(&logMux);
  }

  char memoryMessage[80];
  const bool hasPsram = psramFound();
  snprintf(memoryMessage, sizeof(memoryMessage), "PSRAM %s: %lu MiB available", hasPsram ? "detected" : "not detected",
           static_cast<unsigned long>(ESP.getPsramSize() / (1024 * 1024)));
  write(hasPsram ? LogLevel::Info : LogLevel::Warning, "system", memoryMessage);

  esp_reset_reason_t reason = esp_reset_reason();
  const char* description = "unknown reset reason";
  switch (reason) {
    case ESP_RST_POWERON: description = "power on"; break;
    case ESP_RST_EXT: description = "external reset"; break;
    case ESP_RST_SW: description = "software restart"; break;
    case ESP_RST_PANIC: description = "panic/reset after crash"; break;
    case ESP_RST_INT_WDT: description = "interrupt watchdog reset"; break;
    case ESP_RST_TASK_WDT: description = "task watchdog reset"; break;
    case ESP_RST_WDT: description = "watchdog reset"; break;
    case ESP_RST_DEEPSLEEP: description = "wake from deep sleep"; break;
    case ESP_RST_BROWNOUT: description = "brownout reset"; break;
    case ESP_RST_SDIO: description = "SDIO reset"; break;
    default: break;
  }
  if (!externalEntries) write(LogLevel::Warning, "system", "4096-entry PSRAM log buffer unavailable; using small internal buffer");
  write(LogLevel::Info, "system", description);
}

}  // namespace whisli::system
