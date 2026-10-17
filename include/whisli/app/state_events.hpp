#pragma once

#include <Arduino.h>

namespace whisli::app {

// What changed, so listeners (the panel over WebSocket, later MQTT) refresh only what they show.
enum ChangeTopic : uint8_t {
  kTopicDevices = 1 << 0,  // controller and relay outputs, inputs, link status, device settings
  kTopicLights = 1 << 1,   // light levels, colours and settings
  kTopicActions = 1 << 2,  // button events and actions
  kTopicZones = 1 << 3,    // zones and what belongs to them
};

// Thread-safe; modules call it from any task when their state changes.
void notifyChange(uint8_t topics);
// Returns the topics changed since the last call and clears them, with the matching revision.
uint8_t takeChanges(uint32_t& revision);

// Button activity for live views (the panel map): "press", "release" and recognised gestures
// ("click", "doubleClick", ...). Recorded by the control task without touching the network; loop()
// takes them and broadcasts. A small ring: when it overflows, the oldest are dropped.
struct InputEventRecord { uint8_t input; const char* trigger; };
void recordInputEvent(uint8_t input, const char* trigger);
bool takeInputEvent(InputEventRecord& output);

}  // namespace whisli::app
