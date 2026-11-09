#pragma once

#include <Arduino.h>
#include <HardwareSerial.h>
#include "whisli/app/device_profiles.hpp"

namespace whisli::app {

constexpr uint8_t kMaxRelayDevices = 16;
constexpr uint8_t kMaxRelayChannels = 32;

// A Modbus module: relay outputs or, for input modules (`inputs`), button inputs. Both share the
// stored layout: for an input module `channels` are the inputs as last read (true = pressed),
// `coilStart` is the first holding register with input bits, and used flags and names are per input.
struct RelaySnapshot {
  bool configured = false;
  bool active = true;
  bool online = false;
  bool inputs = false;  // from the profile's device class, not stored
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

// The part of an input module the control task needs every cycle: a few words, copied without
// touching the full device table.
struct InputModuleState {
  uint8_t id = 0;  // 0: no input module in this slot
  uint8_t channelCount = 0;
  bool online = false;
  uint32_t used = 0;
  uint32_t pressed = 0;
};

// Menedżer modułów Modbus RTU (przekaźniki i moduły wejść) na jednej magistrali.
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
  // Control task: input modules by slot (kMaxRelayDevices entries). Never waits on the bus.
  void inputStates(InputModuleState* output);

  static bool isInputProfile(const char* profile);

  // Diagnostics: one read request (functions 01–04 only) to any slave address, raw reply bytes
  // (including exception replies). Runs in the Modbus task between regular commands; the caller
  // waits, so call it only from a route without StateLock.
  struct ProbeResult {
    bool answered = false;
    uint8_t length = 0;
    uint8_t bytes[140] = {};
  };
  bool probe(uint8_t address, uint8_t function, uint16_t start, uint16_t count, ProbeResult& result);

 private:
  enum class CommandType : uint8_t { Read, Set, SetAll };
  struct Command { CommandType type; uint8_t id; uint8_t channel; bool on; };

  static void taskEntry(void* context);
  void task();
  bool readChannels(const RelaySnapshot& device);
  bool readInputs(const RelaySnapshot& device);
  bool writeChannel(const RelaySnapshot& device, uint8_t channel, bool on);
  // `timeoutMs` 0: the default for relay modules.
  bool request(const uint8_t* frame, size_t frameSize, uint8_t* response, size_t responseSize, uint32_t timeoutMs = 0);
  void keepFrameGap();
  void setOnline(uint8_t id, bool online);
  void publishInputState(uint8_t slot);  // stateLock_ held
  void runPendingProbe();
  bool saveDevice(uint8_t slot);
  int slotForId(uint8_t id);

  HardwareSerial serial_{1};
  int64_t lastFrameEndUs_ = 0;  // Modbus task only
  QueueHandle_t commands_ = nullptr;
  SemaphoreHandle_t stateLock_ = nullptr;
  SemaphoreHandle_t probeLock_ = nullptr;   // one probe at a time
  SemaphoreHandle_t probeDone_ = nullptr;   // given by the Modbus task when the probe finished
  volatile bool probePending_ = false;
  uint8_t probeFrame_[8] = {};
  // Filled by the Modbus task; the sequence tells the caller the result is for its own request,
  // not for an earlier one that timed out.
  ProbeResult probeStorage_{};
  volatile uint32_t probeRequested_ = 0;
  volatile uint32_t probeAnswered_ = 0;
  // Read by the control task under a spinlock (a copy of a few words), written with stateLock_ held.
  portMUX_TYPE inputMux_ = portMUX_INITIALIZER_UNLOCKED;
  InputModuleState inputStates_[kMaxRelayDevices] = {};
  // Consecutive failed input reads per slot: one lost reply among 40 a second is not "offline".
  uint8_t inputFailures_[kMaxRelayDevices] = {};
  // ~20 KB: allocated, so it lands in PSRAM; reads like a plain array.
  RelaySnapshot (&devices_)[kMaxRelayDevices] = *new RelaySnapshot[1][kMaxRelayDevices]();
};

}  // namespace whisli::app
