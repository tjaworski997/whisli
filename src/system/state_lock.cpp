#include "whisli/system/state_lock.hpp"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace whisli::system {
namespace {
SemaphoreHandle_t stateMutex = nullptr;
}

void beginStateLock() {
  if (!stateMutex) stateMutex = xSemaphoreCreateRecursiveMutex();
}

bool stateLockHeldByCurrentTask() {
  return stateMutex && xSemaphoreGetMutexHolder(stateMutex) == xTaskGetCurrentTaskHandle();
}

StateLock::StateLock() { xSemaphoreTakeRecursive(stateMutex, portMAX_DELAY); }
StateLock::~StateLock() { xSemaphoreGiveRecursive(stateMutex); }

}  // namespace whisli::system
