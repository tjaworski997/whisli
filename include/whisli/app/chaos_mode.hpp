#pragma once

#include <Arduino.h>

#include "whisli/app/controller_io.hpp"
#include "whisli/app/lights.hpp"
#include "whisli/app/modbus_relays.hpp"

namespace whisli::app {

// Chaos mode: a load test of the whole installation. DMX lights take random brightness and colours,
// every output of the controller and of active relay modules (used or not) switches at random until
// stopped. Then everything returns to how it was before.
//
// Runs from loop() at the lowest priority under the state lock; buttons and lights stay first.
class ChaosMode {
 public:
  ChaosMode(ControllerIo& controller, ModbusRelayManager& relays, LightManager& lights)
      : controller_(controller), relays_(relays), lights_(lights) {}

  void start();
  void stop();
  void tick();

  bool running() const { return running_; }
  bool restoring() const { return restoring_; }
  uint32_t elapsedSeconds() const { return running_ ? (millis() - startedAt_) / 1000 : 0; }
  uint32_t switches() const { return switches_; }

 private:
  bool restore();  // one round of putting outputs back; true once everything is as before

  ControllerIo& controller_;
  ModbusRelayManager& relays_;
  LightManager& lights_;
  bool running_ = false;
  bool restoring_ = false;
  uint32_t startedAt_ = 0;
  uint32_t restoreStartedAt_ = 0;
  uint32_t lastTickAt_ = 0;
  uint32_t switches_ = 0;

  bool outputsBefore_[kControllerOutputCount] = {};
  uint32_t nextOutputAt_ = 0;
  struct Module { uint8_t id = 0; uint8_t channelCount = 0; bool before[kMaxRelayChannels] = {}; uint32_t nextAt = 0; };
  Module modules_[kMaxRelayDevices];
  uint8_t moduleCount_ = 0;
};

}  // namespace whisli::app
