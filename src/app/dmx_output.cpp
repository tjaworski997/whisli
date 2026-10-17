#include "whisli/app/dmx_output.hpp"

#include <driver/uart.h>
#include <esp_rom_sys.h>

#include "whisli/system/task_priorities.hpp"

namespace whisli::app {
namespace {
constexpr int kDmxTxPin = 43;
constexpr uint32_t kDmxFrameIntervalMs = 25;
constexpr uint32_t kBreakUs = 120;
constexpr uint32_t kMarkAfterBreakUs = 12;
}

void DmxOutput::begin() {
  lock_ = xSemaphoreCreateMutex();
  if (!lock_) return;
  serial_.setTxBufferSize(1024);
  serial_.begin(250000, SERIAL_8N2, -1, kDmxTxPin);
  // Core 0 is left to Wi-Fi, Ethernet (W5500) and lwIP; starving its idle task trips the task watchdog.
  xTaskCreatePinnedToCore(taskEntry, "whisli-dmx", 4096, this, whisli::system::kDmxTaskPriority, nullptr, ARDUINO_RUNNING_CORE);
  Serial.printf("[DMX] UART0 TX=%d 250000 8N2\n", kDmxTxPin);
}

bool DmxOutput::setSlot(uint16_t channel, uint8_t value) {
  if (!lock_ || channel == 0 || channel > 512) return false;
  xSemaphoreTake(lock_, portMAX_DELAY);
  frame_[channel] = value;
  xSemaphoreGive(lock_);
  return true;
}

uint8_t DmxOutput::slotValue(uint16_t channel) {
  if (!lock_ || channel == 0 || channel > 512) return 0;
  xSemaphoreTake(lock_, portMAX_DELAY);
  const uint8_t value = frame_[channel];
  xSemaphoreGive(lock_);
  return value;
}

void DmxOutput::taskEntry(void* context) { static_cast<DmxOutput*>(context)->task(); }

void DmxOutput::task() {
  TickType_t nextFrameAt = xTaskGetTickCount();
  uint8_t transmitFrame[513] = {};
  for (;;) {
    xSemaphoreTake(lock_, portMAX_DELAY);
    memcpy(transmitFrame, frame_, sizeof(frame_));
    xSemaphoreGive(lock_);
    // Wait (blocking, no CPU) until the previous frame has left the UART, then send
    // BREAK (>= 88 us) and MARK-after-break (>= 8 us) by briefly inverting TX, and queue the
    // start code plus 512 slots. uart_write_bytes() only copies into the TX ring buffer, which
    // the UART interrupt drains, so this task uses ~0.1 ms of CPU per 25 ms frame.
    // (uart_write_bytes_with_break() kept the CPU busy while flash writes held off interrupts,
    // starving everything below it during OTA.)
    uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(kDmxFrameIntervalMs));
    uart_set_line_inverse(UART_NUM_0, UART_SIGNAL_TXD_INV);
    esp_rom_delay_us(kBreakUs);
    uart_set_line_inverse(UART_NUM_0, UART_SIGNAL_INV_DISABLE);
    esp_rom_delay_us(kMarkAfterBreakUs);
    uart_write_bytes(UART_NUM_0, reinterpret_cast<const char*>(transmitFrame), sizeof(transmitFrame));
    vTaskDelayUntil(&nextFrameAt, pdMS_TO_TICKS(kDmxFrameIntervalMs));
  }
}

}  // namespace whisli::app
