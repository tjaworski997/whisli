#include "whisli/net/mqtt_settings.hpp"

#include <Preferences.h>
#include <esp_random.h>
#include <freertos/event_groups.h>
#include <mqtt_client.h>

#include "whisli/system/task_priorities.hpp"

namespace whisli::net::mqtt {
namespace {
constexpr char kNamespace[] = "whisli-mqtt";
volatile uint32_t version = 1;
portMUX_TYPE statusMux = portMUX_INITIALIZER_UNLOCKED;
State currentState = State::Disabled;
char currentError[96] = {};

// Shared by the test client's event handler and test(); lives until the client is destroyed.
struct TestContext {
  EventGroupHandle_t events = nullptr;
  const char* error = nullptr;
};
constexpr EventBits_t kTestConnected = 1 << 0;
constexpr EventBits_t kTestFailed = 1 << 1;
constexpr uint32_t kTestTimeoutMs = 6000;
volatile bool testRunning = false;
portMUX_TYPE testMux = portMUX_INITIALIZER_UNLOCKED;

void onTestEvent(void* context, esp_event_base_t, int32_t id, void* data) {
  auto* test = static_cast<TestContext*>(context);
  const auto event = static_cast<esp_mqtt_event_handle_t>(data);
  if (id == MQTT_EVENT_CONNECTED) { xEventGroupSetBits(test->events, kTestConnected); return; }
  if (id != MQTT_EVENT_ERROR) return;
  const esp_mqtt_error_codes_t* error = event->error_handle;
  test->error = "Błąd połączenia z brokerem.";
  if (error && error->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
    switch (error->connect_return_code) {
      case MQTT_CONNECTION_REFUSE_BAD_USERNAME:
      case MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED: test->error = "Broker odrzucił użytkownika lub hasło."; break;
      case MQTT_CONNECTION_REFUSE_ID_REJECTED: test->error = "Broker odrzucił identyfikator klienta."; break;
      case MQTT_CONNECTION_REFUSE_SERVER_UNAVAILABLE: test->error = "Broker jest niedostępny."; break;
      default: test->error = "Broker odrzucił połączenie."; break;
    }
  } else if (error && error->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
    test->error = "Brak połączenia z serwerem — sprawdź adres, port i sieć.";
  }
  xEventGroupSetBits(test->events, kTestFailed);
}

}  // namespace

TestResult test(const Settings& settings) {
  TestResult result;
  portENTER_CRITICAL(&testMux);
  const bool busy = testRunning;
  testRunning = true;
  portEXIT_CRITICAL(&testMux);
  if (busy) { result.message = "Test połączenia już trwa."; return result; }

  TestContext context;
  context.events = xEventGroupCreate();
  char clientId[24];
  snprintf(clientId, sizeof(clientId), "whisli-test-%08lx", static_cast<unsigned long>(esp_random()));
  esp_mqtt_client_config_t config = {};
  config.broker.address.hostname = settings.host.c_str();
  config.broker.address.port = settings.port;
  config.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
  config.credentials.client_id = clientId;
  if (!settings.user.isEmpty()) config.credentials.username = settings.user.c_str();
  if (!settings.password.isEmpty()) config.credentials.authentication.password = settings.password.c_str();
  config.network.timeout_ms = kTestTimeoutMs;
  config.network.disable_auto_reconnect = true;
  config.network.reconnect_timeout_ms = 100;  // the client task idles this long after a failure; stop() waits for it
  config.task.priority = whisli::system::kMqttTaskPriority;
  config.task.stack_size = 4096;
  config.buffer.size = 512;
  esp_mqtt_client_handle_t client = context.events ? esp_mqtt_client_init(&config) : nullptr;
  if (!client) {
    result.message = "Nie udało się uruchomić klienta MQTT.";
  } else {
    esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, onTestEvent, &context);
    const uint32_t startedAt = millis();
    esp_mqtt_client_start(client);
    const EventBits_t bits = xEventGroupWaitBits(context.events, kTestConnected | kTestFailed, pdFALSE, pdFALSE, pdMS_TO_TICKS(kTestTimeoutMs + 1000));
    result.elapsedMs = millis() - startedAt;
    result.ok = bits & kTestConnected;
    if (result.ok) result.message = "Połączono z brokerem.";
    else result.message = context.error ? context.error : "Broker nie odpowiedział w ciągu kilku sekund.";
    esp_mqtt_client_stop(client);
    esp_mqtt_client_destroy(client);
  }
  if (context.events) vEventGroupDelete(context.events);
  testRunning = false;
  return result;
}

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
  settings.deviceName = preferences.getString("device", settings.deviceName);
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
                  preferences.putBool("disc", settings.discovery) && text("discPrefix", settings.discoveryPrefix) &&
                  (settings.deviceName.isEmpty() ? (preferences.remove("device"), true) : text("device", settings.deviceName));
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
