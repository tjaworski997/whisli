#pragma once

#include <Arduino.h>

namespace whisli::system {

enum class LogLevel : uint8_t { Info, Warning, Error };

struct LogEntry {
  uint32_t id = 0;
  uint32_t uptimeMs = 0;
  LogLevel level = LogLevel::Info;
  char category[12] = {};
  char message[88] = {};
};

constexpr uint16_t kPreferredLogCapacity = 4096;
constexpr uint16_t kFallbackLogCapacity = 192;
constexpr uint16_t kLogPageSize = 128;
constexpr uint32_t kLogRetentionMs = 30UL * 60UL * 1000UL;

// Bufor alokowany raz przy starcie; dopisywanie nie alokuje pamięci ani nie zapisuje do flasha.
void write(LogLevel level, const char* category, const char* message);
uint16_t count();
uint16_t capacity();
uint32_t sessionId();
bool get(uint16_t index, LogEntry& output);
const char* levelName(LogLevel level);
void begin();

}  // namespace whisli::system
