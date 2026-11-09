#include "whisli/app/modbus_relays.hpp"

#include "whisli/system/config_store.hpp"
#include <driver/uart.h>
#include <esp_timer.h>
#include <cstring>

#include "whisli/app/state_events.hpp"
#include "whisli/system/task_priorities.hpp"

namespace whisli::app {
namespace {

constexpr int kTxPin = 17;
constexpr int kRxPin = 18;
constexpr int kRtsPin = 21;
constexpr uint32_t kBaudRate = 9600;
constexpr uint32_t kResponseTimeoutMs = 250;
// Input modules are read this often: fast enough for gestures (the double click window is 300 ms),
// while one read (~18 ms at 9600 bps) leaves the bus free most of the time for relay commands.
constexpr uint32_t kInputPollIntervalMs = 25;
// An input module that stopped answering is retried rarely: each miss holds the bus for the timeout.
constexpr uint32_t kOfflineInputPollIntervalMs = 1000;
constexpr uint32_t kInputResponseTimeoutMs = 60;
constexpr uint8_t kInputFailuresUntilOffline = 4;
// Silence between frames. RTU asks for 3.5 characters (~4 ms at 9600 bps), but the Waveshare relay
// module missed about one request in five that came 5 ms after the input module's reply; 15 ms is
// reliable (tested: 20/20 for both modules with inputs read continuously).
constexpr uint32_t kInterFrameGapUs = 15000;

struct PersistedRelay {
  uint8_t version = 1;
  uint8_t id = 0;
  uint8_t address = 1;
  uint8_t channelCount = 0;
  uint8_t active = 1;
  uint16_t coilStart = 0;
  uint32_t channelUsed = 0;
  char profile[24] = "";
  char name[33] = "";
  char channelNames[kMaxRelayChannels][33] = {};
};

uint16_t crc16(const uint8_t* bytes, size_t length) {
  uint16_t crc = 0xFFFF;
  for (size_t index = 0; index < length; ++index) {
    crc ^= bytes[index];
    for (int bit = 0; bit < 8; ++bit) crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
  }
  return crc;
}

void addCrc(uint8_t* frame, size_t frameSize) {
  const uint16_t crc = crc16(frame, frameSize - 2);
  frame[frameSize - 2] = crc & 0xFF;
  frame[frameSize - 1] = crc >> 8;
}

}  // namespace

void ModbusRelayManager::begin() {
  stateLock_ = xSemaphoreCreateMutex();
  probeLock_ = xSemaphoreCreateMutex();
  probeDone_ = xSemaphoreCreateBinary();
  commands_ = xQueueCreate(16, sizeof(Command));
  whisli::system::ConfigStore preferences;
  preferences.begin("whisli-relay", false);

  // Migracja jednourządzeniowego prototypu do pierwszego slotu.
  const bool migrateLegacy = preferences.getBool("configured", false) && !preferences.getBool("migrated", false);
  if (migrateLegacy) {
    devices_[0].configured = true;
    devices_[0].id = 1;
    devices_[0].active = true;
    devices_[0].address = preferences.getUChar("address", 1);
    const DeviceProfileDefinition* legacyProfile = findDeviceProfile("waveshare_16ch");
    devices_[0].channelCount = legacyProfile ? legacyProfile->defaultChannelCount : 16;
    strlcpy(devices_[0].profile, "waveshare_16ch", sizeof(devices_[0].profile));
    preferences.getString("name", "Waveshare Relay 16CH #1").toCharArray(devices_[0].name, sizeof(devices_[0].name));
    for (uint8_t channel = 0; channel < 16; ++channel) {
      const String nameKey = "ch" + String(channel + 1);
      const String usedKey = "use" + String(channel + 1);
      preferences.getString(nameKey.c_str(), "").toCharArray(devices_[0].channelNames[channel], sizeof(devices_[0].channelNames[channel]));
      devices_[0].channelUsed[channel] = preferences.getBool(usedKey.c_str(), true);
    }
  }

  for (uint8_t slot = 0; slot < kMaxRelayDevices; ++slot) {
    RelaySnapshot& device = devices_[slot];
    const String deviceKey = "d" + String(slot);
    if (!preferences.isKey(deviceKey.c_str()) || preferences.getBytesLength(deviceKey.c_str()) != sizeof(PersistedRelay)) continue;
    PersistedRelay saved;
    if (preferences.getBytes(deviceKey.c_str(), &saved, sizeof(saved)) != sizeof(saved) || saved.version != 1 || saved.id == 0) continue;
    device.configured = true;
    device.id = saved.id;
    device.active = saved.active;
    device.address = saved.address;
    device.channelCount = saved.channelCount;
    device.coilStart = saved.coilStart;
    strlcpy(device.profile, saved.profile, sizeof(device.profile));
    strlcpy(device.name, saved.name, sizeof(device.name));
    device.inputs = isInputProfile(device.profile);
    if (device.channelCount == 0 || device.channelCount > kMaxRelayChannels) device.channelCount = 16;
    for (uint8_t channel = 0; channel < device.channelCount; ++channel) {
      strlcpy(device.channelNames[channel], saved.channelNames[channel], sizeof(device.channelNames[channel]));
      device.channelUsed[channel] = (saved.channelUsed >> channel) & 1;
    }
  }
  preferences.end();
  for (uint8_t slot = 0; slot < kMaxRelayDevices; ++slot) publishInputState(slot);
  if (migrateLegacy) {
    if (saveDevice(0)) {
      preferences.begin("whisli-relay", false);
      preferences.putBool("migrated", true);
      preferences.end();
    }
  }
  // Core 1 like the other application tasks: core 0 is reserved for the network stack.
  xTaskCreatePinnedToCore(taskEntry, "whisli-modbus", 5120, this, whisli::system::kModbusTaskPriority, nullptr, ARDUINO_RUNNING_CORE);
}

bool ModbusRelayManager::saveDevice(uint8_t slot) {
  if (slot >= kMaxRelayDevices) return false;
  RelaySnapshot device;
  xSemaphoreTake(stateLock_, portMAX_DELAY);
  device = devices_[slot];
  xSemaphoreGive(stateLock_);
  if (!device.configured) return false;
  PersistedRelay saved;
  saved.id = device.id;
  saved.address = device.address;
  saved.channelCount = device.channelCount;
  saved.active = device.active;
  saved.coilStart = device.coilStart;
  strlcpy(saved.profile, device.profile, sizeof(saved.profile));
  strlcpy(saved.name, device.name, sizeof(saved.name));
  for (uint8_t channel = 0; channel < device.channelCount; ++channel) {
    if (device.channelUsed[channel]) saved.channelUsed |= static_cast<uint32_t>(1) << channel;
    strlcpy(saved.channelNames[channel], device.channelNames[channel], sizeof(saved.channelNames[channel]));
  }
  whisli::system::ConfigStore preferences;
  preferences.begin("whisli-relay", false);
  const size_t written = preferences.putBytes(("d" + String(slot)).c_str(), &saved, sizeof(saved));
  preferences.end();
  notifyChange(kTopicDevices | kTopicActions | kTopicLights);
  return written == sizeof(saved);
}

int ModbusRelayManager::slotForId(uint8_t id) {
  for (uint8_t slot = 0; slot < kMaxRelayDevices; ++slot) if (devices_[slot].configured && devices_[slot].id == id) return slot;
  return -1;
}

bool ModbusRelayManager::add(const DeviceProfileDefinition& profile, const String& name, uint8_t address, uint8_t channelCount, uint16_t coilStart) {
  if (name.isEmpty() || address == 0 || address > 247 || channelCount == 0 || channelCount > 32 || coilStart > 65535 - channelCount) return false;
  const bool inputs = std::strcmp(profile.deviceClass, "input") == 0;
  if (!profile.available || (!inputs && std::strcmp(profile.deviceClass, "relay") != 0)) return false;
  if (inputs && coilStart > 65535 - (channelCount + 15) / 16) return false;
  xSemaphoreTake(stateLock_, portMAX_DELAY);
  int freeSlot = -1;
  for (uint8_t slot = 0; slot < kMaxRelayDevices; ++slot) {
    if (!devices_[slot].configured && freeSlot < 0) freeSlot = slot;
    if (devices_[slot].configured) {
      if (devices_[slot].address == address) { xSemaphoreGive(stateLock_); return false; }
    }
  }
  if (freeSlot < 0) { xSemaphoreGive(stateLock_); return false; }
  const uint8_t nextId = freeSlot + 1;
  RelaySnapshot& device = devices_[freeSlot];
  device = RelaySnapshot{};
  device.configured = true;
  device.active = true;
  device.id = nextId;
  device.address = address;
  device.channelCount = channelCount;
  device.coilStart = coilStart;
  device.inputs = inputs;
  strlcpy(device.profile, profile.id, sizeof(device.profile));
  name.substring(0, sizeof(device.name) - 1).toCharArray(device.name, sizeof(device.name));
  // Outputs start used; inputs start unused, so 32 unwired inputs do not crowd the button list
  // (and Home Assistant): the ones with a button are switched on in the panel.
  for (uint8_t channel = 0; channel < channelCount; ++channel) device.channelUsed[channel] = !inputs;
  inputFailures_[freeSlot] = 0;
  publishInputState(freeSlot);
  xSemaphoreGive(stateLock_);
  saveDevice(freeSlot);
  const Command command{CommandType::Read, nextId, 0, false};
  xQueueSend(commands_, &command, 0);
  return true;
}

bool ModbusRelayManager::update(uint8_t id, const String& name, uint8_t address, uint8_t channelCount, uint16_t coilStart) {
  if (name.isEmpty() || address == 0 || address > 247 || channelCount == 0 || channelCount > 32 || coilStart > 65535 - channelCount) return false;
  xSemaphoreTake(stateLock_, portMAX_DELAY);
  const int slot = slotForId(id);
  if (slot < 0) { xSemaphoreGive(stateLock_); return false; }
  for (uint8_t other = 0; other < kMaxRelayDevices; ++other) {
    if (other != slot && devices_[other].configured && devices_[other].address == address) { xSemaphoreGive(stateLock_); return false; }
  }
  RelaySnapshot& device = devices_[slot];
  const uint8_t previousCount = device.channelCount;
  device.address = address;
  device.channelCount = channelCount;
  device.coilStart = coilStart;
  name.substring(0, sizeof(device.name) - 1).toCharArray(device.name, sizeof(device.name));
  for (uint8_t channel = previousCount; channel < channelCount; ++channel) device.channelUsed[channel] = false;
  for (uint8_t channel = channelCount; channel < kMaxRelayChannels; ++channel) device.channels[channel] = false;
  inputFailures_[slot] = 0;
  publishInputState(slot);
  xSemaphoreGive(stateLock_);
  saveDevice(slot);
  const Command command{CommandType::Read, id, 0, false};
  xQueueSend(commands_, &command, 0);
  return true;
}

bool ModbusRelayManager::remove(uint8_t id) {
  xSemaphoreTake(stateLock_, portMAX_DELAY);
  const int slot = slotForId(id);
  if (slot < 0) { xSemaphoreGive(stateLock_); return false; }
  devices_[slot] = RelaySnapshot{};
  publishInputState(slot);
  xSemaphoreGive(stateLock_);
  whisli::system::ConfigStore preferences;
  preferences.begin("whisli-relay", false);
  preferences.remove(("d" + String(slot)).c_str());
  preferences.end();
  notifyChange(kTopicDevices | kTopicActions | kTopicLights);
  return true;
}

bool ModbusRelayManager::setActive(uint8_t id, bool active) {
  xSemaphoreTake(stateLock_, portMAX_DELAY);
  const int slot = slotForId(id);
  if (slot < 0) { xSemaphoreGive(stateLock_); return false; }
  devices_[slot].active = active;
  publishInputState(slot);
  xSemaphoreGive(stateLock_);
  saveDevice(slot);
  return true;
}

bool ModbusRelayManager::setChannelUsed(uint8_t id, uint8_t channel, bool used) {
  xSemaphoreTake(stateLock_, portMAX_DELAY);
  const int slot = slotForId(id);
  if (slot < 0 || channel >= devices_[slot < 0 ? 0 : slot].channelCount) { xSemaphoreGive(stateLock_); return false; }
  devices_[slot].channelUsed[channel] = used;
  publishInputState(slot);
  xSemaphoreGive(stateLock_);
  saveDevice(slot);
  return true;
}

bool ModbusRelayManager::setChannel(uint8_t id, uint8_t channel, bool on) {
  const RelaySnapshot device = snapshot(id);
  if (!device.configured || device.inputs || channel >= device.channelCount) return false;
  const Command command{CommandType::Set, id, channel, on};
  return xQueueSend(commands_, &command, 0) == pdTRUE;
}

bool ModbusRelayManager::setAllChannels(uint8_t id, bool on) {
  const RelaySnapshot device = snapshot(id);
  if (!device.configured || device.inputs) return false;
  const Command command{CommandType::SetAll, id, 0, on};
  return xQueueSend(commands_, &command, 0) == pdTRUE;
}

bool ModbusRelayManager::setChannelName(uint8_t id, uint8_t channel, const String& name) {
  if (name.length() >= 33) return false;
  xSemaphoreTake(stateLock_, portMAX_DELAY);
  const int slot = slotForId(id);
  if (slot < 0 || channel >= devices_[slot < 0 ? 0 : slot].channelCount) { xSemaphoreGive(stateLock_); return false; }
  name.toCharArray(devices_[slot].channelNames[channel], sizeof(devices_[slot].channelNames[channel]));
  xSemaphoreGive(stateLock_);
  saveDevice(slot);
  return true;
}

RelaySnapshot ModbusRelayManager::snapshot(uint8_t id) {
  RelaySnapshot copy;
  xSemaphoreTake(stateLock_, portMAX_DELAY);
  const int slot = slotForId(id);
  if (slot >= 0) copy = devices_[slot];
  xSemaphoreGive(stateLock_);
  return copy;
}

uint8_t ModbusRelayManager::deviceIds(uint8_t* output, uint8_t capacity) {
  uint8_t count = 0;
  xSemaphoreTake(stateLock_, portMAX_DELAY);
  for (const auto& device : devices_) if (device.configured && count < capacity) output[count++] = device.id;
  xSemaphoreGive(stateLock_);
  return count;
}

void ModbusRelayManager::taskEntry(void* context) { static_cast<ModbusRelayManager*>(context)->task(); }

void ModbusRelayManager::task() {
  serial_.begin(kBaudRate, SERIAL_8N1, kRxPin, kTxPin);
  uart_set_pin(UART_NUM_1, kTxPin, kRxPin, kRtsPin, UART_PIN_NO_CHANGE);
  uart_set_mode(UART_NUM_1, UART_MODE_RS485_HALF_DUPLEX);
  Serial.printf("[MODBUS] Relay bus UART1 TX=%d RX=%d RTS=%d %lu 8N1\n", kTxPin, kRxPin, kRtsPin, kBaudRate);
  uint32_t lastReadAt[kMaxRelayDevices] = {};
  uint32_t lastInputReadAt[kMaxRelayDevices] = {};
  for (;;) {
    // Wake for the next input read at the latest; relay commands still run as soon as they come.
    InputModuleState inputs[kMaxRelayDevices];
    inputStates(inputs);
    uint32_t wait = 25;
    const uint32_t now = millis();
    for (uint8_t slot = 0; slot < kMaxRelayDevices; ++slot) {
      if (!inputs[slot].id) continue;
      const uint32_t interval = inputs[slot].online || inputFailures_[slot] < kInputFailuresUntilOffline ? kInputPollIntervalMs : kOfflineInputPollIntervalMs;
      const uint32_t elapsed = now - lastInputReadAt[slot];
      wait = elapsed >= interval ? 0 : min(wait, interval - elapsed);
      if (!wait) break;
    }
    Command command{};
    const bool received = xQueueReceive(commands_, &command, pdMS_TO_TICKS(wait)) == pdTRUE;
    if (received) {
      const RelaySnapshot device = snapshot(command.id);
      if (device.configured && device.inputs) {
        if (command.type == CommandType::Read) readInputs(device);
      } else if (device.configured) {
        if (command.type == CommandType::Set) writeChannel(device, command.channel, command.on);
        if (command.type == CommandType::SetAll) {
          for (uint8_t channel = 0; channel < device.channelCount; ++channel) if (!writeChannel(device, channel, command.on)) break;
          readChannels(device);
        }
        if (command.type == CommandType::Read) readChannels(device);
      }
    }
    runPendingProbe();
    // Input modules: every few tens of milliseconds, one request per module for all its inputs.
    for (uint8_t slot = 0; slot < kMaxRelayDevices; ++slot) {
      if (!inputs[slot].id) continue;
      const uint32_t interval = inputs[slot].online || inputFailures_[slot] < kInputFailuresUntilOffline ? kInputPollIntervalMs : kOfflineInputPollIntervalMs;
      if (millis() - lastInputReadAt[slot] < interval) continue;
      lastInputReadAt[slot] = millis();
      const RelaySnapshot device = snapshot(inputs[slot].id);
      if (device.configured && device.inputs && device.active) readInputs(device);
    }
    // Relay modules: their state, in case something else switched them.
    uint8_t configuredIds[kMaxRelayDevices] = {};
    const uint8_t deviceCount = deviceIds(configuredIds, kMaxRelayDevices);
    for (uint8_t index = 0; index < deviceCount; ++index) {
      if (millis() - lastReadAt[index] < 3000) continue;
      RelaySnapshot device = snapshot(configuredIds[index]);
      if (device.configured && device.active && !device.inputs) readChannels(device);
      lastReadAt[index] = millis();
    }
  }
}

// Back-to-back frames would run into the previous reply's tail: keep the silence RTU requires.
void ModbusRelayManager::keepFrameGap() {
  const int64_t quietFor = esp_timer_get_time() - lastFrameEndUs_;
  if (quietFor < static_cast<int64_t>(kInterFrameGapUs)) vTaskDelay(pdMS_TO_TICKS((kInterFrameGapUs - quietFor) / 1000 + 1));
}

bool ModbusRelayManager::request(const uint8_t* frame, size_t frameSize, uint8_t* response, size_t responseSize, uint32_t timeoutMs) {
  keepFrameGap();
  while (serial_.available()) serial_.read();
  if (serial_.write(frame, frameSize) != frameSize) return false;
  serial_.flush();
  size_t received = 0;
  const uint32_t startedAt = millis();
  const uint32_t timeout = timeoutMs ? timeoutMs : kResponseTimeoutMs;
  while (received < responseSize && millis() - startedAt < timeout) {
    if (serial_.available()) {
      response[received++] = serial_.read();
      if (received == 5 && (response[1] & 0x80)) break;  // an exception reply is complete: do not wait out the timeout
    } else {
      vTaskDelay(1);
    }
  }
  lastFrameEndUs_ = esp_timer_get_time();
  return received == responseSize && crc16(response, responseSize - 2) ==
      static_cast<uint16_t>(response[responseSize - 2] | (response[responseSize - 1] << 8));
}

bool ModbusRelayManager::writeChannel(const RelaySnapshot& device, uint8_t channel, bool on) {
  const uint16_t address = device.coilStart + channel;
  uint8_t frame[8] = {device.address, 0x05, static_cast<uint8_t>(address >> 8), static_cast<uint8_t>(address), static_cast<uint8_t>(on ? 0xFF : 0x00), 0x00, 0, 0};
  addCrc(frame, sizeof(frame));
  uint8_t response[8] = {};
  const bool successful = request(frame, sizeof(frame), response, sizeof(response)) && memcmp(frame, response, sizeof(frame)) == 0;
  setOnline(device.id, successful);
  if (successful) {
    xSemaphoreTake(stateLock_, portMAX_DELAY);
    const int slot = slotForId(device.id);
    const bool changed = slot >= 0 && channel < devices_[slot].channelCount && devices_[slot].channels[channel] != on;
    if (changed) devices_[slot].channels[channel] = on;
    xSemaphoreGive(stateLock_);
    if (changed) notifyChange(kTopicDevices | kTopicActions | kTopicLights);  // relay lights follow their outputs
  }
  return successful;
}

bool ModbusRelayManager::readChannels(const RelaySnapshot& device) {
  const uint8_t bytes = (device.channelCount + 7) / 8;
  uint8_t frame[8] = {device.address, 0x01, static_cast<uint8_t>(device.coilStart >> 8), static_cast<uint8_t>(device.coilStart), static_cast<uint8_t>(device.channelCount >> 8), static_cast<uint8_t>(device.channelCount), 0, 0};
  addCrc(frame, sizeof(frame));
  uint8_t response[37] = {};
  const bool successful = request(frame, sizeof(frame), response, 3 + bytes + 2) &&
      response[0] == device.address && response[1] == 0x01 && response[2] == bytes;
  setOnline(device.id, successful);
  if (successful) {
    xSemaphoreTake(stateLock_, portMAX_DELAY);
    const int slot = slotForId(device.id);
    bool changed = false;
    if (slot >= 0) for (uint8_t channel = 0; channel < device.channelCount; ++channel) {
      const bool on = (response[3 + channel / 8] >> (channel % 8)) & 1;
      changed |= devices_[slot].channels[channel] != on;
      devices_[slot].channels[channel] = on;
    }
    xSemaphoreGive(stateLock_);
    if (changed) notifyChange(kTopicDevices | kTopicActions | kTopicLights);  // relay lights follow their outputs
  }
  return successful;
}

bool ModbusRelayManager::probe(uint8_t address, uint8_t function, uint16_t start, uint16_t count, ProbeResult& result) {
  if (address == 0 || address > 247 || function < 1 || function > 4 || count == 0) return false;
  // Keep the reply within the buffer: bits pack 8 per byte, registers take 2 bytes each.
  if (function <= 2 ? count > 1000 : count > 64) return false;
  if (xSemaphoreTake(probeLock_, pdMS_TO_TICKS(5000)) != pdTRUE) return false;
  uint8_t frame[8] = {address, function, static_cast<uint8_t>(start >> 8), static_cast<uint8_t>(start),
                      static_cast<uint8_t>(count >> 8), static_cast<uint8_t>(count), 0, 0};
  addCrc(frame, sizeof(frame));
  xSemaphoreTake(stateLock_, portMAX_DELAY);
  memcpy(probeFrame_, frame, sizeof(frame));
  const uint32_t sequence = probeRequested_ + 1;
  probeRequested_ = sequence;
  probePending_ = true;
  xSemaphoreGive(stateLock_);
  bool finished = false;
  // A SetAll on a large relay module can keep the bus busy for seconds; wait it out.
  const uint32_t startedAt = millis();
  while (!finished && millis() - startedAt < 10000) {
    if (xSemaphoreTake(probeDone_, pdMS_TO_TICKS(100)) != pdTRUE) continue;
    xSemaphoreTake(stateLock_, portMAX_DELAY);
    finished = probeAnswered_ == sequence;
    if (finished) result = probeStorage_;
    xSemaphoreGive(stateLock_);
  }
  if (!finished) {
    xSemaphoreTake(stateLock_, portMAX_DELAY);
    probePending_ = false;
    xSemaphoreGive(stateLock_);
  }
  xSemaphoreGive(probeLock_);
  return finished;
}

void ModbusRelayManager::runPendingProbe() {
  uint8_t frame[8];
  xSemaphoreTake(stateLock_, portMAX_DELAY);
  const bool pending = probePending_;
  const uint32_t sequence = probeRequested_;
  if (pending) memcpy(frame, probeFrame_, sizeof(frame));
  probePending_ = false;
  xSemaphoreGive(stateLock_);
  if (!pending) return;

  // Read whatever comes back until the line goes quiet: a normal reply, an exception reply or noise
  // are all useful when looking for an unknown module.
  ProbeResult result{};
  keepFrameGap();
  while (serial_.available()) serial_.read();
  if (serial_.write(frame, sizeof(frame)) == sizeof(frame)) {
    serial_.flush();
    const uint32_t startedAt = millis();
    uint32_t lastByteAt = 0;
    while (millis() - startedAt < kResponseTimeoutMs) {
      if (serial_.available()) {
        const int value = serial_.read();
        if (result.length < sizeof(result.bytes)) result.bytes[result.length++] = static_cast<uint8_t>(value);
        lastByteAt = millis();
      } else if (result.length && millis() - lastByteAt > 10) {
        break;  // 3.5 characters at 9600 bps is ~4 ms; 10 ms of silence ends the frame
      } else {
        vTaskDelay(1);
      }
    }
  }
  lastFrameEndUs_ = esp_timer_get_time();
  result.answered = result.length > 0;
  xSemaphoreTake(stateLock_, portMAX_DELAY);
  probeStorage_ = result;
  probeAnswered_ = sequence;
  xSemaphoreGive(stateLock_);
  xSemaphoreGive(probeDone_);
}

// Input bits come 16 to a holding register, input 1 in its lowest bit (N4DIH32: X00 = bit 0 of 0x00C0).
bool ModbusRelayManager::readInputs(const RelaySnapshot& device) {
  const uint8_t registers = (device.channelCount + 15) / 16;
  uint8_t frame[8] = {device.address, 0x03, static_cast<uint8_t>(device.coilStart >> 8), static_cast<uint8_t>(device.coilStart), 0, registers, 0, 0};
  addCrc(frame, sizeof(frame));
  uint8_t response[5 + 2 * ((kMaxRelayChannels + 15) / 16)] = {};
  const bool successful = request(frame, sizeof(frame), response, 5 + 2 * registers, kInputResponseTimeoutMs) &&
      response[0] == device.address && response[1] == 0x03 && response[2] == 2 * registers;
  bool changed = false;
  xSemaphoreTake(stateLock_, portMAX_DELAY);
  const int slot = slotForId(device.id);
  if (slot >= 0 && devices_[slot].inputs) {
    RelaySnapshot& current = devices_[slot];
    if (successful) {
      inputFailures_[slot] = 0;
      changed |= !current.online;
      current.online = true;
      for (uint8_t channel = 0; channel < current.channelCount && channel < device.channelCount; ++channel) {
        const uint16_t word = static_cast<uint16_t>(response[3 + 2 * (channel / 16)] << 8 | response[4 + 2 * (channel / 16)]);
        const bool pressed = (word >> (channel % 16)) & 1;
        changed |= current.channels[channel] != pressed;
        current.channels[channel] = pressed;
      }
    } else if (inputFailures_[slot] < kInputFailuresUntilOffline && ++inputFailures_[slot] == kInputFailuresUntilOffline) {
      // Gone: every input reads as released, so a button held at that moment does not stay held.
      changed = true;
      current.online = false;
      for (bool& pressed : current.channels) pressed = false;
    }
    if (changed) publishInputState(slot);
  }
  xSemaphoreGive(stateLock_);
  if (changed) notifyChange(kTopicDevices | kTopicActions);
  return successful;
}

void ModbusRelayManager::publishInputState(uint8_t slot) {
  const RelaySnapshot& device = devices_[slot];
  InputModuleState state;
  if (device.configured && device.inputs && device.active) {
    state.id = device.id;
    state.channelCount = device.channelCount;
    state.online = device.online;
    for (uint8_t channel = 0; channel < device.channelCount; ++channel) {
      if (device.channelUsed[channel]) state.used |= 1UL << channel;
      if (device.channels[channel]) state.pressed |= 1UL << channel;
    }
  }
  portENTER_CRITICAL(&inputMux_);
  inputStates_[slot] = state;
  portEXIT_CRITICAL(&inputMux_);
}

void ModbusRelayManager::inputStates(InputModuleState* output) {
  portENTER_CRITICAL(&inputMux_);
  memcpy(output, inputStates_, sizeof(inputStates_));
  portEXIT_CRITICAL(&inputMux_);
}

bool ModbusRelayManager::isInputProfile(const char* profile) {
  const DeviceProfileDefinition* definition = findDeviceProfile(profile);
  return definition && std::strcmp(definition->deviceClass, "input") == 0;
}

void ModbusRelayManager::setOnline(uint8_t id, bool online) {
  xSemaphoreTake(stateLock_, portMAX_DELAY);
  const int slot = slotForId(id);
  const bool changed = slot >= 0 && devices_[slot].online != online;
  if (changed) devices_[slot].online = online;
  if (changed) publishInputState(slot);
  xSemaphoreGive(stateLock_);
  if (changed) notifyChange(kTopicDevices | kTopicActions | kTopicLights);  // relay lights follow their outputs
}

}  // namespace whisli::app
