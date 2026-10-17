#include <Arduino.h>

#include "whisli/app/application.hpp"
#include "whisli/bootstrap/bootstrap_portal.hpp"
#include "whisli/system/log.hpp"
#include "whisli/system/state_lock.hpp"

// Composition root. Systemowy bootstrap i automatyka są niezależnymi modułami,
// ale zawsze tworzą jeden obraz firmware dla ESP32.
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[BOOT] Whisli");
  whisli::system::begin();
  whisli::system::beginStateLock();
  whisli::bootstrap::begin();
  whisli::app::begin();
}

// Lowest priority: Wi-Fi housekeeping and change notifications. Buttons and lights run in their own
// task (see whisli/system/task_priorities.hpp), so nothing here can delay them.
void loop() {
  whisli::bootstrap::poll();
  whisli::app::poll();
  delay(5);
}
