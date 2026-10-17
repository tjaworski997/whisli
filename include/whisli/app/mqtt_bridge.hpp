#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <mqtt_client.h>

#include "whisli/app/controller_io.hpp"
#include "whisli/app/lights.hpp"
#include "whisli/net/mqtt_settings.hpp"

namespace whisli::app {

// Home Assistant over MQTT. Lights become `light` entities (JSON schema, with brightness, colour
// temperature or colour as they support), buttons become device triggers, all announced through MQTT
// Discovery under one device. Topics, with <base> from the settings:
//
//   <base>/status                     online / offline (retained, last will)
//   <base>/light/<id>/state           {"state":"ON","brightness":…} (retained)
//   <base>/light/<id>/set             commands from Home Assistant, same JSON
//   <base>/input/<n>/action           click, doubleClick, tripleClick, longClick, hold
//
// Everything that can wait for the network runs in low-priority tasks: the esp-mqtt client (connects
// and reconnects by itself) and our worker (starts and stops the client when settings change, sends
// what loop() queued). loop() only applies queued commands under the state lock and compares states;
// it never waits for the network, a broker or a lock held by them. Nothing here runs ahead of buttons
// and lights, and a wrong or unreachable broker only shows up as an error on the MQTT page.
class MqttBridge {
 public:
  MqttBridge(LightManager& lights, ControllerIo& controller) : lights_(lights), controller_(controller) {}

  // From loop(): (re)connects when settings change, applies commands, publishes what changed.
  void poll();
  // A recognised gesture (click, doubleClick, …) of controller input `input` (0-based).
  void publishGesture(uint8_t input, const char* trigger);

 private:
  struct Command { char topic[96]; char payload[256]; };
  struct Outgoing { String topic; String payload; bool retain; };
  static void workerTask(void* context);
  void restart();  // worker only

  static void onEvent(void* context, esp_event_base_t base, int32_t id, void* data);
  void handleEvent(esp_mqtt_event_handle_t event);
  void start();
  void stop();
  void applyCommand(const Command& command);
  void publishAll();
  void publishStates(bool force);
  void publishDiscovery(bool force);
  bool publish(const String& topic, const String& payload, bool retain);
  String deviceJson() const;
  String lightStateJson(const LightSnapshot& light) const;

  LightManager& lights_;
  ControllerIo& controller_;
  esp_mqtt_client_handle_t client_ = nullptr;
  QueueHandle_t commands_ = nullptr;
  QueueHandle_t outgoing_ = nullptr;      // Outgoing*, sent by senderTask
  SemaphoreHandle_t clientLock_ = nullptr;    // worker-side: publishing vs stopping the client
  SemaphoreHandle_t settingsLock_ = nullptr;  // settings_ and topics; loop() only ever tries it
  net::mqtt::Settings settings_;
  uint32_t settingsVersion_ = 0;
  String clientId_;
  String nodeId_;           // "whisli_<mac>": unique ids and the discovery node
  String availability_;     // <base>/status
  volatile bool connected_ = false;
  volatile bool resync_ = false;  // just connected, or Home Assistant restarted: send everything again
  uint32_t lastStatesAt_ = 0;
  uint32_t lastDiscoveryCheckAt_ = 0;

  String lightStates_[kMaxLights];        // last published state per light slot
  String lightDiscovery_[kMaxLights];     // last published discovery config per light slot
  String inputDiscovery_[kControllerInputCount];
};

}  // namespace whisli::app
