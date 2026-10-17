#pragma once

#include <Arduino.h>
#include <HardwareSerial.h>
#include "whisli/app/device_profiles.hpp"

namespace whisli::app {

constexpr uint8_t kMaxRelayDevices = 16;
constexpr uint8_t kMaxRelayChannels = 32;

struct RelaySnapshot {
  bool configured = false;
  bool active = true;
  bool online = false;
  uint8_t id = 0;
  uint8_t address = 1;
  uint8_t channelCount = 0;
  uint16_t coilStart = 0;
  char profile[24] = "";
  char name[33] = "";
  bool channels[kMaxRelayChannels] = {};
  bool channelUsed[kMaxRelayChannels] = {};
  char channelNames[kMaxRelayChannels][33] = {};
};

// Menedżer modułów przekaźnikowych na jednej magistrali Modbus RTU.
// Zadanie FreeRTOS i kolejka izolują komunikację od HTTP i pętli automatyki.
class ModbusRelayManager {
 public:
  void begin();
  bool add(const DeviceProfileDefinition& profile, const String& name, uint8_t address, uint8_t channelCount, uint16_t coilStart);
  bool update(uint8_t id, const String& name, uint8_t address, uint8_t channelCount, uint16_t coilStart);
  bool remove(uint8_t id);
  bool setActive(uint8_t id, bool active);
  bool setChannelUsed(uint8_t id, uint8_t channel, bool used);
  bool setChannel(uint8_t id, uint8_t channel, bool on);
  bool setAllChannels(uint8_t id, bool on);
  bool setChannelName(uint8_t id, uint8_t channel, const String& name);
  RelaySnapshot snapshot(uint8_t id);
  uint8_t deviceIds(uint8_t* output, uint8_t capacity);

 private:
  enum class CommandType : uint8_t { Read, Set, SetAll };
  struct Command { CommandType type; uint8_t id; uint8_t channel; bool on; };

  static void taskEntry(void* context);
  void task();
  bool readChannels(const RelaySnapshot& device);
  bool writeChannel(const RelaySnapshot& device, uint8_t channel, bool on);
  bool request(const uint8_t* frame, size_t frameSize, uint8_t* response, size_t responseSize);
  void setOnline(uint8_t id, bool online);
  bool saveDevice(uint8_t slot);
  int slotForId(uint8_t id);

  HardwareSerial serial_{1};
  QueueHandle_t commands_ = nullptr;
  SemaphoreHandle_t stateLock_ = nullptr;
  // ~20 KB: allocated, so it lands in PSRAM; reads like a plain array.
  RelaySnapshot (&devices_)[kMaxRelayDevices] = *new RelaySnapshot[1][kMaxRelayDevices]();
};

}  // namespace whisli::app
