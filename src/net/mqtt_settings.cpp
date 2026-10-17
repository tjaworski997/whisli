#include "whisli/net/mqtt_settings.hpp"

#include <Preferences.h>

namespace whisli::net::mqtt {
namespace {
constexpr char kNamespace[] = "whisli-mqtt";
volatile uint32_t version = 1;
portMUX_TYPE statusMux = portMUX_INITIALIZER_UNLOCKED;
State currentState = State::Disabled;
char currentError[96] = {};
}  // namespace

uint32_t settingsVersion() { return version; }

void setStatus(State state, const char* error) {
  portENTER_CRITICAL(&statusMux);
  currentState = state;
  strlcpy(currentError, error ? error : "", sizeof(currentError));
  portEXIT_CRITICAL(&statusMux);
}

Status status() {
  char error[sizeof(currentError)];
  portENTER_CRITICAL(&statusMux);
  const State state = currentState;
  memcpy(error, currentError, sizeof(error));
  portEXIT_CRITICAL(&statusMux);
  return Status{state, String(error)};
}

const char* stateName(State state) {
  switch (state) {
    case State::Connecting: return "connecting";
    case State::Connected: return "connected";
    case State::Error: return "error";
    default: return "disabled";
  }
}

Settings load() {
  Settings settings;
  Preferences preferences;
  preferences.begin(kNamespace, true);
  settings.enabled = preferences.getBool("enabled", settings.enabled);
  settings.host = preferences.getString("host", settings.host);
  settings.port = preferences.getUShort("port", settings.port);
  settings.user = preferences.getString("user", settings.user);
  settings.password = preferences.getString("pass", settings.password);
  settings.baseTopic = preferences.getString("topic", settings.baseTopic);
  settings.discovery = preferences.getBool("disc", settings.discovery);
  settings.discoveryPrefix = preferences.getString("discPrefix", settings.discoveryPrefix);
  preferences.end();
  return settings;
}

bool save(const Settings& settings) {
  Preferences preferences;
  preferences.begin(kNamespace, false);
  // Strings are only rewritten when they change: the settings storage is small.
  const auto text = [&](const char* key, const String& value) {
    return preferences.getString(key, "") == value || preferences.putString(key, value) == value.length() || value.isEmpty();
  };
  const bool ok = preferences.putBool("enabled", settings.enabled) && text("host", settings.host) &&
                  preferences.putUShort("port", settings.port) && text("user", settings.user) &&
                  text("pass", settings.password) && text("topic", settings.baseTopic) &&
                  preferences.putBool("disc", settings.discovery) && text("discPrefix", settings.discoveryPrefix);
  preferences.end();
  ++version;
  return ok;
}

bool validTopic(const String& topic) {
  if (topic.isEmpty() || topic.length() > 64 || topic.startsWith("/") || topic.endsWith("/") || topic.indexOf("//") >= 0) return false;
  for (const char character : topic) if (character == '#' || character == '+' || character < 0x20) return false;
  return true;
}

}  // namespace whisli::net::mqtt
