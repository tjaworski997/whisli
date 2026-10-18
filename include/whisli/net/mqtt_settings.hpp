#pragma once

#include <Arduino.h>

namespace whisli::net::mqtt {

// Connection settings for an MQTT broker (e.g. the one in Home Assistant), kept in "whisli-mqtt".
// The client itself is app::MqttBridge; this module only stores settings and its live status.
struct Settings {
  bool enabled = false;
  String host;
  uint16_t port = 1883;
  String user;
  String password;
  String baseTopic = "whisli";            // states and commands under <baseTopic>/...
  bool discovery = true;                  // announce lights and buttons to Home Assistant
  String discoveryPrefix = "homeassistant";
  String deviceName;                      // how the controller is named in Home Assistant; empty: its own name
};

Settings load();
bool save(const Settings& settings);
// A topic level without MQTT wildcards or empty levels: "whisli", "dom/whisli".
bool validTopic(const String& topic);

// Bumped by every save(), so the client knows to reconnect with the new settings.
uint32_t settingsVersion();

enum class State : uint8_t { Disabled, Connecting, Connected, Error };
struct Status { State state = State::Disabled; String error; };
// Written by the client (its own task), read by the panel; thread-safe.
void setStatus(State state, const char* error = "");
Status status();
const char* stateName(State state);

// Tries `settings` with a separate, one-off client (its own id, so the running link stays up) and
// reports whether the broker accepted it, within a few seconds. Blocks the caller meanwhile: call it
// from an HTTP route without the state lock. One test at a time.
struct TestResult { bool ok = false; String message; uint32_t elapsedMs = 0; };
TestResult test(const Settings& settings);

}  // namespace whisli::net::mqtt
