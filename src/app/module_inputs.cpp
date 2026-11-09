#include "whisli/app/module_inputs.hpp"

#include "whisli/app/state_events.hpp"

namespace whisli::app {
namespace {

// The module filters its optocoupler inputs and is read every ~25 ms, which already spans any contact
// bounce; a debounce on top would only add a reading's worth of delay to every press.
InputTiming moduleTiming() {
  InputTiming timing;
  timing.debounceMs = 0;
  return timing;
}

}  // namespace

ModuleInputs::ModuleInputs(ModbusRelayManager& relays) : relays_(relays) {}

void ModuleInputs::setEventHandler(EventHandler handler, void* context) {
  handler_ = handler;
  context_ = context;
}

void ModuleInputs::reset(uint8_t slot, uint8_t id) {
  ids_[slot] = id;
  used_[slot] = 0;
  pressed_[slot] = 0;
  for (uint8_t channel = 0; channel < kMaxRelayChannels; ++channel) {
    buttons_[slot][channel] = ButtonInput(moduleTiming());
    lastEvents_[slot][channel] = ButtonEvent::None;
  }
}

void ModuleInputs::poll() {
  InputModuleState states[kMaxRelayDevices];
  relays_.inputStates(states);
  const uint32_t now = millis();
  for (uint8_t slot = 0; slot < kMaxRelayDevices; ++slot) {
    const InputModuleState& state = states[slot];
    if (state.id != ids_[slot]) reset(slot, state.id);  // added, removed or switched off
    if (!state.id) continue;
    for (uint8_t channel = 0; channel < state.channelCount && channel < kMaxRelayChannels; ++channel) {
      const uint32_t bit = 1UL << channel;
      if (!(state.used & bit)) { used_[slot] &= ~bit; continue; }
      ButtonInput& button = buttons_[slot][channel];
      // Just switched on in the panel: start from the current level, like after a restart, so a
      // button held at that moment does not count as a press.
      if (!(used_[slot] & bit)) { button = ButtonInput(moduleTiming()); used_[slot] |= bit; }
      const bool pressed = state.pressed & bit;
      button.update(pressed, now);
      const InputRef input{state.id, channel};
      if (pressed != static_cast<bool>(pressed_[slot] & bit)) {
        pressed_[slot] ^= bit;
        recordInputEvent(input, pressed ? "press" : "release");
      }
      ButtonEvent events[4];
      const uint8_t count = button.takeEvents(events, 4);
      lastEvents_[slot][channel] = button.lastEvent();
      if (handler_) for (uint8_t index = 0; index < count; ++index) handler_(context_, input, events[index]);
    }
  }
}

ButtonEvent ModuleInputs::lastEvent(uint8_t device, uint8_t channel) const {
  if (channel >= kMaxRelayChannels) return ButtonEvent::None;
  for (uint8_t slot = 0; slot < kMaxRelayDevices; ++slot) if (ids_[slot] && ids_[slot] == device) return lastEvents_[slot][channel];
  return ButtonEvent::None;
}

}  // namespace whisli::app
