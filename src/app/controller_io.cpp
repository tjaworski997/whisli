#include "whisli/app/controller_io.hpp"

#include <Preferences.h>
#include <Wire.h>
#include <cstring>

#include "whisli/app/state_events.hpp"

namespace whisli::app {
namespace {

constexpr int kI2cSdaPin = 42;
constexpr int kI2cSclPin = 41;
constexpr uint8_t kExpanderAddress = 0x20;
constexpr uint8_t kOutputRegister = 0x01;
constexpr uint8_t kConfigurationRegister = 0x03;
constexpr uint32_t kInputPollIntervalMs = 10;
constexpr int kInputPins[kControllerInputCount] = {4, 5, 6, 7, 8, 9, 10, 11};

struct PersistedControllerV1 {
  uint8_t version = 1;
  char name[33] = "Whisli Controller";
  uint8_t outputUsed = 0xFF;
  char outputNames[kControllerOutputCount][33] = {};
};

struct PersistedController {
  uint8_t version = 2;
  char name[33] = "Whisli Controller";
  uint8_t outputUsed = 0xFF;
  char outputNames[kControllerOutputCount][33] = {};
  uint8_t inputUsed = 0xFF;
  char inputNames[kControllerInputCount][33] = {};
};

bool writeRegister(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(kExpanderAddress);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

}  // namespace

bool ControllerIo::begin() {
  lock_ = xSemaphoreCreateMutex();
  if (!lock_) return false;

  Preferences preferences;
  preferences.begin("whisli-board", true);
  PersistedController saved;
  const size_t savedSize = preferences.getBytesLength("config");
  bool migrateV1 = false;
  bool settingsLoaded = false;
  if (savedSize == sizeof(saved) &&
      preferences.getBytes("config", &saved, sizeof(saved)) == sizeof(saved) && saved.version == 2) {
    settingsLoaded = true;
    strlcpy(device_.name, saved.name, sizeof(device_.name));
    for (uint8_t index = 0; index < kControllerOutputCount; ++index) {
      device_.outputUsed[index] = (saved.outputUsed >> index) & 1;
      strlcpy(device_.outputNames[index], saved.outputNames[index], sizeof(device_.outputNames[index]));
    }
    for (uint8_t index = 0; index < kControllerInputCount; ++index) {
      device_.inputUsed[index] = (saved.inputUsed >> index) & 1;
      strlcpy(device_.inputNames[index], saved.inputNames[index], sizeof(device_.inputNames[index]));
    }
  } else if (savedSize == sizeof(PersistedControllerV1)) {
    PersistedControllerV1 legacy;
    if (preferences.getBytes("config", &legacy, sizeof(legacy)) == sizeof(legacy) && legacy.version == 1) {
      strlcpy(device_.name, legacy.name, sizeof(device_.name));
      for (uint8_t index = 0; index < kControllerOutputCount; ++index) {
        device_.outputUsed[index] = (legacy.outputUsed >> index) & 1;
        strlcpy(device_.outputNames[index], legacy.outputNames[index], sizeof(device_.outputNames[index]));
        device_.inputUsed[index] = true;
      }
      migrateV1 = true;
      settingsLoaded = true;
    }
  }
  if (!settingsLoaded) {
    for (bool& used : device_.outputUsed) used = true;
    for (bool& used : device_.inputUsed) used = true;
  }
  preferences.end();

  Wire.begin(kI2cSdaPin, kI2cSclPin);
  Wire.setClock(100000);
  for (const int pin : kInputPins) pinMode(pin, INPUT_PULLUP);
  const uint32_t now = millis();
  for (uint8_t index = 0; index < kControllerInputCount; ++index) {
    const bool levelHigh = digitalRead(kInputPins[index]) == HIGH;
    buttonInputs_[index].begin(!levelHigh, now);
    device_.inputsHigh[index] = levelHigh;
  }

  // Wstępna korekta polaryzacji według obserwacji diod na płytce:
  // logiczne OFF ustawia HIGH na EXIO. Wymaga potwierdzenia na zaciskach DO.
  // Ustawiamy wszystkie wyjścia na logiczne OFF przed udostępnieniem API.
  xSemaphoreTake(lock_, portMAX_DELAY);
  const bool ready = writeRegister(kConfigurationRegister, 0x00) && writeRegister(kOutputRegister, 0xFF);
  device_.online = ready;
  outputState_ = 0;
  xSemaphoreGive(lock_);
  if (migrateV1) save();
  Serial.printf("[BOARD] Local digital outputs %s (TCA9554PWR 0x20, I2C SDA=%d SCL=%d)\n", ready ? "ready" : "offline", kI2cSdaPin, kI2cSclPin);
  return ready;
}

ControllerDevice ControllerIo::snapshot() {
  xSemaphoreTake(lock_, portMAX_DELAY);
  ControllerDevice copy = device_;
  xSemaphoreGive(lock_);
  return copy;
}

bool ControllerIo::save() {
  PersistedController saved;
  strlcpy(saved.name, device_.name, sizeof(saved.name));
  saved.outputUsed = 0;
  saved.inputUsed = 0;
  for (uint8_t index = 0; index < kControllerOutputCount; ++index) {
    if (device_.outputUsed[index]) saved.outputUsed |= static_cast<uint8_t>(1U << index);
    strlcpy(saved.outputNames[index], device_.outputNames[index], sizeof(saved.outputNames[index]));
  }
  for (uint8_t index = 0; index < kControllerInputCount; ++index) {
    if (device_.inputUsed[index]) saved.inputUsed |= static_cast<uint8_t>(1U << index);
    strlcpy(saved.inputNames[index], device_.inputNames[index], sizeof(saved.inputNames[index]));
  }
  Preferences preferences;
  preferences.begin("whisli-board", false);
  const size_t written = preferences.putBytes("config", &saved, sizeof(saved));
  preferences.end();
  notifyChange(kTopicDevices | kTopicActions);
  return written == sizeof(saved);
}

bool ControllerIo::setName(const String& name) {
  if (name.isEmpty() || name.length() >= sizeof(device_.name)) return false;
  xSemaphoreTake(lock_, portMAX_DELAY);
  name.toCharArray(device_.name, sizeof(device_.name));
  xSemaphoreGive(lock_);
  return save();
}

bool ControllerIo::writeOutputsLocked(uint8_t state) {
  const uint8_t physicalState = static_cast<uint8_t>(~state);
  notifyChange(kTopicDevices | kTopicActions | kTopicLights);  // relay lights follow their outputs
  if (!device_.online || !writeRegister(kOutputRegister, physicalState)) {
    device_.online = false;
    return false;
  }
  outputState_ = state;
  for (uint8_t index = 0; index < kControllerOutputCount; ++index) device_.outputs[index] = (state >> index) & 1;
  return true;
}

bool ControllerIo::setOutput(uint8_t channel, bool on) {
  if (channel >= kControllerOutputCount) return false;
  xSemaphoreTake(lock_, portMAX_DELAY);
  const uint8_t bit = static_cast<uint8_t>(1U << channel);
  const uint8_t next = on ? outputState_ | bit : outputState_ & static_cast<uint8_t>(~bit);
  const bool result = writeOutputsLocked(next);
  xSemaphoreGive(lock_);
  return result;
}

bool ControllerIo::setAllOutputs(bool on) {
  xSemaphoreTake(lock_, portMAX_DELAY);
  const bool result = writeOutputsLocked(on ? 0xFF : 0x00);
  xSemaphoreGive(lock_);
  return result;
}

bool ControllerIo::setOutputName(uint8_t channel, const String& name) {
  if (channel >= kControllerOutputCount || name.length() >= sizeof(device_.outputNames[0])) return false;
  xSemaphoreTake(lock_, portMAX_DELAY);
  name.toCharArray(device_.outputNames[channel], sizeof(device_.outputNames[channel]));
  xSemaphoreGive(lock_);
  return save();
}

bool ControllerIo::setOutputUsed(uint8_t channel, bool used) {
  if (channel >= kControllerOutputCount) return false;
  xSemaphoreTake(lock_, portMAX_DELAY);
  device_.outputUsed[channel] = used;
  xSemaphoreGive(lock_);
  return save();
}

bool ControllerIo::setInputName(uint8_t channel, const String& name) {
  if (channel >= kControllerInputCount || name.length() >= sizeof(device_.inputNames[0])) return false;
  xSemaphoreTake(lock_, portMAX_DELAY);
  name.toCharArray(device_.inputNames[channel], sizeof(device_.inputNames[channel]));
  xSemaphoreGive(lock_);
  return save();
}

bool ControllerIo::setInputUsed(uint8_t channel, bool used) {
  if (channel >= kControllerInputCount) return false;
  xSemaphoreTake(lock_, portMAX_DELAY);
  device_.inputUsed[channel] = used;
  xSemaphoreGive(lock_);
  return save();
}

void ControllerIo::setInputEventHandler(InputEventHandler handler, void* context) {
  inputEventHandler_ = handler;
  inputEventContext_ = context;
}

void ControllerIo::poll() {
  const uint32_t now = millis();
  if (now - lastInputPollAt_ < kInputPollIntervalMs) return;
  lastInputPollAt_ = now;

  bool inputLevelsHigh[kControllerInputCount] = {};
  ButtonEvent inputEvents[kControllerInputCount] = {};
  ButtonEvent newEvents[kControllerInputCount][4] = {};
  uint8_t newEventCounts[kControllerInputCount] = {};
  for (uint8_t index = 0; index < kControllerInputCount; ++index) {
    const bool levelHigh = digitalRead(kInputPins[index]) == HIGH;
    buttonInputs_[index].update(!levelHigh, now);
    inputLevelsHigh[index] = !buttonInputs_[index].isPressed();
    inputEvents[index] = buttonInputs_[index].lastEvent();
    newEventCounts[index] = buttonInputs_[index].takeEvents(newEvents[index], 4);
  }

  bool changed = false;
  xSemaphoreTake(lock_, portMAX_DELAY);
  for (uint8_t index = 0; index < kControllerInputCount; ++index) {
    changed |= device_.inputsHigh[index] != inputLevelsHigh[index] || device_.inputLastEvents[index] != inputEvents[index];
    // Press and release right away, for live views; gestures follow once recognised.
    if (device_.inputsHigh[index] != inputLevelsHigh[index]) recordInputEvent(index, inputLevelsHigh[index] ? "release" : "press");
    device_.inputsHigh[index] = inputLevelsHigh[index];
    device_.inputLastEvents[index] = inputEvents[index];
  }
  xSemaphoreGive(lock_);
  if (changed) notifyChange(kTopicDevices | kTopicActions);

  if (inputEventHandler_) {
    for (uint8_t input = 0; input < kControllerInputCount; ++input) {
      for (uint8_t event = 0; event < newEventCounts[input]; ++event) {
        inputEventHandler_(inputEventContext_, input, newEvents[input][event]);
      }
    }
  }
}

}  // namespace whisli::app
