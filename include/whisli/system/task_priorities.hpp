#pragma once

#include <freertos/FreeRTOS.h>

namespace whisli::system {

// Absolute priority: buttons and lights. Everything else (panel, API, OTA, logs, push) yields to them.
// All application tasks run on core 1; core 0 belongs to Wi-Fi, Ethernet and lwIP.
constexpr UBaseType_t kControlTaskPriority = 10;  // inputs → actions → lights and outputs
constexpr UBaseType_t kModbusTaskPriority = 9;    // relay commands issued by actions
constexpr UBaseType_t kDmxTaskPriority = 8;       // DMX frames to the dimmers
constexpr UBaseType_t kHttpTaskPriority = 5;      // panel, API and firmware upload (lower makes OTA ~3x slower)
constexpr UBaseType_t kMqttTaskPriority = 3;      // Home Assistant link: never ahead of buttons, lights or the panel
// loop() stays at the Arduino default (1): network housekeeping and change notifications.

}  // namespace whisli::system
