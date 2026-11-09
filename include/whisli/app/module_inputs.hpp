#pragma once

#include <Arduino.h>

#include "whisli/app/button_input.hpp"
#include "whisli/app/input_ref.hpp"
#include "whisli/app/modbus_relays.hpp"

namespace whisli::app {

// Buttons on Modbus input modules: the levels the Modbus task last read go through the same gesture
// recognition as the board's inputs. Runs in the control task, under the state lock, and never waits
// for the bus: it only takes the latest levels the Modbus task left behind.
class ModuleInputs {
 public:
  using EventHandler = void (*)(void* context, InputRef input, ButtonEvent event);

  explicit ModuleInputs(ModbusRelayManager& relays);
  void setEventHandler(EventHandler handler, void* context);
  void poll();
  // Under the state lock: the last gesture of an input, for the panel.
  ButtonEvent lastEvent(uint8_t device, uint8_t channel) const;

 private:
  void reset(uint8_t slot, uint8_t id);

  ModbusRelayManager& relays_;
  EventHandler handler_ = nullptr;
  void* context_ = nullptr;
  uint8_t ids_[kMaxRelayDevices] = {};       // module per slot as last seen; another id starts afresh
  uint32_t used_[kMaxRelayDevices] = {};
  uint32_t pressed_[kMaxRelayDevices] = {};  // as last fed, for press/release on the live map
  // ~20 KB together: allocated, so they land in PSRAM.
  ButtonInput (&buttons_)[kMaxRelayDevices][kMaxRelayChannels] = *new ButtonInput[1][kMaxRelayDevices][kMaxRelayChannels];
  ButtonEvent (&lastEvents_)[kMaxRelayDevices][kMaxRelayChannels] = *new ButtonEvent[1][kMaxRelayDevices][kMaxRelayChannels]();
};

}  // namespace whisli::app
