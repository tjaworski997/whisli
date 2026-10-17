#pragma once

#include <Arduino.h>
#include <cstring>

namespace whisli::app {

struct DeviceClassDefinition {
  const char* id;
  const char* nameKey;
  const char* descriptionKey;
  const char* icon;
  bool available;
};

struct DeviceProfileDefinition {
  const char* id;
  const char* deviceClass;
  const char* nameKey;
  const char* descriptionKey;
  const char* defaultNameKey;
  const char* communication;
  const char* searchKeywords;
  uint8_t defaultChannelCount;
  uint16_t defaultCoilStart;
  uint8_t minimumChannelCount;
  uint8_t maximumChannelCount;
  bool editableChannelCount;
  bool editableCoilStart;
  bool available;
};

#include "device_profiles.generated.hpp"

inline const DeviceProfileDefinition* findDeviceProfile(const char* id) {
  for (const auto& profile : kDeviceProfiles) {
    if (std::strcmp(profile.id, id) == 0) return &profile;
  }
  return nullptr;
}

}  // namespace whisli::app
