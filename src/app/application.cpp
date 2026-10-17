#include "whisli/app/application.hpp"

#include <Arduino.h>
#include <ETH.h>
#include <WiFi.h>
#include <nvs.h>

#include "embedded_ui.hpp"
#include "whisli/app/chaos_mode.hpp"
#include "whisli/app/controller_io.hpp"
#include "whisli/app/dmx_output.hpp"
#include "whisli/app/input_actions.hpp"
#include "whisli/app/lights.hpp"
#include "whisli/app/modbus_relays.hpp"
#include "whisli/app/mqtt_bridge.hpp"
#include "whisli/app/presets.hpp"
#include "whisli/app/state_events.hpp"
#include "whisli/app/zones.hpp"
#include "whisli/bootstrap/bootstrap_portal.hpp"
#include "whisli/net/http_server.hpp"
#include "whisli/net/json.hpp"
#include "whisli/system/log.hpp"
#include "whisli/system/state_lock.hpp"
#include "whisli/system/task_priorities.hpp"
#include "whisli/version.generated.hpp"

namespace whisli::app {
namespace {

using whisli::net::jsonEscape;
using whisli::net::Method;
using whisli::net::Request;
using whisli::net::StateAccess;

ModbusRelayManager relays;
ControllerIo controllerIo;
DmxOutput dmx;
LightManager lights(controllerIo, relays, dmx);
ZoneManager zones;
PresetManager presets(lights);
InputActionManager inputActions(controllerIo, relays, lights, presets);
ChaosMode chaos(controllerIo, relays, lights);
MqttBridge mqttBridge(lights, controllerIo);

const char* lightKindName(LightKind kind) {
  switch (kind) {
    case LightKind::RelaySwitch: return "relay";
    case LightKind::DmxDimmable: return "dmx";
    case LightKind::DmxCct: return "cct";
    case LightKind::DmxRgb: return "rgb";
    case LightKind::DmxRgbw: return "rgbw";
    default: return "none";
  }
}

// Change notifications go out over the WebSocket at most this often; a heartbeat keeps idle
// connections verifiably alive so the panel can tell a quiet controller from a dead link.
constexpr unsigned long kBroadcastIntervalMs = 50;
constexpr unsigned long kHeartbeatIntervalMs = 20000;
unsigned long lastBroadcastAt = 0;

// Buttons and lights: a fixed-rate task above everything else. ControllerIo debounces on its own
// 10 ms grid; sampling twice as often keeps the reaction within one grid step.
constexpr uint32_t kControlPeriodMs = 5;
// Worst cases since boot, shown in /api/v1/system: how late a cycle started and how long one took.
// They prove the priority rule: panel traffic or OTA must not push these up.
volatile uint32_t controlMaxLateUs = 0;
volatile uint32_t controlMaxBusyUs = 0;

void controlTask(void*) {
  TickType_t nextWakeAt = xTaskGetTickCount();
  int64_t expectedAt = esp_timer_get_time();
  for (;;) {
    const int64_t startedAt = esp_timer_get_time();
    const uint32_t late = startedAt > expectedAt ? static_cast<uint32_t>(startedAt - expectedAt) : 0;
    if (late > controlMaxLateUs) controlMaxLateUs = late;
    {
      whisli::system::StateLock lock;
      controllerIo.poll();  // reads inputs and runs their actions (lights, outputs, relay commands)
      lights.tick();        // advances fades
    }
    const uint32_t busy = static_cast<uint32_t>(esp_timer_get_time() - startedAt);
    if (busy > controlMaxBusyUs) controlMaxBusyUs = busy;
    vTaskDelayUntil(&nextWakeAt, pdMS_TO_TICKS(kControlPeriodMs));
    expectedAt += kControlPeriodMs * 1000;
    if (esp_timer_get_time() - expectedAt > 1000000) expectedAt = esp_timer_get_time();  // resync after a long stall
  }
}

void handleInputEvent(void* context, uint8_t input, ButtonEvent event) {
  static_cast<InputActionManager*>(context)->handleInputEvent(input, event);
  notifyChange(kTopicDevices | kTopicActions);  // the same gesture twice is still news for the panel
  // Repeated hold ticks would flood the map; its start is enough to show the gesture (release shows its end).
  if (event != ButtonEvent::Hold && event != ButtonEvent::HoldEnd) recordInputEvent(input, buttonEventName(event));
}

void broadcastChanges() {
  // Gestures go out right away: the map animates them as they happen.
  InputEventRecord record;
  while (takeInputEvent(record)) {
    mqttBridge.publishGesture(record.input, record.trigger);  // Home Assistant device triggers
    whisli::net::broadcast("{\"type\":\"input\",\"input\":\"controller.input-" + String(record.input + 1) +
                           "\",\"trigger\":\"" + record.trigger + "\"}");
  }
  const unsigned long now = millis();
  if (now - lastBroadcastAt < kBroadcastIntervalMs) return;
  uint32_t revision = 0;
  const uint8_t topics = takeChanges(revision);
  if (!topics && now - lastBroadcastAt < kHeartbeatIntervalMs) return;
  lastBroadcastAt = now;
  String message = "{\"type\":\"changed\",\"revision\":" + String(revision) + ",\"topics\":[";
  bool first = true;
  const auto add = [&](uint8_t topic, const char* name) {
    if (!(topics & topic)) return;
    if (!first) message += ',';
    first = false;
    message += '"';
    message += name;
    message += '"';
  };
  add(kTopicDevices, "devices");
  add(kTopicLights, "lights");
  add(kTopicActions, "actions");
  add(kTopicZones, "zones");
  message += "]}";
  whisli::net::broadcast(message);
}

void sendDevices(Request& request) {
  uint8_t deviceIds[kMaxRelayDevices];
  const uint8_t count = relays.deviceIds(deviceIds, kMaxRelayDevices);
  String body = "{\"devices\":[";
  const ControllerDevice controller = controllerIo.snapshot();
  body += "{\"id\":\"controller\",\"kind\":\"controller\",\"name\":\"" + jsonEscape(controller.name) +
      "\",\"profile\":\"waveshare_esp32_s3_poe_eth_8di_8do\",\"profileNameKey\":\"controllerBoard\",\"builtIn\":true,\"active\":true,\"online\":" +
      String(controller.online ? "true" : "false") + ",\"channelCount\":" + String(kControllerOutputCount) + ",\"channels\":[";
  for (uint8_t channel = 0; channel < kControllerOutputCount; ++channel) {
    if (channel) body += ',';
    body += controller.outputs[channel] ? "true" : "false";
  }
  body += "],\"channelUsed\":[";
  for (uint8_t channel = 0; channel < kControllerOutputCount; ++channel) {
    if (channel) body += ',';
    body += controller.outputUsed[channel] ? "true" : "false";
  }
  body += "],\"channelNames\":[";
  for (uint8_t channel = 0; channel < kControllerOutputCount; ++channel) {
    if (channel) body += ',';
    body += "\"" + jsonEscape(controller.outputNames[channel]) + "\"";
  }
  body += "],\"inputsHigh\":[";
  for (uint8_t channel = 0; channel < kControllerInputCount; ++channel) {
    if (channel) body += ',';
    body += controller.inputsHigh[channel] ? "true" : "false";
  }
  body += "],\"inputUsed\":[";
  for (uint8_t channel = 0; channel < kControllerInputCount; ++channel) {
    if (channel) body += ',';
    body += controller.inputUsed[channel] ? "true" : "false";
  }
  body += "],\"inputNames\":[";
  for (uint8_t channel = 0; channel < kControllerInputCount; ++channel) {
    if (channel) body += ',';
    body += "\"" + jsonEscape(controller.inputNames[channel]) + "\"";
  }
  body += "],\"inputLastEvents\":[";
  for (uint8_t channel = 0; channel < kControllerInputCount; ++channel) {
    if (channel) body += ',';
    body += "\"" + String(buttonEventName(controller.inputLastEvents[channel])) + "\"";
  }
  body += "]}";
  for (uint8_t index = 0; index < count; ++index) {
    body += ',';
    const RelaySnapshot relay = relays.snapshot(deviceIds[index]);
    const DeviceProfileDefinition* profile = findDeviceProfile(relay.profile);
    body += "{\"id\":\"relay-" + String(relay.id) + "\",\"kind\":\"relay\",\"builtIn\":false,\"name\":\"" + jsonEscape(relay.name) +
        "\",\"profile\":\"" + jsonEscape(relay.profile) + "\",\"profileNameKey\":\"" +
        (profile ? String(profile->nameKey) : String("relayProfileCustom")) +
        "\",\"address\":" + String(relay.address) + ",\"channelCount\":" + String(relay.channelCount) +
        ",\"coilStart\":" + String(relay.coilStart) + ",\"active\":" + String(relay.active ? "true" : "false") +
        ",\"online\":" + String(relay.online ? "true" : "false") + ",\"channels\":[";
    for (uint8_t channel = 0; channel < relay.channelCount; ++channel) {
      if (channel) body += ',';
      body += relay.channels[channel] ? "true" : "false";
    }
    body += "],\"channelUsed\":[";
    for (uint8_t channel = 0; channel < relay.channelCount; ++channel) {
      if (channel) body += ',';
      body += relay.channelUsed[channel] ? "true" : "false";
    }
    body += "],\"channelNames\":[";
    for (uint8_t channel = 0; channel < relay.channelCount; ++channel) {
      if (channel) body += ',';
      body += "\"" + jsonEscape(relay.channelNames[channel]) + "\"";
    }
    body += "]}";
  }
  body += "]}";
  request.sendJson(200, body);
}

void sendActionPanel(Request& request) {
  const ControllerDevice controller = controllerIo.snapshot();
  String body = "{\"inputs\":[";
  bool first = true;
  for (uint8_t channel = 0; channel < kControllerInputCount; ++channel) {
    if (!controller.inputUsed[channel]) continue;
    if (!first) body += ',';
    first = false;
    String name = controller.inputNames[channel];
    String hardwareName = "DI" + String(channel < 9 ? "0" : "") + String(channel + 1);
    if (name.isEmpty()) name = hardwareName;
    body += "{\"id\":\"controller.input-" + String(channel + 1) + "\",\"name\":\"" + jsonEscape(name.c_str()) +
        "\",\"hardwareName\":\"" + hardwareName +
        "\",\"high\":" + String(controller.inputsHigh[channel] ? "true" : "false") +
        ",\"lastEvent\":\"" + String(buttonEventName(controller.inputLastEvents[channel])) + "\"}";
  }
  // Targets: lights first, then relay outputs. `group` lets the UI split them.
  body += "],\"targets\":[";
  first = true;
  for (uint8_t index = 0; index < lights.count(); ++index) {
    LightSnapshot light;
    if (!lights.get(index, light) || light.kind == LightKind::None) continue;
    if (!first) body += ',';
    first = false;
    body += "{\"id\":\"light-" + String(light.id) + "\",\"group\":\"light\",\"name\":\"" + jsonEscape(light.name) +
        "\",\"state\":" + String(light.level > 0 ? "true" : "false") + ",\"online\":true,\"dimmable\":" +
        String(light.kind != LightKind::RelaySwitch ? "true" : "false") + ",\"kind\":\"" + lightKindName(light.kind) + "\"}";
  }
  for (uint8_t channel = 0; channel < kControllerOutputCount; ++channel) {
    if (!controller.outputUsed[channel]) continue;
    if (!first) body += ',';
    first = false;
    String name = controller.outputNames[channel];
    if (name.isEmpty()) name = "DO" + String(channel < 9 ? "0" : "") + String(channel + 1);
    body += "{\"id\":\"controller.output-" + String(channel + 1) + "\",\"group\":\"relay\",\"name\":\"" + jsonEscape(name.c_str()) +
        "\",\"state\":" + String(controller.outputs[channel] ? "true" : "false") +
        ",\"online\":" + String(controller.online ? "true" : "false") + "}";
  }
  uint8_t relayIds[kMaxRelayDevices];
  const uint8_t relayCount = relays.deviceIds(relayIds, kMaxRelayDevices);
  for (uint8_t index = 0; index < relayCount; ++index) {
    const RelaySnapshot relay = relays.snapshot(relayIds[index]);
    if (!relay.active) continue;
    for (uint8_t channel = 0; channel < relay.channelCount; ++channel) {
      if (!relay.channelUsed[channel]) continue;
      if (!first) body += ',';
      first = false;
      String name = relay.channelNames[channel];
      if (name.isEmpty()) name = "CH" + String(channel < 9 ? "0" : "") + String(channel + 1);
      body += "{\"id\":\"relay-" + String(relay.id) + ".output-" + String(channel + 1) +
          "\",\"group\":\"relay\",\"name\":\"" + jsonEscape((String(relay.name) + " · " + name).c_str()) +
          "\",\"state\":" + String(relay.channels[channel] ? "true" : "false") +
          ",\"online\":" + String(relay.online ? "true" : "false") + "}";
    }
  }
  body += "],\"events\":[";
  for (uint8_t index = 0; index < inputActions.eventCount(); ++index) {
    InputEventBinding event;
    if (!inputActions.getEvent(index, event)) continue;
    if (index) body += ',';
    body += "{\"id\":" + String(event.id) + ",\"inputId\":\"" + String(event.inputId) +
        "\",\"trigger\":\"" + String(InputActionManager::triggerName(event.trigger)) + "\"}";
  }
  body += "],\"actions\":[";
  for (uint8_t index = 0; index < inputActions.count(); ++index) {
    InputAction action;
    if (!inputActions.get(index, action)) continue;
    if (index) body += ',';
    body += "{\"id\":" + String(action.id) + ",\"inputId\":\"" + String(action.inputId) +
        "\",\"trigger\":\"" + String(InputActionManager::triggerName(action.trigger)) +
        "\",\"command\":\"" + String(InputActionManager::commandName(action.command)) +
        "\",\"targetId\":\"" + String(action.targetId) + "\",\"enabled\":" +
        String(action.enabled ? "true" : "false") + ",\"value\":" + String(action.value) + "}";
  }
  body += "],\"triggers\":[\"click\",\"doubleClick\",\"tripleClick\",\"longClick\",\"hold\"],\"commands\":[\"toggle\",\"on\",\"off\",\"setLevel\",\"stepUp\",\"stepDown\",\"preset\"]}";
  request.sendJson(200, body);
}

uint8_t requestedDeviceId(const Request& request) {
  const String id = request.arg("id");
  if (!id.startsWith("relay-")) return 0;
  const int value = id.substring(6).toInt();
  return value > 0 && value <= 255 ? static_cast<uint8_t>(value) : 0;
}

void sendPresets(Request& request) {
  String body = "{\"presets\":[";
  for (uint8_t index = 0; index < presets.count(); ++index) {
    const LightPreset& preset = presets.at(index);
    if (index) body += ',';
    body += "{\"id\":" + String(preset.id) + ",\"name\":\"" + jsonEscape(preset.name) + "\",\"kind\":\"" +
            (preset.kind == PresetKind::Temperature ? "temperature" : "color") + "\",\"kelvin\":" + String(preset.kelvin) +
            ",\"color\":{\"red\":" + String(preset.color.red) + ",\"green\":" + String(preset.color.green) +
            ",\"blue\":" + String(preset.color.blue) + ",\"white\":" + String(preset.color.white) + "}}";
  }
  request.sendJson(200, body + "]}");
}

void configurePresetRoutes() {
  using whisli::net::on;
  const auto presetId = [](const Request& request) { return static_cast<uint8_t>(request.arg("id").toInt()); };
  on("/api/v1/presets", Method::Get, sendPresets);
  // kind=temperature|color, name: the panel sends a default name to type over.
  on("/api/v1/presets", Method::Post, [](Request& request) {
    const String kind = request.arg("kind");
    if (kind != "temperature" && kind != "color") { request.sendJson(400, "{\"message\":\"Nieznany rodzaj presetu.\"}"); return; }
    const uint8_t id = presets.create(kind == "color" ? PresetKind::Color : PresetKind::Temperature, request.arg("name"));
    if (!id) { request.sendJson(400, "{\"message\":\"Nie udało się dodać presetu.\"}"); return; }
    request.sendJson(201, "{\"message\":\"Preset dodany.\",\"id\":" + String(id) + "}");
  });
  // Any of: name, kelvin (temperature), red/green/blue/white (colour).
  on("/api/v1/presets/update", Method::Post, [presetId](Request& request) {
    RgbwColor color;
    const bool hasColor = request.hasArg("red");
    if (hasColor) {
      color.red = request.arg("red").toInt(); color.green = request.arg("green").toInt();
      color.blue = request.arg("blue").toInt(); color.white = request.arg("white").toInt();
    }
    if (!presets.update(presetId(request), request.arg("name"), static_cast<uint16_t>(request.arg("kelvin").toInt()), hasColor ? &color : nullptr)) {
      request.sendJson(400, "{\"message\":\"Nie udało się zapisać presetu.\"}");
      return;
    }
    request.sendJson(200, "{\"message\":\"Preset zapisany.\"}");
  });
  on("/api/v1/presets/remove", Method::Post, [presetId](Request& request) {
    const uint8_t id = presetId(request);
    if (!presets.remove(id)) { request.sendJson(404, "{\"message\":\"Nie znaleziono presetu.\"}"); return; }
    inputActions.forgetPreset(id);
    request.sendJson(200, "{\"message\":\"Preset usunięty.\"}");
  });
  // id, light: sets the light's white or colour; its brightness (and off) stays.
  on("/api/v1/presets/apply", Method::Post, [presetId](Request& request) {
    if (!presets.apply(presetId(request), static_cast<uint8_t>(request.arg("light").toInt()))) {
      request.sendJson(400, "{\"message\":\"Ten preset nie pasuje do tego światła.\"}");
      return;
    }
    request.sendJson(200, "{\"message\":\"Preset ustawiony.\"}");
  });
}

void sendZones(Request& request) {
  String body = "{\"zones\":[";
  for (uint8_t index = 0; index < zones.count(); ++index) {
    const Zone& zone = zones.at(index);
    if (index) body += ',';
    body += "{\"id\":" + String(zone.id) + ",\"name\":\"" + jsonEscape(zone.name) + "\"}";
  }
  body += "],\"assignments\":{";
  for (uint8_t index = 0; index < zones.assignmentCount(); ++index) {
    if (index) body += ',';
    body += "\"" + jsonEscape(zones.assignmentTarget(index)) + "\":" + String(zones.assignmentZone(index));
  }
  request.sendJson(200, body + "}}");
}

// Things that may belong to a zone: lights and the controller's inputs (buttons).
bool zoneTargetExists(const String& target) {
  LightSnapshot light;
  if (target.startsWith("light-")) return lights.find(static_cast<uint8_t>(target.substring(6).toInt()), light);
  if (target.startsWith("controller.input-")) { const long input = target.substring(17).toInt(); return input >= 1 && input <= kControllerInputCount; }
  return false;
}

void configureZoneRoutes() {
  using whisli::net::on;
  const auto zoneId = [](const Request& request) { return static_cast<uint8_t>(request.arg("id").toInt()); };
  on("/api/v1/zones", Method::Get, sendZones);
  on("/api/v1/zones", Method::Post, [](Request& request) {
    const uint8_t id = zones.create(request.arg("name"));
    if (!id) { request.sendJson(400, "{\"message\":\"Nie udało się dodać strefy. Nazwa nie może być pusta ani powtórzona.\"}"); return; }
    request.sendJson(201, "{\"message\":\"Strefa dodana.\",\"id\":" + String(id) + "}");
  });
  on("/api/v1/zones/rename", Method::Post, [zoneId](Request& request) {
    if (!zones.rename(zoneId(request), request.arg("name"))) { request.sendJson(400, "{\"message\":\"Nie udało się zmienić nazwy. Nazwa nie może być pusta ani powtórzona.\"}"); return; }
    request.sendJson(200, "{\"message\":\"Nazwa zmieniona.\"}");
  });
  on("/api/v1/zones/remove", Method::Post, [zoneId](Request& request) {
    if (!zones.remove(zoneId(request))) { request.sendJson(404, "{\"message\":\"Nie znaleziono strefy.\"}"); return; }
    request.sendJson(200, "{\"message\":\"Strefa usunięta.\"}");
  });
  // ids=3,1,2 — every zone, in the new order.
  on("/api/v1/zones/order", Method::Post, [](Request& request) {
    uint8_t ids[kMaxZones];
    uint8_t count = 0;
    const String list = request.arg("ids");
    int start = 0;
    while (start < static_cast<int>(list.length()) && count < kMaxZones) {
      int end = list.indexOf(',', start);
      if (end < 0) end = list.length();
      ids[count++] = static_cast<uint8_t>(list.substring(start, end).toInt());
      start = end + 1;
    }
    if (!zones.reorder(ids, count)) { request.sendJson(400, "{\"message\":\"Nieprawidłowa kolejność stref.\"}"); return; }
    request.sendJson(200, "{\"message\":\"Kolejność zapisana.\"}");
  });
  // target=light-3 or controller.input-2, zone=0 clears it.
  on("/api/v1/zones/assign", Method::Post, [](Request& request) {
    const String target = request.arg("target");
    if (!zoneTargetExists(target) || !zones.assign(target, static_cast<uint8_t>(request.arg("zone").toInt()))) {
      request.sendJson(400, "{\"message\":\"Nie udało się przypisać strefy.\"}");
      return;
    }
    request.sendJson(200, "{\"message\":\"Strefa przypisana.\"}");
  });
}

// Settings storage (NVS) fill, in entries of 32 bytes: to see it coming before saves start failing.
String nvsStats() {
  nvs_stats_t stats;
  if (nvs_get_stats(nullptr, &stats) != ESP_OK) return String();
  return ",\"nvsUsedEntries\":" + String(stats.used_entries) + ",\"nvsFreeEntries\":" + String(stats.available_entries) +
         ",\"nvsNamespaces\":" + String(stats.namespace_count);
}

uint8_t requestedLightId(const Request& request) {
  const int value = request.arg("id").toInt();
  return value > 0 && value <= kMaxLights ? static_cast<uint8_t>(value) : 0;
}

// Behaviour as JSON: all fields, or with `fields` only the overridden groups.
String behaviorJson(const LightBehavior& behavior, uint8_t fields = 0xff) {
  String body = "{";
  const auto add = [&](const String& part) { if (body.length() > 1) body += ','; body += part; };
  if (fields & kBehaviorFadeIn) add("\"fadeIn\":" + String(behavior.fadeIn ? "true" : "false") + ",\"fadeInMs\":" + String(behavior.fadeInMs));
  if (fields & kBehaviorFadeOut) add("\"fadeOut\":" + String(behavior.fadeOut ? "true" : "false") + ",\"fadeOutMs\":" + String(behavior.fadeOutMs));
  if (fields & kBehaviorOnLevel) add("\"onLevel\":" + (behavior.onLevelLast ? String("\"last\"") : String(behavior.onLevelPercent)));
  if (fields & kBehaviorMax) add("\"maxLevel\":" + String(behavior.maxPercent));
  if (fields & kBehaviorRestore) add("\"restore\":" + String(behavior.restoreAfterBoot ? "true" : "false"));
  return body + "}";
}

// Reads behaviour fields from a request into `behavior`. Returns the groups present; a group sent as
// "inherit" is reported in `inherit` instead. False when a value is invalid.
bool readBehavior(const Request& request, LightBehavior& behavior, uint8_t& present, uint8_t& inherit) {
  present = 0; inherit = 0;
  const auto flag = [&](const char* name, bool& out) {
    const String value = request.arg(name);
    if (value == "true") out = true; else if (value == "false") out = false; else return false;
    return true;
  };
  const auto number = [&](const char* name, long low, long high, long& out) {
    const String value = request.arg(name);
    if (value.isEmpty()) return false;
    for (char character : value) if (character < '0' || character > '9') return false;
    out = value.toInt();
    return out >= low && out <= high;
  };
  long value = 0;
  const auto group = [&](const char* key, uint8_t field, auto read) {
    if (!request.hasArg(key)) return true;
    if (request.arg(key) == "inherit") { inherit |= field; return true; }
    if (!read()) return false;
    present |= field;
    return true;
  };
  return group("fadeIn", kBehaviorFadeIn, [&] { return flag("fadeIn", behavior.fadeIn) && number("fadeInMs", 0, 10000, value) && ((behavior.fadeInMs = value), true); }) &&
         group("fadeOut", kBehaviorFadeOut, [&] { return flag("fadeOut", behavior.fadeOut) && number("fadeOutMs", 0, 10000, value) && ((behavior.fadeOutMs = value), true); }) &&
         group("onLevel", kBehaviorOnLevel, [&] {
           if (request.arg("onLevel") == "last") { behavior.onLevelLast = true; return true; }
           if (!number("onLevel", 1, 100, value)) return false;
           behavior.onLevelLast = false; behavior.onLevelPercent = value; return true;
         }) &&
         group("maxLevel", kBehaviorMax, [&] { return number("maxLevel", 10, 100, value) && ((behavior.maxPercent = value), true); }) &&
         group("restore", kBehaviorRestore, [&] { return flag("restore", behavior.restoreAfterBoot); });
}

void sendLights(Request& request) {
  String body = "{\"lights\":[";
  for (uint8_t index = 0; index < lights.count(); ++index) {
    LightSnapshot light;
    if (!lights.get(index, light)) continue;
    if (index) body += ',';
    const bool isDmx = light.kind == LightKind::DmxDimmable;
    const bool isCct = light.kind == LightKind::DmxCct;
    const bool isRgbw = light.kind == LightKind::DmxRgbw;
    const bool isRgb = light.kind == LightKind::DmxRgb;
    const bool isRelay = light.kind == LightKind::RelaySwitch;
    body += "{\"id\":" + String(light.id) + ",\"name\":\"" + jsonEscape(light.name) +
        "\",\"kind\":\"" + String(isDmx ? "dmx" : isCct ? "cct" : isRgbw ? "rgbw" : isRgb ? "rgb" : isRelay ? "relay" : "none") + "\",\"type\":\"" +
        String(isDmx ? "dimmable" : isCct ? "cct" : isRgbw ? "rgbw" : isRgb ? "rgb" : isRelay ? "switch" : "none") + "\",\"level\":" + String(light.level) +
        ",\"relayTarget\":\"" + jsonEscape(light.relayTarget) + "\",\"dmxChannel\":" + String(light.dmxChannel);
    if (isDmx || isCct || isRgbw || isRgb) {
      static const char* const curves[] = {"linear", "perceptual", "square"};
      const DimmingProfile dimming = lights.dimming(light.id);
      body += ",\"dimming\":{\"minimum\":" + String(dimming.minimumPercent) + ",\"curve\":\"" +
              curves[static_cast<uint8_t>(dimming.curve)] + "\"}";
    }
    if (isCct) body += ",\"colorTemperature\":" + String(lights.colorTemperature(light.id));
    const LightBehaviorOverride override = lights.behaviorOverride(light.id);
    body += ",\"behavior\":" + behaviorJson(override.values, override.fields);
    if (isRgbw || isRgb) {
      const RgbwColor color = lights.color(light.id);
      body += ",\"color\":{\"red\":" + String(color.red) + ",\"green\":" + String(color.green) +
              ",\"blue\":" + String(color.blue) + ",\"white\":" + String(color.white) + "}";
    }
    body += "}";
  }
  body += "],\"defaults\":" + behaviorJson(lights.defaults()) + "}";
  request.sendJson(200, body);
}

void sendAsset(Request& request, size_t index) {
  const auto& asset = embedded_ui::kAssets[index];
  const String path(asset.path);
  // index.html must always be fresh after OTA; /assets/* carry a content hash in the name, so they never change.
  if (path == "/index.html") request.setHeader("Cache-Control", "no-store, max-age=0");
  else if (path.startsWith("/assets/")) request.setHeader("Cache-Control", "public, max-age=31536000, immutable");
  request.setHeader("Content-Encoding", "gzip");
  request.send(200, asset.contentType, reinterpret_cast<const char*>(asset.bytes), asset.size);
}

}  // namespace

void begin() {
  relays.begin();
  controllerIo.begin();
  dmx.begin();
  lights.begin();
  zones.begin();
  presets.begin();
  inputActions.begin();
  controllerIo.setInputEventHandler(handleInputEvent, &inputActions);
  xTaskCreatePinnedToCore(controlTask, "whisli-control", 6144, nullptr, whisli::system::kControlTaskPriority, nullptr, 1);
  using whisli::net::on;
  using whisli::net::onPublic;
  // The panel itself is public: it holds no data and shows the login screen when the API asks for it.
  onPublic("/app", Method::Get, [](Request& request) { request.redirect("/"); });
  for (size_t index = 0; index < embedded_ui::kAssetCount; ++index) {
    const char* path = embedded_ui::kAssets[index].path;
    const auto handler = [index](Request& request) { sendAsset(request, index); };
    if (strcmp(path, "/index.html") == 0) {
      onPublic("/", Method::Get, handler);
      onPublic("/app/", Method::Get, handler);
    } else {
      onPublic(path, Method::Get, handler);
    }
  }

  on("/api/v1/system", Method::Get, [](Request& request) {
    const bool wifiConnected = WiFi.status() == WL_CONNECTED;
    const String ethernetIp = ETH.hasIP() ? ETH.localIP().toString() : "";
    const String wifiIp = wifiConnected ? WiFi.localIP().toString() : "";
    String body = "{\"apiVersion\":\"v1\",\"application\":\"Whisli\",\"firmware\":{\"version\":\"" +
                  String(kFirmwareVersion) + "\",\"build\":\"" + String(__DATE__ " " __TIME__) +
                  "\"},\"uptimeSeconds\":" + String(millis() / 1000) +
                  ",\"hardware\":{\"board\":\"Waveshare ESP32-S3-POE-ETH-8DI-8DO\",\"model\":\"" +
                  String(ESP.getChipModel()) + "\",\"revision\":" + String(ESP.getChipRevision()) +
                  ",\"cpuMhz\":" + String(ESP.getCpuFreqMHz()) + ",\"cores\":" + String(ESP.getChipCores()) +
                  ",\"flashMb\":" + String(ESP.getFlashChipSize() / 1024 / 1024) +
                  ",\"psramMb\":" + String(ESP.getPsramSize() / 1024 / 1024) +
                  ",\"freeHeapKb\":" + String(ESP.getFreeHeap() / 1024) +
                  ",\"wifiMac\":\"" + WiFi.macAddress() + "\"},\"network\":{\"ethernetIp\":\"" +
                  ethernetIp + "\",\"wifiConnected\":" + String(wifiConnected ? "true" : "false") +
                  ",\"wifiIp\":\"" + wifiIp + "\",\"ssid\":\"" + jsonEscape(WiFi.SSID().c_str()) +
                  "\"},\"storage\":{\"firmwareKb\":" + String(ESP.getSketchSize() / 1024) +
                  ",\"otaAvailableKb\":" + String(ESP.getFreeSketchSpace() / 1024) + nvsStats() +
                  "},\"control\":{\"periodMs\":" + String(kControlPeriodMs) +
                  ",\"maxLateMs\":" + String(controlMaxLateUs / 1000.0f, 1) +
                  ",\"maxBusyMs\":" + String(controlMaxBusyUs / 1000.0f, 1) + "}}";
    request.sendJson(200, body);
  }, StateAccess::None);
  on("/api/v1/devices", Method::Get, [](Request& request) {
    sendDevices(request);
  });
  on("/api/v1/lights", Method::Get, [](Request& request) {
    sendLights(request);
  });
  on("/api/v1/lights", Method::Post, [](Request& request) {
    const uint8_t id = lights.create();
    if (!id) {
      request.sendJson(400, "{\"message\":\"Nie udało się dodać światła.\"}");
      return;
    }
    whisli::system::write(whisli::system::LogLevel::Info, "lights", "Light added");
    request.sendJson(201, "{\"message\":\"Światło dodane.\",\"id\":" + String(id) + "}");
  });
  on("/api/v1/lights/settings", Method::Post, [](Request& request) {
    const int channel = request.arg("dmxChannel").toInt();
    String reason;
    if (channel < 0 || channel > 512 || !lights.update(requestedLightId(request), request.arg("name"), request.arg("kind"), request.arg("relayTarget"), static_cast<uint16_t>(channel), &reason)) {
      if (reason.isEmpty()) reason = "Nie udało się zapisać światła.";
      request.sendJson(400, "{\"message\":\"" + jsonEscape(reason) + "\"}"); return;
    }
    request.sendJson(200, "{\"message\":\"Ustawienia światła zapisane.\"}");
  });
  on("/api/v1/lights/remove", Method::Post, [](Request& request) {
    const uint8_t id = requestedLightId(request);
    if (!lights.remove(id)) { request.sendJson(404, "{\"message\":\"Nie znaleziono światła.\"}"); return; }
    zones.assign("light-" + String(id), 0);  // ids are reused by the next light added
    whisli::system::write(whisli::system::LogLevel::Info, "lights", "Light removed");
    request.sendJson(200, "{\"message\":\"Światło usunięte.\"}");
  });
  configureZoneRoutes();
  configurePresetRoutes();
  on("/api/v1/test", Method::Get, [](Request& request) {
    request.sendJson(200, String("{\"chaos\":{\"running\":") + (chaos.running() ? "true" : "false") + ",\"restoring\":" +
                          (chaos.restoring() ? "true" : "false") + ",\"elapsedSeconds\":" + String(chaos.elapsedSeconds()) +
                          ",\"switches\":" + String(chaos.switches()) + "}}");
  });
  // on=true|false: chaos mode (random lights and outputs); off puts everything back as it was.
  on("/api/v1/test/chaos", Method::Post, [](Request& request) {
    const String value = request.arg("on");
    if (value != "true" && value != "false") { request.sendJson(400, "{\"message\":\"Podaj on=true albo on=false.\"}"); return; }
    if (value == "true") chaos.start(); else chaos.stop();
    request.sendJson(200, "{\"message\":\"" + String(value == "true" ? "Chaos mode włączony." : "Chaos mode wyłączony — wszystko wraca do stanu sprzed.") + "\"}");
  });
  on("/api/v1/system/restart", Method::Post, [](Request& request) {
    // Save what "restore after restart" needs first: the regular save waits for a quiet moment.
    LightManager::SavedState state;
    lights.takeStateToSave(state, true);
    LightManager::saveState(state);
    whisli::system::write(whisli::system::LogLevel::Info, "system", "Restart requested from the panel");
    request.setHeader("Connection", "close");
    request.sendJson(202, "{\"message\":\"Sterownik uruchamia się ponownie.\"}");
    whisli::bootstrap::scheduleRestart(1000);
  });
  on("/api/v1/lights/defaults", Method::Post, [](Request& request) {
    LightBehavior behavior = lights.defaults();
    uint8_t present = 0, inherit = 0;
    if (!readBehavior(request, behavior, present, inherit) || inherit || !lights.setDefaults(behavior)) {
      request.sendJson(400, "{\"message\":\"Nieprawidłowe ustawienia domyślne świateł.\"}"); return;
    }
    request.sendJson(200, "{\"message\":\"Ustawienia domyślne zapisane.\"}");
  });
  // Per-light overrides: send a group to override it, or the group's key as "inherit" to drop it.
  on("/api/v1/lights/behavior", Method::Post, [](Request& request) {
    const uint8_t id = requestedLightId(request);
    LightBehaviorOverride override = lights.behaviorOverride(id);
    uint8_t present = 0, inherit = 0;
    if (!readBehavior(request, override.values, present, inherit)) {
      request.sendJson(400, "{\"message\":\"Nieprawidłowe ustawienia światła.\"}"); return;
    }
    override.fields = static_cast<uint8_t>((override.fields | present) & ~inherit);
    if (!lights.setBehaviorOverride(id, override)) { request.sendJson(404, "{\"message\":\"Nie znaleziono światła.\"}"); return; }
    request.sendJson(200, "{\"message\":\"Ustawienia światła zapisane.\"}");
  });
  on("/api/v1/lights/dimming", Method::Post, [](Request& request) {
    const String curveName = request.arg("curve");
    const int minimum = request.arg("minimum").toInt();
    const int curve = curveName == "linear" ? 0 : curveName == "perceptual" ? 1 : curveName == "square" ? 2 : -1;
    if (curve < 0 || minimum < 0 || minimum > 50 ||
        !lights.setDimming(requestedLightId(request), static_cast<uint8_t>(minimum), static_cast<DimCurve>(curve))) {
      request.sendJson(400, "{\"message\":\"Nie udało się zapisać ściemniania.\"}"); return;
    }
    request.sendJson(200, "{\"message\":\"Ściemnianie zapisane.\"}");
  });
  on("/api/v1/lights/level", Method::Post, [](Request& request) {
    // "on" uses the light's on-level (last brightness or a fixed one) instead of a number.
    if (request.arg("level") == "on") {
      if (!lights.turnOn(requestedLightId(request))) { request.sendJson(400, "{\"message\":\"Nie udało się włączyć światła.\"}"); return; }
      request.sendJson(202, "{\"message\":\"Światło włączone.\"}");
      return;
    }
    const int level = request.arg("level").toInt();
    const int temperature = request.hasArg("temperature") ? request.arg("temperature").toInt() : -1;
    // RGBW colour is optional: red, green, blue and white (0–255) are sent together.
    const bool hasColor = request.hasArg("red");
    RgbwColor color;
    bool colorValid = true;
    if (hasColor) {
      const char* names[4] = {"red", "green", "blue", "white"};
      uint8_t* fields[4] = {&color.red, &color.green, &color.blue, &color.white};
      for (uint8_t i = 0; i < 4; ++i) {
        const int value = request.arg(names[i]).toInt();
        if (!request.hasArg(names[i]) || value < 0 || value > 255) colorValid = false;
        else *fields[i] = static_cast<uint8_t>(value);
      }
    }
    if (level < 0 || level > 255 || temperature > 100 || temperature < -1 || !colorValid ||
        !lights.setLevel(requestedLightId(request), static_cast<uint8_t>(level), temperature, hasColor ? &color : nullptr)) {
      request.sendJson(400, "{\"message\":\"Nie udało się zmienić poziomu światła.\"}"); return;
    }
    request.sendJson(202, "{\"message\":\"Poziom światła zmieniony.\"}");
  });
  on("/api/v1/action-panel", Method::Get, [](Request& request) {
    sendActionPanel(request);
  });
  on("/api/v1/logs", Method::Get, [](Request& request) {
    const String requestedLevel = request.arg("level");
    const String requestedCategory = request.arg("category");
    const long beforeValue = request.arg("before").toInt();
    const long afterValue = request.arg("after").toInt();
    const bool hasBefore = beforeValue > 0;
    const bool hasAfter = afterValue > 0;
    const uint32_t before = hasBefore ? static_cast<uint32_t>(beforeValue) : UINT32_MAX;
    const uint32_t after = hasAfter ? static_cast<uint32_t>(afterValue) : 0;
    const long requestedLimit = request.arg("limit").toInt();
    const uint16_t limit = requestedLimit <= 0 ? whisli::system::kLogPageSize :
        static_cast<uint16_t>(requestedLimit > whisli::system::kLogPageSize ? whisli::system::kLogPageSize : requestedLimit);
    const uint16_t count = whisli::system::count();
    uint16_t pageStart = 0;
    uint16_t selectedCount = 0;
    bool hasMore = false;
    if (hasAfter) {
      for (uint16_t index = 0; index < count; ++index) {
        whisli::system::LogEntry entry;
        if (!whisli::system::get(index, entry) || entry.id <= after || entry.id >= before) continue;
        if (selectedCount < limit) ++selectedCount;
        else { hasMore = true; break; }
      }
    } else {
      for (uint16_t index = count; index > 0; --index) {
        whisli::system::LogEntry entry;
        if (!whisli::system::get(index - 1, entry) || entry.id >= before) continue;
        const String entryLevel = whisli::system::levelName(entry.level);
        if ((!requestedLevel.isEmpty() && requestedLevel != "all" && requestedLevel != entryLevel) ||
            (!requestedCategory.isEmpty() && requestedCategory != "all" && requestedCategory != entry.category)) continue;
        if (selectedCount < limit) {
          ++selectedCount;
          pageStart = index - 1;
        } else {
          hasMore = true;
          break;
        }
      }
    }

    String body = "{\"sessionId\":" + String(whisli::system::sessionId()) +
        ",\"capacity\":" + String(whisli::system::capacity()) +
        ",\"retentionSeconds\":1800,\"hasMore\":" + String(hasMore ? "true" : "false") + ",\"entries\":[";
    body.reserve(96 + static_cast<size_t>(selectedCount) * 160);
    bool first = true;
    uint16_t serializedCount = 0;
    const uint16_t start = hasAfter ? 0 : pageStart;
    for (uint16_t index = start; index < count && serializedCount < selectedCount; ++index) {
      whisli::system::LogEntry entry;
      if (!whisli::system::get(index, entry)) continue;
      if (hasAfter && (entry.id <= after || entry.id >= before)) continue;
      if (!hasAfter && entry.id >= before) continue;
      const String entryLevel = whisli::system::levelName(entry.level);
      if ((!requestedLevel.isEmpty() && requestedLevel != "all" && requestedLevel != entryLevel) ||
          (!requestedCategory.isEmpty() && requestedCategory != "all" && requestedCategory != entry.category)) continue;
      if (!first) body += ',';
      first = false;
      ++serializedCount;
      body += "{\"id\":" + String(entry.id) + ",\"uptimeMs\":" + String(entry.uptimeMs) +
          ",\"level\":\"" + entryLevel + "\",\"category\":\"" +
          jsonEscape(entry.category) + "\",\"message\":\"" + jsonEscape(entry.message) + "\"}";
    }
    body += "]}";
    request.sendJson(200, body);
  }, StateAccess::None);
  on("/api/v1/events", Method::Post, [](Request& request) {
    if (!inputActions.addEvent(request.arg("inputId"), request.arg("trigger"))) {
      request.sendJson(400, "{\"message\":\"Nie udało się dodać zdarzenia. Sprawdź przycisk i gest.\"}");
      return;
    }
    request.sendJson(201, "{\"message\":\"Zdarzenie dodane.\"}");
  });
  on("/api/v1/events/remove", Method::Post, [](Request& request) {
    const int id = request.arg("id").toInt();
    if (id < 1 || id > 65535 || !inputActions.removeEvent(static_cast<uint16_t>(id))) {
      request.sendJson(404, "{\"message\":\"Nie znaleziono zdarzenia.\"}");
      return;
    }
    request.sendJson(200, "{\"message\":\"Zdarzenie i przypisane do niego akcje usunięto.\"}");
  });
  on("/api/v1/actions", Method::Post, [](Request& request) {
    const String enabledValue = request.arg("enabled");
    const bool enabled = enabledValue.isEmpty() || enabledValue == "true";
    if ((!enabledValue.isEmpty() && enabledValue != "true" && enabledValue != "false") ||
        !inputActions.add(request.arg("inputId"), request.arg("trigger"), request.arg("command"), request.arg("targetId"), enabled,
                          request.hasArg("value") ? request.arg("value").toInt() : -1)) {
      whisli::system::write(whisli::system::LogLevel::Error, "actions", "Could not add button action");
      request.sendJson(400, "{\"message\":\"Nie udało się dodać akcji. Sprawdź przycisk, polecenie i dostępne wyjście.\"}");
      return;
    }
    whisli::system::write(whisli::system::LogLevel::Info, "actions", "Button action added");
    request.sendJson(201, "{\"message\":\"Akcja dodana.\"}");
  });
  on("/api/v1/actions/update", Method::Post, [](Request& request) {
    const int id = request.arg("id").toInt();
    if (id < 1 || id > 65535 ||
        !inputActions.update(static_cast<uint16_t>(id), request.arg("command"), request.arg("targetId"),
                             request.hasArg("value") ? request.arg("value").toInt() : -1)) {
      whisli::system::write(whisli::system::LogLevel::Error, "actions", "Could not update button action");
      request.sendJson(400, "{\"message\":\"Nie udało się zaktualizować akcji. Sprawdź polecenie i dostępne wyjście.\"}");
      return;
    }
    whisli::system::write(whisli::system::LogLevel::Info, "actions", "Button action updated");
    request.sendJson(200, "{\"message\":\"Akcja zapisana.\"}");
  });
  on("/api/v1/actions/enabled", Method::Post, [](Request& request) {
    const int id = request.arg("id").toInt();
    const String enabled = request.arg("enabled");
    if (id < 1 || id > 65535 || (enabled != "true" && enabled != "false") ||
        !inputActions.setEnabled(static_cast<uint16_t>(id), enabled == "true")) {
      whisli::system::write(whisli::system::LogLevel::Error, "actions", "Could not change button action state");
      request.sendJson(400, "{\"message\":\"Nieprawidłowa akcja lub jej stan.\"}");
      return;
    }
    whisli::system::write(whisli::system::LogLevel::Info, "actions", "Button action state changed");
    request.sendJson(200, "{\"message\":\"Stan akcji zapisany.\"}");
  });
  on("/api/v1/actions/test", Method::Post, [](Request& request) {
    const int id = request.arg("id").toInt();
    if (id < 1 || id > 65535 || !inputActions.test(static_cast<uint16_t>(id))) {
      whisli::system::write(whisli::system::LogLevel::Error, "actions", "Button action test failed");
      request.sendJson(400, "{\"message\":\"Nie udało się przetestować akcji. Sprawdź dostępność wyjścia.\"}");
      return;
    }
    whisli::system::write(whisli::system::LogLevel::Info, "actions", "Button action tested manually");
    request.sendJson(200, "{\"message\":\"Test akcji wykonany.\"}");
  });
  on("/api/v1/actions/remove", Method::Post, [](Request& request) {
    const int id = request.arg("id").toInt();
    whisli::system::write(whisli::system::LogLevel::Info, "actions", "Action removal requested");
    if (id < 1 || id > 65535 || !inputActions.remove(static_cast<uint16_t>(id))) {
      whisli::system::write(whisli::system::LogLevel::Error, "actions", "Action removal failed");
      request.sendJson(404, "{\"message\":\"Nie znaleziono akcji.\"}");
      return;
    }
    whisli::system::write(whisli::system::LogLevel::Info, "actions", "Action removed");
    request.sendJson(200, "{\"message\":\"Akcja usunięta.\"}");
  });
  on("/api/v1/device-profiles", Method::Get, [](Request& request) {
    String body = "{\"schemaVersion\":" + String(kDeviceProfileSchemaVersion) + ",\"classes\":[";
    for (size_t index = 0; index < sizeof(kDeviceClasses) / sizeof(kDeviceClasses[0]); ++index) {
      if (index) body += ',';
      const auto& item = kDeviceClasses[index];
      body += "{\"id\":\"" + String(item.id) + "\",\"nameKey\":\"" + String(item.nameKey) +
          "\",\"descriptionKey\":\"" + String(item.descriptionKey) + "\",\"icon\":\"" + String(item.icon) +
          "\",\"available\":" + String(item.available ? "true" : "false") + "}";
    }
    body += "],\"profiles\":[";
    for (size_t index = 0; index < sizeof(kDeviceProfiles) / sizeof(kDeviceProfiles[0]); ++index) {
      if (index) body += ',';
      const auto& item = kDeviceProfiles[index];
      body += "{\"id\":\"" + String(item.id) + "\",\"deviceClass\":\"" + String(item.deviceClass) +
          "\",\"nameKey\":\"" + String(item.nameKey) + "\",\"descriptionKey\":\"" + String(item.descriptionKey) +
          "\",\"defaultNameKey\":\"" + String(item.defaultNameKey) + "\",\"communication\":\"" + String(item.communication) +
          "\",\"searchKeywords\":\"" + String(item.searchKeywords) + "\",\"defaultChannelCount\":" + String(item.defaultChannelCount) +
          ",\"defaultCoilStart\":" + String(item.defaultCoilStart) + ",\"minimumChannelCount\":" + String(item.minimumChannelCount) +
          ",\"maximumChannelCount\":" + String(item.maximumChannelCount) + ",\"editableChannelCount\":" + String(item.editableChannelCount ? "true" : "false") +
          ",\"editableCoilStart\":" + String(item.editableCoilStart ? "true" : "false") +
          ",\"available\":" + String(item.available ? "true" : "false") + "}";
    }
    body += "]}";
    request.sendJson(200, body);
  }, StateAccess::None);
  on("/api/v1/devices", Method::Post, [](Request& request) {
    const String profile = request.arg("profile");
    const String name = request.arg("name");
    const int address = request.arg("address").toInt();
    const String channelCountArgument = request.arg("channelCount");
    const String coilStartArgument = request.arg("coilStart");
    const int channelCount = channelCountArgument.isEmpty() ? 0 : channelCountArgument.toInt();
    const int coilStart = coilStartArgument.isEmpty() ? -1 : coilStartArgument.toInt();
    const DeviceProfileDefinition* selectedProfile = findDeviceProfile(profile.c_str());
    const int selectedChannels = channelCount > 0 ? channelCount : selectedProfile ? selectedProfile->defaultChannelCount : 0;
    const int selectedCoilStart = coilStart >= 0 ? coilStart : selectedProfile ? selectedProfile->defaultCoilStart : -1;
    if (name.isEmpty() || address < 1 || address > 247 || !selectedProfile || !selectedProfile->available ||
        String(selectedProfile->deviceClass) != "relay" || selectedChannels < selectedProfile->minimumChannelCount ||
        selectedChannels > selectedProfile->maximumChannelCount || selectedCoilStart < 0 || selectedCoilStart > 65503 ||
        (!channelCountArgument.isEmpty() && !selectedProfile->editableChannelCount) ||
        (!coilStartArgument.isEmpty() && !selectedProfile->editableCoilStart)) {
      request.sendJson(400, "{\"message\":\"Sprawdź profil, nazwę, adres Modbus i liczbę przekaźników.\"}");
      return;
    }
    if (!relays.add(*selectedProfile, name, static_cast<uint8_t>(address), selectedChannels, selectedCoilStart)) {
      whisli::system::write(whisli::system::LogLevel::Error, "devices", "Could not add relay device");
      request.sendJson(409, "{\"message\":\"Nie udało się dodać urządzenia. Sprawdź limit, unikalny adres slave i konfigurację profilu.\"}");
      return;
    }
    whisli::system::write(whisli::system::LogLevel::Info, "devices", "Relay device added");
    request.sendJson(201, "{\"message\":\"Urządzenie dodane. Trwa sprawdzanie połączenia.\"}");
  });
  on("/api/v1/devices/settings", Method::Post, [](Request& request) {
    if (request.arg("id") == "controller") {
      const String name = request.arg("name");
      if (!controllerIo.setName(name)) {
        request.sendJson(400, "{\"message\":\"Nieprawidłowa nazwa sterownika.\"}");
        return;
      }
      request.sendJson(200, "{\"message\":\"Nazwa sterownika zapisana.\"}");
      return;
    }
    const uint8_t id = requestedDeviceId(request);
    const String name = request.arg("name");
    const int address = request.arg("address").toInt();
    const String channelCountArgument = request.arg("channelCount");
    const String coilStartArgument = request.arg("coilStart");
    const RelaySnapshot current = relays.snapshot(id);
    const int selectedChannels = channelCountArgument.isEmpty() ? current.channelCount : channelCountArgument.toInt();
    const int selectedCoilStart = coilStartArgument.isEmpty() ? current.coilStart : coilStartArgument.toInt();
    if (!id || !current.configured || address < 1 || address > 247 || selectedChannels < 1 || selectedChannels > 32 || selectedCoilStart < 0 || selectedCoilStart > 65503 ||
        !relays.update(id, name.isEmpty() ? String(current.name) : name, static_cast<uint8_t>(address), selectedChannels, selectedCoilStart)) {
      request.sendJson(400, "{\"message\":\"Nieprawidłowe ustawienia lub adres slave jest już zajęty.\"}");
      return;
    }
    request.sendJson(200, "{\"message\":\"Ustawienia zapisane. Trwa sprawdzanie połączenia.\"}");
  });
  on("/api/v1/devices/remove", Method::Post, [](Request& request) {
    if (request.arg("id") == "controller") {
      request.sendJson(400, "{\"message\":\"Wbudowanego sterownika nie można usunąć.\"}");
      return;
    }
    if (!relays.remove(requestedDeviceId(request))) {
      whisli::system::write(whisli::system::LogLevel::Error, "devices", "Relay device removal failed");
      request.sendJson(404, "{\"message\":\"Nie znaleziono urządzenia.\"}");
      return;
    }
    whisli::system::write(whisli::system::LogLevel::Info, "devices", "Relay device removed");
    request.sendJson(200, "{\"message\":\"Urządzenie usunięte.\"}");
  });
  on("/api/v1/devices/active", Method::Post, [](Request& request) {
    const String active = request.arg("active");
    if (active != "true" && active != "false") {
      request.sendJson(400, "{\"message\":\"Aktywność musi mieć wartość true albo false.\"}");
      return;
    }
    if (!relays.setActive(requestedDeviceId(request), active == "true")) {
      request.sendJson(404, "{\"message\":\"Nie znaleziono urządzenia.\"}");
      return;
    }
    request.sendJson(200, "{\"message\":\"Status urządzenia zapisany.\"}");
  });
  on("/api/v1/devices/channels", Method::Get, [](Request& request) {
    if (request.arg("id") == "controller") {
      const ControllerDevice controller = controllerIo.snapshot();
      String body = "{\"channels\":[";
      const bool usedOnly = request.arg("used") == "true";
      bool first = true;
      for (uint8_t channel = 0; channel < kControllerOutputCount; ++channel) {
        if (usedOnly && !controller.outputUsed[channel]) continue;
        if (!first) body += ',';
        first = false;
        String channelName = controller.outputNames[channel];
        if (channelName.isEmpty()) channelName = "DO" + String(channel < 9 ? "0" : "") + String(channel + 1);
        body += "{\"id\":\"controller.output-" + String(channel + 1) + "\",\"number\":" + String(channel + 1) +
            ",\"name\":\"" + jsonEscape(channelName.c_str()) + "\",\"used\":" + String(controller.outputUsed[channel] ? "true" : "false") +
            ",\"state\":" + String(controller.outputs[channel] ? "true" : "false") + "}";
      }
      body += "]}";
      request.sendJson(200, body);
      return;
    }
    const RelaySnapshot relay = relays.snapshot(requestedDeviceId(request));
    if (!relay.configured) { request.sendJson(404, "{\"channels\":[]}"); return; }
    const bool usedOnly = request.arg("used") == "true";
    String body = "{\"channels\":[";
    bool first = true;
    if (usedOnly && !relay.active) {
      body += "]}";
      request.sendJson(200, body);
      return;
    }
    for (uint8_t channel = 0; channel < relay.channelCount; ++channel) {
      if (usedOnly && !relay.channelUsed[channel]) continue;
      if (!first) body += ',';
      first = false;
      String channelName = relay.channelNames[channel];
      if (channelName.isEmpty()) channelName = "CH" + String(channel + 1);
      body += "{\"id\":\"relay-" + String(relay.id) + ".channel-" + String(channel + 1) + "\",\"number\":" + String(channel + 1) +
          ",\"name\":\"" + jsonEscape(channelName.c_str()) + "\",\"used\":" + String(relay.channelUsed[channel] ? "true" : "false") +
          ",\"state\":" + String(relay.channels[channel] ? "true" : "false") + "}";
    }
    body += "]}";
    request.sendJson(200, body);
  });
  on("/api/v1/devices/channel", Method::Post, [](Request& request) {
    if (request.arg("id") == "controller") {
      const int channel = request.arg("channel").toInt();
      const String state = request.arg("state");
      if (channel < 1 || channel > kControllerOutputCount || (state != "on" && state != "off") ||
          !controllerIo.setOutput(channel - 1, state == "on")) {
        request.sendJson(503, "{\"message\":\"Nie udało się sterować wyjściem cyfrowym sterownika.\"}"); return;
      }
      whisli::system::write(whisli::system::LogLevel::Info, "devices", "Built-in digital output command accepted");
      request.sendJson(202, "{\"message\":\"Stan wyjścia cyfrowego zmieniony.\"}");
      return;
    }
    const uint8_t id = requestedDeviceId(request);
    const int channel = request.arg("channel").toInt();
    const String state = request.arg("state");
    if (channel < 1 || channel > 32 || (state != "on" && state != "off") || !relays.setChannel(id, channel - 1, state == "on")) {
      request.sendJson(400, "{\"message\":\"Nieprawidłowy kanał lub stan.\"}"); return;
    }
    whisli::system::write(whisli::system::LogLevel::Info, "relays", "Relay output command queued");
    request.sendJson(202, "{\"message\":\"Polecenie przekazane do Modbus.\"}");
  });
  on("/api/v1/devices/channels/all", Method::Post, [](Request& request) {
    const String state = request.arg("state");
    if (request.arg("id") == "controller") {
      if ((state != "on" && state != "off") || !controllerIo.setAllOutputs(state == "on")) {
        request.sendJson(503, "{\"message\":\"Nie udało się zmienić stanu wyjść sterownika.\"}"); return;
      }
      request.sendJson(202, "{\"message\":\"Stan wyjść cyfrowych zmieniony.\"}");
      return;
    }
    if ((state != "on" && state != "off") || !relays.setAllChannels(requestedDeviceId(request), state == "on")) {
      request.sendJson(400, "{\"message\":\"Nieprawidłowy stan lub nie znaleziono urządzenia.\"}"); return;
    }
    request.sendJson(202, "{\"message\":\"Polecenie zbiorcze przekazane do Modbus.\"}");
  });
  on("/api/v1/devices/channel/name", Method::Post, [](Request& request) {
    const int channel = request.arg("channel").toInt();
    if (request.arg("id") == "controller") {
      if (channel < 1 || channel > kControllerOutputCount || !controllerIo.setOutputName(channel - 1, request.arg("name"))) {
        request.sendJson(400, "{\"message\":\"Nieprawidłowa nazwa wyjścia cyfrowego.\"}"); return;
      }
      request.sendJson(200, "{\"message\":\"Nazwa wyjścia zapisana.\"}");
      return;
    }
    if (channel < 1 || channel > 32 || !relays.setChannelName(requestedDeviceId(request), channel - 1, request.arg("name"))) {
      request.sendJson(400, "{\"message\":\"Nieprawidłowy kanał lub nazwa jest zbyt długa.\"}"); return;
    }
    request.sendJson(200, "{\"message\":\"Nazwa kanału zapisana.\"}");
  });
  on("/api/v1/devices/channel/usage", Method::Post, [](Request& request) {
    const int channel = request.arg("channel").toInt();
    const String used = request.arg("used");
    if (request.arg("id") == "controller") {
      if (channel < 1 || channel > kControllerOutputCount || (used != "true" && used != "false") || !controllerIo.setOutputUsed(channel - 1, used == "true")) {
        request.sendJson(400, "{\"message\":\"Nieprawidłowy kanał lub wartość pola Używany.\"}"); return;
      }
      request.sendJson(200, "{\"message\":\"Ustawienie użycia wyjścia zapisane.\"}");
      return;
    }
    if (channel < 1 || channel > 32 || (used != "true" && used != "false") || !relays.setChannelUsed(requestedDeviceId(request), channel - 1, used == "true")) {
      request.sendJson(400, "{\"message\":\"Nieprawidłowy kanał lub wartość pola Używany.\"}"); return;
    }
    request.sendJson(200, "{\"message\":\"Ustawienie użycia kanału zapisane.\"}");
  });
  on("/api/v1/devices/inputs", Method::Get, [](Request& request) {
    if (request.arg("id") != "controller") {
      request.sendJson(404, "{\"inputs\":[]}"); return;
    }
    const ControllerDevice controller = controllerIo.snapshot();
    const bool usedOnly = request.arg("used") == "true";
    String body = "{\"inputs\":[";
    bool first = true;
    for (uint8_t channel = 0; channel < kControllerInputCount; ++channel) {
      if (usedOnly && !controller.inputUsed[channel]) continue;
      if (!first) body += ',';
      first = false;
      String channelName = controller.inputNames[channel];
      if (channelName.isEmpty()) channelName = "DI" + String(channel < 9 ? "0" : "") + String(channel + 1);
      body += "{\"id\":\"controller.input-" + String(channel + 1) + "\",\"number\":" + String(channel + 1) +
          ",\"name\":\"" + jsonEscape(channelName.c_str()) + "\",\"used\":" + String(controller.inputUsed[channel] ? "true" : "false") +
          ",\"high\":" + String(controller.inputsHigh[channel] ? "true" : "false") +
          ",\"lastEvent\":\"" + String(buttonEventName(controller.inputLastEvents[channel])) + "\"}";
    }
    body += "]}";
    request.sendJson(200, body);
  });
  on("/api/v1/devices/input/name", Method::Post, [](Request& request) {
    const int channel = request.arg("channel").toInt();
    if (request.arg("id") != "controller" || channel < 1 || channel > kControllerInputCount ||
        !controllerIo.setInputName(channel - 1, request.arg("name"))) {
      request.sendJson(400, "{\"message\":\"Nieprawidłowa nazwa wejścia cyfrowego.\"}"); return;
    }
    request.sendJson(200, "{\"message\":\"Nazwa wejścia zapisana.\"}");
  });
  on("/api/v1/devices/input/usage", Method::Post, [](Request& request) {
    const int channel = request.arg("channel").toInt();
    const String used = request.arg("used");
    if (request.arg("id") != "controller" || channel < 1 || channel > kControllerInputCount ||
        (used != "true" && used != "false") || !controllerIo.setInputUsed(channel - 1, used == "true")) {
      request.sendJson(400, "{\"message\":\"Nieprawidłowe wejście lub wartość pola Używany.\"}"); return;
    }
    request.sendJson(200, "{\"message\":\"Ustawienie użycia wejścia zapisane.\"}");
  });

  Serial.printf("[APP] routes: / (%u embedded assets) and /api/v1\n", embedded_ui::kAssetCount);
}

void poll() {
  broadcastChanges();
  // Levels to restore after a restart: copied under the lock, written to flash outside it.
  LightManager::SavedState state;
  bool due;
  {
    whisli::system::StateLock lock;
    chaos.tick();
    due = lights.takeStateToSave(state);
  }
  if (due) LightManager::saveState(state);
  mqttBridge.poll();  // outside the lock: it takes it itself, briefly
}

}  // namespace whisli::app
