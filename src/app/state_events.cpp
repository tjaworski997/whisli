#include "whisli/app/state_events.hpp"

#include <freertos/FreeRTOS.h>

namespace whisli::app {
namespace {
portMUX_TYPE changeMux = portMUX_INITIALIZER_UNLOCKED;
uint8_t pendingTopics = 0;
uint32_t revisionCounter = 0;

constexpr uint8_t kInputEventCapacity = 16;
InputEventRecord inputEvents[kInputEventCapacity];
uint8_t inputEventHead = 0;
uint8_t inputEventCount = 0;
}  // namespace

void notifyChange(uint8_t topics) {
  portENTER_CRITICAL(&changeMux);
  pendingTopics |= topics;
  ++revisionCounter;
  portEXIT_CRITICAL(&changeMux);
}

uint8_t takeChanges(uint32_t& revision) {
  portENTER_CRITICAL(&changeMux);
  const uint8_t topics = pendingTopics;
  pendingTopics = 0;
  revision = revisionCounter;
  portEXIT_CRITICAL(&changeMux);
  return topics;
}

void recordInputEvent(uint8_t input, const char* trigger) {
  portENTER_CRITICAL(&changeMux);
  inputEvents[(inputEventHead + inputEventCount) % kInputEventCapacity] = InputEventRecord{input, trigger};
  if (inputEventCount < kInputEventCapacity) ++inputEventCount;
  else inputEventHead = (inputEventHead + 1) % kInputEventCapacity;
  portEXIT_CRITICAL(&changeMux);
}

bool takeInputEvent(InputEventRecord& output) {
  portENTER_CRITICAL(&changeMux);
  const bool available = inputEventCount > 0;
  if (available) {
    output = inputEvents[inputEventHead];
    inputEventHead = (inputEventHead + 1) % kInputEventCapacity;
    --inputEventCount;
  }
  portEXIT_CRITICAL(&changeMux);
  return available;
}

}  // namespace whisli::app
