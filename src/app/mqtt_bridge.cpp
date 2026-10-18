#include "whisli/app/mqtt_bridge.hpp"

#include <ETH.h>
#include <WiFi.h>

#include <cmath>
#include <vector>

#include "whisli/net/json.hpp"
#include "whisli/system/log.hpp"
#include "whisli/system/state_lock.hpp"
#include "whisli/system/task_priorities.hpp"
#include "whisli/version.generated.hpp"

namespace whisli::app {
namespace {

using whisli::net::jsonEscape;
namespace mqtt = whisli::net::mqtt;

constexpr uint32_t kStatesEveryMs = 200;      // lights changed by buttons show up in HA this quickly
constexpr uint32_t kDiscoveryEveryMs = 3000;  // renamed or added lights and buttons
constexpr uint16_t kWarmKelvin = 2700;

// FNV-1a: what was published, without keeping the text. Never 0, which means "nothing sent".
uint32_t textHash(const String& text) {
  uint32_t hash = 2166136261u;
  for (const char character : text) hash = (hash ^ static_cast<uint8_t>(character)) * 16777619u;
  return hash ? hash : 1;
}
constexpr uint16_t kCoolKelvin = 6500;

struct Gesture { const char* trigger; const char* type; };
// Home Assistant's device trigger types for each gesture of ours.
constexpr Gesture kGestures[] = {
  {"click", "button_short_press"}, {"doubleClick", "button_double_press"}, {"tripleClick", "button_triple_press"},
  {"longClick", "button_long_release"}, {"hold", "button_long_press"},
};

// CCT lights mix warm and cool linearly in mireds, like the panel and the presets.
uint16_t mixToKelvin(uint8_t mix) {
  const float warm = 1e6f / kWarmKelvin, cool = 1e6f / kCoolKelvin;
  return static_cast<uint16_t>(lroundf(1e6f / (warm + (cool - warm) * mix / 100.0f)));
}
uint8_t kelvinToMix(long kelvin) {
  const float warm = 1e6f / kWarmKelvin, cool = 1e6f / kCoolKelvin;
  const float mix = (warm - 1e6f / constrain(kelvin, static_cast<long>(kWarmKelvin), static_cast<long>(kCoolKelvin))) / (warm - cool) * 100.0f;
  return static_cast<uint8_t>(constrain(lroundf(mix), 0L, 100L));
}

// Small readers for Home Assistant's command JSON: {"state":"ON","brightness":128,"color":{"r":…}}.
bool jsonNumber(const String& json, const char* key, long& output) {
  const int at = json.indexOf(String("\"") + key + "\"");
  if (at < 0) return false;
  const int colon = json.indexOf(':', at);
  if (colon < 0) return false;
  int start = colon + 1;
  while (start < static_cast<int>(json.length()) && json[start] == ' ') ++start;
  if (start >= static_cast<int>(json.length()) || !(isDigit(json[start]) || json[start] == '-')) return false;
  output = json.substring(start).toInt();
  return true;
}
String jsonText(const String& json, const char* key) {
  const int at = json.indexOf(String("\"") + key + "\"");
  if (at < 0) return String();
  const int open = json.indexOf('"', json.indexOf(':', at));
  const int close = open < 0 ? -1 : json.indexOf('"', open + 1);
  return close < 0 ? String() : json.substring(open + 1, close);
}

bool isDmx(LightKind kind) { return kind == LightKind::DmxDimmable || kind == LightKind::DmxCct || kind == LightKind::DmxRgb || kind == LightKind::DmxRgbw; }

}  // namespace

void MqttBridge::poll() {
  if (!commands_) {
    commands_ = xQueueCreate(8, sizeof(Command));
    outgoing_ = xQueueCreate(64, sizeof(Outgoing*));  // rounds queue at most kMessagesPerRound
    clientLock_ = xSemaphoreCreateMutex();
    settingsLock_ = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(workerTask, "whisli-mqtt", 4096, this, whisli::system::kMqttTaskPriority, nullptr, 0);
    return;
  }
  // The worker is restarting the client: skip this round rather than wait.
  if (xSemaphoreTake(settingsLock_, 0) != pdTRUE) return;
  Command command;
  if (client_) while (xQueueReceive(commands_, &command, 0) == pdTRUE) applyCommand(command);
  if (client_ && connected_) {
    const uint32_t now = millis();
    if (resync_) { resync_ = false; publishAll(); discoveryPending_ = true; }
    else if (settings_.discovery && (discoveryPending_ || now - lastDiscoveryCheckAt_ >= kDiscoveryEveryMs)) {
      // Configs first; a backlog continues on the next poll instead of waiting the full interval.
      lastDiscoveryCheckAt_ = now;
      discoveryPending_ = !publishDiscovery();
    }
    else if (now - lastStatesAt_ >= kStatesEveryMs) { lastStatesAt_ = now; publishStates(false); }
  }
  xSemaphoreGive(settingsLock_);
}

void MqttBridge::workerTask(void* context) {
  auto* bridge = static_cast<MqttBridge*>(context);
  for (;;) {
    bridge->restart();
    Outgoing* message = nullptr;
    if (xQueueReceive(bridge->outgoing_, &message, pdMS_TO_TICKS(500)) != pdTRUE) continue;
    xSemaphoreTake(bridge->clientLock_, portMAX_DELAY);
    if (bridge->client_ && bridge->connected_) {
      esp_mqtt_client_publish(bridge->client_, message->topic.c_str(), message->payload.c_str(), message->payload.length(), 0, message->retain ? 1 : 0);
    }
    xSemaphoreGive(bridge->clientLock_);
    delete message;
  }
}

// New settings (saved in the panel): stop the client and start again with them; also starts it once
// the network is up. Stopping can take seconds with a stalled broker, which is why it lives here.
void MqttBridge::restart() {
  const bool changed = settingsVersion_ != mqtt::settingsVersion();
  if (!changed && (client_ || !settings_.enabled || settings_.host.isEmpty())) return;
  if (!changed && !ETH.hasIP() && WiFi.status() != WL_CONNECTED) return;
  xSemaphoreTake(settingsLock_, portMAX_DELAY);
  if (changed) {
    settingsVersion_ = mqtt::settingsVersion();
    const mqtt::Settings next = mqtt::load();
    // Moving to other topics or another broker: take our retained messages off the old place first,
    // so no stale states or "offline" stay behind there.
    stop(next.baseTopic != settings_.baseTopic || next.host != settings_.host || next.port != settings_.port);
    settings_ = next;
    if (!settings_.enabled || settings_.host.isEmpty()) mqtt::setStatus(mqtt::State::Disabled);
  }
  if (!client_ && settings_.enabled && !settings_.host.isEmpty() && (ETH.hasIP() || WiFi.status() == WL_CONNECTED)) start();
  xSemaphoreGive(settingsLock_);
  Command stale;
  while (changed && xQueueReceive(commands_, &stale, 0) == pdTRUE) {}
}

void MqttBridge::start() {
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  mac.toLowerCase();
  nodeId_ = "whisli_" + mac;
  clientId_ = nodeId_;
  availability_ = settings_.baseTopic + "/status";

  esp_mqtt_client_config_t config = {};
  config.broker.address.hostname = settings_.host.c_str();
  config.broker.address.port = settings_.port;
  config.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
  config.credentials.client_id = clientId_.c_str();
  if (!settings_.user.isEmpty()) config.credentials.username = settings_.user.c_str();
  if (!settings_.password.isEmpty()) config.credentials.authentication.password = settings_.password.c_str();
  config.session.last_will.topic = availability_.c_str();
  config.session.last_will.msg = "offline";
  config.session.last_will.qos = 1;
  config.session.last_will.retain = 1;
  config.session.keepalive = 30;
  config.network.reconnect_timeout_ms = 5000;
  config.network.timeout_ms = 5000;
  config.task.priority = whisli::system::kMqttTaskPriority;
  config.task.stack_size = 6144;
  config.buffer.size = 2048;  // discovery configs carry the whole device description
  client_ = esp_mqtt_client_init(&config);
  if (!client_) { mqtt::setStatus(mqtt::State::Error, "Nie udało się uruchomić klienta MQTT."); return; }
  esp_mqtt_client_register_event(client_, MQTT_EVENT_ANY, &MqttBridge::onEvent, this);
  esp_mqtt_client_start(client_);
  mqtt::setStatus(mqtt::State::Connecting);
  whisli::system::write(whisli::system::LogLevel::Info, "network", ("MQTT connecting to " + settings_.host).c_str());
}

void MqttBridge::stop(bool forget) {
  if (!client_) return;
  xSemaphoreTake(clientLock_, portMAX_DELAY);
  if (connected_ && forget) {
    for (uint8_t slot = 0; slot < kMaxLights; ++slot) {
      if (!lightStates_[slot]) continue;
      const String topic = settings_.baseTopic + "/light/" + String(slot + 1) + "/state";
      esp_mqtt_client_publish(client_, topic.c_str(), "", 0, 1, 1);  // empty retained: removed from the broker
    }
    esp_mqtt_client_publish(client_, availability_.c_str(), "", 0, 1, 1);
    whisli::system::write(whisli::system::LogLevel::Info, "network", "MQTT: old topics cleared");
  } else if (connected_) {
    esp_mqtt_client_publish(client_, availability_.c_str(), "offline", 0, 1, 1);
  }
  esp_mqtt_client_stop(client_);
  esp_mqtt_client_destroy(client_);
  client_ = nullptr;
  connected_ = false;
  xSemaphoreGive(clientLock_);
  Outgoing* stale = nullptr;
  while (xQueueReceive(outgoing_, &stale, 0) == pdTRUE) delete stale;
  memset(lightStates_, 0, sizeof(lightStates_));
  memset(lightDiscovery_, 0, sizeof(lightDiscovery_));
  for (auto& config : inputDiscovery_) config = String();
}

void MqttBridge::onEvent(void* context, esp_event_base_t, int32_t, void* data) {
  static_cast<MqttBridge*>(context)->handleEvent(static_cast<esp_mqtt_event_handle_t>(data));
}

// Runs in the esp-mqtt task: only flags, status and queueing here; the rest happens in loop().
void MqttBridge::handleEvent(esp_mqtt_event_handle_t event) {
  switch (event->event_id) {
    case MQTT_EVENT_CONNECTED: {
      connected_ = true;
      resync_ = true;
      mqtt::setStatus(mqtt::State::Connected);
      esp_mqtt_client_subscribe(client_, (settings_.baseTopic + "/light/+/set").c_str(), 0);
      if (settings_.discovery) esp_mqtt_client_subscribe(client_, (settings_.discoveryPrefix + "/status").c_str(), 0);
      whisli::system::write(whisli::system::LogLevel::Info, "network", "MQTT connected");
      break;
    }
    case MQTT_EVENT_DISCONNECTED:
      if (connected_) whisli::system::write(whisli::system::LogLevel::Warning, "network", "MQTT disconnected");
      connected_ = false;
      if (mqtt::status().state != mqtt::State::Error) mqtt::setStatus(mqtt::State::Connecting);
      break;
    case MQTT_EVENT_ERROR: {
      const esp_mqtt_error_codes_t* error = event->error_handle;
      const char* message = "Błąd połączenia z brokerem.";
      if (error && error->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
        switch (error->connect_return_code) {
          case MQTT_CONNECTION_REFUSE_BAD_USERNAME:
          case MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED: message = "Broker odrzucił użytkownika lub hasło."; break;
          case MQTT_CONNECTION_REFUSE_ID_REJECTED: message = "Broker odrzucił identyfikator klienta."; break;
          default: message = "Broker odrzucił połączenie."; break;
        }
      } else if (error && error->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
        message = "Brak połączenia z serwerem — sprawdź adres, port i sieć.";
      }
      mqtt::setStatus(mqtt::State::Error, message);
      break;
    }
    case MQTT_EVENT_DATA: {
      // Commands are short; anything split across several events is not one of ours.
      if (event->current_data_offset != 0 || event->total_data_len != event->data_len) break;
      Command command = {};
      memcpy(command.topic, event->topic, min<size_t>(event->topic_len, sizeof(command.topic) - 1));
      memcpy(command.payload, event->data, min<size_t>(event->data_len, sizeof(command.payload) - 1));
      xQueueSend(commands_, &command, 0);
      break;
    }
    default: break;
  }
}

void MqttBridge::applyCommand(const Command& command) {
  const String topic(command.topic);
  const String payload(command.payload);
  // Home Assistant came back: it forgot the discovery configs and states, send them again.
  if (topic == settings_.discoveryPrefix + "/status") { if (payload == "online") resync_ = true; return; }
  const String prefix = settings_.baseTopic + "/light/";
  if (!topic.startsWith(prefix) || !topic.endsWith("/set")) return;
  const long id = topic.substring(prefix.length(), topic.length() - 4).toInt();
  if (id < 1 || id > 255) return;

  whisli::system::StateLock lock;
  LightSnapshot light;
  if (!lights_.find(static_cast<uint8_t>(id), light) || light.kind == LightKind::None) return;
  const String state = jsonText(payload, "state");
  if (state == "OFF") { lights_.setLevel(light.id, 0); return; }
  if (light.kind == LightKind::RelaySwitch) { if (state == "ON") lights_.turnOn(light.id); return; }

  long brightness = -1, kelvin = -1, red = -1, green = -1, blue = -1, white = -1;
  jsonNumber(payload, "brightness", brightness);
  jsonNumber(payload, "color_temp", kelvin);
  const int colorAt = payload.indexOf("\"color\"");
  if (colorAt >= 0) {
    const String color = payload.substring(colorAt, payload.indexOf('}', colorAt) + 1);
    jsonNumber(color, "r", red); jsonNumber(color, "g", green); jsonNumber(color, "b", blue); jsonNumber(color, "w", white);
  }
  // "ON" alone comes back at the light's on-level, like a button would.
  if (brightness < 0 && light.level == 0) { lights_.turnOn(light.id); lights_.find(light.id, light); }
  const uint8_t level = brightness >= 0 ? static_cast<uint8_t>(constrain(brightness, 0L, 255L)) : light.level;
  const int temperature = light.kind == LightKind::DmxCct && kelvin > 0 ? kelvinToMix(kelvin) : -1;
  RgbwColor color = lights_.color(light.id);
  const bool hasColor = (light.kind == LightKind::DmxRgb || light.kind == LightKind::DmxRgbw) && red >= 0 && green >= 0 && blue >= 0;
  if (hasColor) {
    color.red = static_cast<uint8_t>(constrain(red, 0L, 255L));
    color.green = static_cast<uint8_t>(constrain(green, 0L, 255L));
    color.blue = static_cast<uint8_t>(constrain(blue, 0L, 255L));
    if (white >= 0) color.white = static_cast<uint8_t>(constrain(white, 0L, 255L));
  }
  lights_.setLevel(light.id, level, temperature, hasColor ? &color : nullptr);
}

// Handed to the worker: loop() never waits for the network. False when the queue is full.
bool MqttBridge::publish(const String& topic, const String& payload, bool retain) {
  auto* message = new Outgoing{topic, payload, retain};
  if (xQueueSend(outgoing_, &message, 0) == pdTRUE) return true;
  delete message;
  return false;
}

// Everything again (connected, or Home Assistant restarted): forgetting what was sent makes the
// next rounds send it all, a portion at a time.
void MqttBridge::publishAll() {
  publish(availability_, "online", true);
  memset(lightStates_, 0, sizeof(lightStates_));
  memset(lightDiscovery_, 0, sizeof(lightDiscovery_));
  for (auto& config : inputDiscovery_) config = String("\x01");
}

String MqttBridge::deviceJson() const {
  const String ip = ETH.hasIP() ? ETH.localIP().toString() : WiFi.localIP().toString();
  return "{\"identifiers\":[\"" + nodeId_ + "\"],\"name\":\"" + jsonEscape(settings_.deviceName.isEmpty() ? String(controller_.snapshot().name) : settings_.deviceName) +
         "\",\"manufacturer\":\"Whisli\",\"model\":\"Waveshare ESP32-S3-POE-ETH-8DI-8DO\",\"sw_version\":\"" +
         String(kFirmwareVersion) + "\",\"configuration_url\":\"http://" + ip + "/\"}";
}

String MqttBridge::lightStateJson(const LightSnapshot& light) const {
  String json = String("{\"state\":\"") + (light.level > 0 ? "ON" : "OFF") + "\"";
  if (!isDmx(light.kind)) return json + "}";
  if (light.level > 0) json += ",\"brightness\":" + String(light.level);
  if (light.kind == LightKind::DmxCct) {
    json += ",\"color_mode\":\"color_temp\",\"color_temp\":" + String(mixToKelvin(lights_.colorTemperature(light.id)));
  } else if (light.kind == LightKind::DmxRgb || light.kind == LightKind::DmxRgbw) {
    const RgbwColor color = lights_.color(light.id);
    const bool rgbw = light.kind == LightKind::DmxRgbw;
    json += String(",\"color_mode\":\"") + (rgbw ? "rgbw" : "rgb") + "\",\"color\":{\"r\":" + color.red + ",\"g\":" + color.green + ",\"b\":" + color.blue +
            (rgbw ? String(",\"w\":") + color.white : String()) + "}";
  } else {
    json += ",\"color_mode\":\"brightness\"";
  }
  return json + "}";
}

// How many messages one round may queue: keeps memory flat with many lights. What does not fit
// stays different from what was sent and goes out in the next rounds.
uint16_t MqttBridge::budget() const {
  const UBaseType_t spaces = uxQueueSpacesAvailable(outgoing_);
  return spaces > kMessagesPerRound ? kMessagesPerRound : static_cast<uint16_t>(spaces);
}

void MqttBridge::publishStates(bool force) {
  struct Pending { String topic; String payload; uint8_t slot; };
  std::vector<Pending> pending;
  const uint16_t room = budget();
  {
    // Copied under the lock, published after it.
    whisli::system::StateLock lock;
    for (uint8_t index = 0; index < lights_.count() && pending.size() < room; ++index) {
      LightSnapshot light;
      if (!lights_.get(index, light) || light.kind == LightKind::None || light.id < 1 || light.id > kMaxLights) continue;
      const String json = lightStateJson(light);
      uint32_t& last = lightStates_[light.id - 1];
      const uint32_t hash = textHash(json);
      if (!force && hash == last) continue;
      last = hash;
      pending.push_back({settings_.baseTopic + "/light/" + light.id + "/state", json, static_cast<uint8_t>(light.id - 1)});
    }
  }
  // Not queued: forget it was sent, so the next round tries again.
  for (const Pending& item : pending) if (!publish(item.topic, item.payload, true)) lightStates_[item.slot] = 0;
}

// Configs are compared with what was sent last: a renamed light updates, a removed one is withdrawn
// (an empty retained config deletes it in Home Assistant). True when everything is up to date.
bool MqttBridge::publishDiscovery() {
  struct Pending { String topic; String payload; int lightSlot; int input; };
  std::vector<Pending> pending;
  const uint16_t room = budget();
  const String device = deviceJson();
  {
    whisli::system::StateLock lock;
    bool present[kMaxLights] = {};
    for (uint8_t index = 0; index < lights_.count(); ++index) {
      LightSnapshot light;
      if (!lights_.get(index, light) || light.kind == LightKind::None || light.id < 1 || light.id > kMaxLights) continue;
      present[light.id - 1] = true;
      if (pending.size() >= room) continue;
      const String id = String(light.id);
      String modes;
      switch (light.kind) {
        case LightKind::RelaySwitch: modes = "[\"onoff\"]"; break;
        case LightKind::DmxCct: modes = "[\"color_temp\"],\"color_temp_kelvin\":true,\"min_kelvin\":" + String(kWarmKelvin) + ",\"max_kelvin\":" + String(kCoolKelvin); break;
        case LightKind::DmxRgb: modes = "[\"rgb\"]"; break;
        case LightKind::DmxRgbw: modes = "[\"rgbw\"]"; break;
        default: modes = "[\"brightness\"]"; break;
      }
      const String config = "{\"name\":\"" + jsonEscape(light.name) + "\",\"unique_id\":\"" + nodeId_ + "_light_" + id +
                            "\",\"schema\":\"json\",\"command_topic\":\"" + settings_.baseTopic + "/light/" + id + "/set\",\"state_topic\":\"" +
                            settings_.baseTopic + "/light/" + id + "/state\",\"availability_topic\":\"" + availability_ +
                            "\",\"brightness\":" + (isDmx(light.kind) ? "true" : "false") + ",\"supported_color_modes\":" + modes +
                            ",\"device\":" + device + "}";
      uint32_t& last = lightDiscovery_[light.id - 1];
      const uint32_t hash = textHash(config);
      if (hash == last) continue;
      last = hash;
      pending.push_back({settings_.discoveryPrefix + "/light/" + nodeId_ + "/light_" + id + "/config", config, light.id - 1, -1});
    }
    for (uint8_t slot = 0; slot < kMaxLights && pending.size() < room; ++slot) {
      if (present[slot] || lightDiscovery_[slot] == 0) continue;
      lightDiscovery_[slot] = 0;
      pending.push_back({settings_.discoveryPrefix + "/light/" + nodeId_ + "/light_" + String(slot + 1) + "/config", String(), -1, -1});
    }
  }
  // Buttons: one device trigger per gesture of every used input (for automations), plus an event
  // entity per input (visible among the entities, with the last gesture and its history). Both read
  // the same action topic; the entity turns the plain gesture into the JSON it expects.
  const ControllerDevice controller = controller_.snapshot();
  String eventTypes = "[";
  for (const Gesture& gesture : kGestures) eventTypes += String(eventTypes.length() > 1 ? "," : "") + "\"" + gesture.trigger + "\"";
  eventTypes += "]";
  for (uint8_t input = 0; input < kControllerInputCount; ++input) {
    String name = controller.inputNames[input];
    if (name.isEmpty()) name = "DI" + String(input < 9 ? "0" : "") + String(input + 1);
    const String signature = controller.inputUsed[input] ? name : String();
    if (signature == inputDiscovery_[input]) continue;
    const size_t gestures = sizeof(kGestures) / sizeof(kGestures[0]);
    if (pending.size() + gestures + 1 > room) break;
    inputDiscovery_[input] = signature;
    const String actionTopic = settings_.baseTopic + "/input/" + String(input + 1) + "/action";
    pending.push_back({settings_.discoveryPrefix + "/event/" + nodeId_ + "/input_" + String(input + 1) + "/config",
                       signature.isEmpty() ? String() :
                       "{\"name\":\"" + jsonEscape(name) + "\",\"unique_id\":\"" + nodeId_ + "_input_" + String(input + 1) +
                       "\",\"state_topic\":\"" + actionTopic + "\",\"value_template\":\"{\\\"event_type\\\":\\\"{{ value }}\\\"}\"" +
                       ",\"event_types\":" + eventTypes + ",\"device_class\":\"button\",\"availability_topic\":\"" + availability_ +
                       "\",\"device\":" + device + "}", -1, input});
    for (const Gesture& gesture : kGestures) {
      const String topic = settings_.discoveryPrefix + "/device_automation/" + nodeId_ + "/input_" + String(input + 1) + "_" + gesture.trigger + "/config";
      const String config = signature.isEmpty() ? String() :
          "{\"automation_type\":\"trigger\",\"topic\":\"" + actionTopic + "\",\"type\":\"" +
          gesture.type + "\",\"subtype\":\"" + jsonEscape(name) + "\",\"payload\":\"" + gesture.trigger + "\",\"device\":" + device + "}";
      pending.push_back({topic, config, -1, input});
    }
  }
  // Not queued: forget it was sent, so it goes again.
  for (const Pending& item : pending) {
    if (publish(item.topic, item.payload, true)) continue;
    if (item.lightSlot >= 0) lightDiscovery_[item.lightSlot] = 0;
    if (item.input >= 0) inputDiscovery_[item.input] = String("\x01");
  }
  return pending.size() < room;
}

void MqttBridge::publishGesture(uint8_t input, const char* trigger) {
  if (!settingsLock_ || !client_ || !connected_ || input >= kControllerInputCount) return;
  if (xSemaphoreTake(settingsLock_, 0) != pdTRUE) return;
  for (const Gesture& gesture : kGestures) {
    if (strcmp(gesture.trigger, trigger) != 0) continue;
    publish(settings_.baseTopic + "/input/" + String(input + 1) + "/action", trigger, false);
    break;
  }
  xSemaphoreGive(settingsLock_);
}

}  // namespace whisli::app
