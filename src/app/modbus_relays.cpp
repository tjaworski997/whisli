#include "whisli/app/modbus_relays.hpp"

#include <Preferences.h>
#include <driver/uart.h>
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
  commands_ = xQueueCreate(16, sizeof(Command));
  Preferences preferences;
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
    if (device.channelCount == 0 || device.channelCount > kMaxRelayChannels) device.channelCount = 16;
    for (uint8_t channel = 0; channel < device.channelCount; ++channel) {
      strlcpy(device.channelNames[channel], saved.channelNames[channel], sizeof(device.channelNames[channel]));
      device.channelUsed[channel] = (saved.channelUsed >> channel) & 1;
    }
  }
  preferences.end();
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
  Preferences preferences;
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
  if (!profile.available || std::strcmp(profile.deviceClass, "relay") != 0) return false;
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
  strlcpy(device.profile, profile.id, sizeof(device.profile));
  name.substring(0, sizeof(device.name) - 1).toCharArray(device.name, sizeof(device.name));
  for (uint8_t channel = 0; channel < channelCount; ++channel) device.channelUsed[channel] = true;
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
  xSemaphoreGive(stateLock_);
  Preferences preferences;
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
  xSemaphoreGive(stateLock_);
  saveDevice(slot);
  return true;
}

bool ModbusRelayManager::setChannelUsed(uint8_t id, uint8_t channel, bool used) {
  xSemaphoreTake(stateLock_, portMAX_DELAY);
  const int slot = slotForId(id);
  if (slot < 0 || channel >= devices_[slot < 0 ? 0 : slot].channelCount) { xSemaphoreGive(stateLock_); return false; }
  devices_[slot].channelUsed[channel] = used;
  xSemaphoreGive(stateLock_);
  saveDevice(slot);
  return true;
}

bool ModbusRelayManager::setChannel(uint8_t id, uint8_t channel, bool on) {
  const RelaySnapshot device = snapshot(id);
  if (!device.configured || channel >= device.channelCount) return false;
  const Command command{CommandType::Set, id, channel, on};
  return xQueueSend(commands_, &command, 0) == pdTRUE;
}

bool ModbusRelayManager::setAllChannels(uint8_t id, bool on) {
  if (!snapshot(id).configured) return false;
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
  for (;;) {
    Command command{};
    const bool received = xQueueReceive(commands_, &command, pdMS_TO_TICKS(25)) == pdTRUE;
    if (received) {
      const RelaySnapshot device = snapshot(command.id);
      if (device.configured) {
        if (command.type == CommandType::Set) writeChannel(device, command.channel, command.on);
        if (command.type == CommandType::SetAll) {
          for (uint8_t channel = 0; channel < device.channelCount; ++channel) if (!writeChannel(device, channel, command.on)) break;
          readChannels(device);
        }
        if (command.type == CommandType::Read) readChannels(device);
      }
    }
    uint8_t configuredIds[kMaxRelayDevices] = {};
    const uint8_t deviceCount = deviceIds(configuredIds, kMaxRelayDevices);
    for (uint8_t index = 0; index < deviceCount; ++index) {
      if (millis() - lastReadAt[index] < 3000) continue;
      RelaySnapshot device = snapshot(configuredIds[index]);
      if (device.configured && device.active) readChannels(device);
      lastReadAt[index] = millis();
    }
  }
}

bool ModbusRelayManager::request(const uint8_t* frame, size_t frameSize, uint8_t* response, size_t responseSize) {
  while (serial_.available()) serial_.read();
  if (serial_.write(frame, frameSize) != frameSize) return false;
  serial_.flush();
  size_t received = 0;
  const uint32_t startedAt = millis();
  while (received < responseSize && millis() - startedAt < kResponseTimeoutMs) {
    if (serial_.available()) response[received++] = serial_.read();
    else vTaskDelay(1);
  }
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

void ModbusRelayManager::setOnline(uint8_t id, bool online) {
  xSemaphoreTake(stateLock_, portMAX_DELAY);
  const int slot = slotForId(id);
  const bool changed = slot >= 0 && devices_[slot].online != online;
  if (changed) devices_[slot].online = online;
  xSemaphoreGive(stateLock_);
  if (changed) notifyChange(kTopicDevices | kTopicActions | kTopicLights);  // relay lights follow their outputs
}

}  // namespace whisli::app
