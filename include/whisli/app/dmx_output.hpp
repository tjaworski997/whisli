#pragma once

#include <Arduino.h>

namespace whisli::app {

// One physical DMX512 universe. It owns UART0/TXD GPIO43 and continuously
// transmits a full frame; callers only update slots in its output buffer.
class DmxOutput {
 public:
  void begin();
  bool setSlot(uint16_t channel, uint8_t value);
  uint8_t slotValue(uint16_t channel);

 private:
  static void taskEntry(void* context);
  void task();

  SemaphoreHandle_t lock_ = nullptr;
  HardwareSerial serial_{0};
  uint8_t frame_[513] = {};
};

}  // namespace whisli::app
