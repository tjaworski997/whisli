#include "whisli/app/chaos_mode.hpp"

#include "whisli/app/state_events.hpp"
#include "whisli/system/log.hpp"

namespace whisli::app {
namespace {
constexpr uint32_t kTickMs = 100;  // relay snapshots are large: no need to look more often
uint32_t randomBetween(uint32_t low, uint32_t high) { return low + esp_random() % (high - low); }
}  // namespace

void ChaosMode::start() {
  if (running_) stop();
  restoring_ = false;
  const ControllerDevice controller = controller_.snapshot();
  memcpy(outputsBefore_, controller.outputs, sizeof(outputsBefore_));
  // Active modules only: an inactive one is switched off on purpose (or not even connected).
  uint8_t ids[kMaxRelayDevices];
  const uint8_t count = relays_.deviceIds(ids, kMaxRelayDevices);
  moduleCount_ = 0;
  for (uint8_t index = 0; index < count; ++index) {
    const RelaySnapshot relay = relays_.snapshot(ids[index]);
    if (!relay.configured || !relay.active) continue;
    Module& module = modules_[moduleCount_++];
    module.id = relay.id;
    module.channelCount = relay.channelCount;
    memcpy(module.before, relay.channels, sizeof(module.before));
    module.nextAt = millis() + randomBetween(0, 800);
  }
  lights_.setChaos(true);
  startedAt_ = millis();
  nextOutputAt_ = startedAt_;
  switches_ = 0;
  running_ = true;
  whisli::system::write(whisli::system::LogLevel::Info, "system", "Chaos mode started");
  notifyChange(kTopicDevices | kTopicLights);
}

void ChaosMode::stop() {
  if (!running_) return;
  running_ = false;
  restoring_ = true;
  restoreStartedAt_ = millis();
  lights_.setChaos(false);
  char message[64];
  snprintf(message, sizeof(message), "Chaos mode ended after %lu s, %lu switches", static_cast<unsigned long>((millis() - startedAt_) / 1000), static_cast<unsigned long>(switches_));
  whisli::system::write(whisli::system::LogLevel::Info, "system", message);
  restore();
  notifyChange(kTopicDevices | kTopicLights);
}

void ChaosMode::tick() {
  if (!running_ && !restoring_) return;
  const uint32_t now = millis();
  if (now - lastTickAt_ < kTickMs) return;
  lastTickAt_ = now;
  if (restoring_) {
    // A module that stopped answering would never confirm: give up on it after a while.
    const bool timedOut = now - restoreStartedAt_ > 30000;
    if (restore() || timedOut) {
      restoring_ = false;
      if (timedOut) whisli::system::write(whisli::system::LogLevel::Warning, "system", "Chaos mode: some relay outputs did not return to their previous state");
      notifyChange(kTopicDevices);
    }
    return;
  }
  lights_.chaosTick();
  bool changed = false;
  if (static_cast<int32_t>(now - nextOutputAt_) >= 0) {
    const uint8_t channel = esp_random() % kControllerOutputCount;
    if (controller_.setOutput(channel, !controller_.snapshot().outputs[channel])) { ++switches_; changed = true; }
    nextOutputAt_ = now + randomBetween(300, 1200);
  }
  for (uint8_t index = 0; index < moduleCount_; ++index) {
    Module& module = modules_[index];
    if (!module.channelCount || static_cast<int32_t>(now - module.nextAt) < 0) continue;
    const uint8_t channel = esp_random() % module.channelCount;
    if (relays_.setChannel(module.id, channel, !relays_.snapshot(module.id).channels[channel])) { ++switches_; changed = true; }
    module.nextAt = now + randomBetween(400, 1600);
  }
  if (changed) notifyChange(kTopicDevices);
}

// The relay command queue is short, so channels go back a few at a time over several ticks.
bool ChaosMode::restore() {
  bool done = true;
  const ControllerDevice controller = controller_.snapshot();
  for (uint8_t channel = 0; channel < kControllerOutputCount; ++channel) {
    if (controller.outputs[channel] != outputsBefore_[channel]) controller_.setOutput(channel, outputsBefore_[channel]);
  }
  for (uint8_t index = 0; index < moduleCount_; ++index) {
    const Module& module = modules_[index];
    const RelaySnapshot relay = relays_.snapshot(module.id);
    if (!relay.configured) continue;  // removed meanwhile
    uint8_t queued = 0;
    for (uint8_t channel = 0; channel < module.channelCount && queued < 4; ++channel) {
      if (relay.channels[channel] == module.before[channel]) continue;
      done = false;
      if (relays_.setChannel(module.id, channel, module.before[channel])) ++queued;
    }
  }
  return done;
}

}  // namespace whisli::app
