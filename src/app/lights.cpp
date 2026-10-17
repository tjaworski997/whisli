#include "whisli/app/lights.hpp"

#include "whisli/system/config_store.hpp"

#include <memory>
#include <cmath>
#include <cstring>

#include "whisli/app/state_events.hpp"
#include "whisli/system/log.hpp"

namespace whisli::app {
namespace {
// Version 1, key "config": all 32 slots, used or not (2.3 KB). Read only to migrate.
constexpr uint8_t kLegacySlots = 32;
struct PersistedLights { uint8_t version = 1; LightSnapshot lights[kLegacySlots] = {}; };
// Version 2, key "lights": [version][count] and only the lights that exist. The settings storage is
// small and every rewrite of a blob needs room for a second copy, so blobs stay as short as they can.
constexpr uint8_t kCompactVersion = 2;

// Writes only when the stored bytes differ, sparing the storage a copy of unchanged data.
// The store itself skips writes of unchanged values.
bool putIfChanged(whisli::system::ConfigStore& preferences, const char* key, const void* data, size_t size) {
  return preferences.putBytes(key, data, size) == size;
}

// Per-light tables saved with fewer slots (older firmware had 32): the rest keeps its defaults.
void getUpTo(whisli::system::ConfigStore& preferences, const char* key, void* data, size_t size, size_t itemSize) {
  const size_t stored = preferences.getBytesLength(key);
  if (stored == 0 || stored > size || stored % itemSize != 0) return;
  std::unique_ptr<uint8_t[]> buffer(new uint8_t[stored]);
  if (preferences.getBytes(key, buffer.get(), stored) == stored) memcpy(data, buffer.get(), stored);
}

bool parseNumber(const String& value, uint8_t& output) {
  if (value.isEmpty()) return false;
  for (char character : value) if (character < '0' || character > '9') return false;
  const long number = value.toInt();
  if (number < 1 || number > 255) return false;
  output = static_cast<uint8_t>(number);
  return true;
}

bool isDmxKind(LightKind kind) { return kind == LightKind::DmxDimmable || kind == LightKind::DmxCct || kind == LightKind::DmxRgbw || kind == LightKind::DmxRgb; }
bool isColorKind(LightKind kind) { return kind == LightKind::DmxRgbw || kind == LightKind::DmxRgb; }
uint8_t dmxChannelCount(LightKind kind) { return kind == LightKind::DmxRgbw ? 4 : kind == LightKind::DmxRgb ? 3 : kind == LightKind::DmxCct ? 2 : 1; }
uint8_t scaled(uint8_t value, uint8_t level) { return static_cast<uint8_t>(static_cast<uint16_t>(value) * level / 255); }
}

LightManager::LightManager(ControllerIo& controller, ModbusRelayManager& relays, DmxOutput& dmx)
    : controller_(controller), relays_(relays), dmx_(dmx) {}

void LightManager::begin() {
  memset(colorTemperatures_, 50, sizeof(colorTemperatures_));
  whisli::system::ConfigStore preferences;
  preferences.begin("whisli-lights", true);
  // Definitions: the compact key, or the old full one (then saved compactly once loaded).
  std::unique_ptr<PersistedLights> persisted(new PersistedLights());
  std::unique_ptr<LightSnapshot[]> loadedLights(new LightSnapshot[kMaxLights]());
  bool loaded = false;
  const size_t compactSize = preferences.getBytesLength("lights");
  if (compactSize >= 2 && compactSize <= 2 + sizeof(LightSnapshot) * kMaxLights) {
    std::unique_ptr<uint8_t[]> compact(new uint8_t[compactSize]);
    if (preferences.getBytes("lights", compact.get(), compactSize) == compactSize && compact[0] == kCompactVersion &&
        compactSize == 2 + sizeof(LightSnapshot) * compact[1]) {
      for (uint8_t item = 0; item < compact[1]; ++item) {
        LightSnapshot light;
        memcpy(&light, compact.get() + 2 + sizeof(LightSnapshot) * item, sizeof(light));
        if (light.id >= 1 && light.id <= kMaxLights) loadedLights[light.id - 1] = light;  // a light's id is its slot + 1
      }
      loaded = true;
    }
  }
  migrateDefinitions_ = false;
  if (!loaded && preferences.getBytesLength("config") == sizeof(PersistedLights) &&
      preferences.getBytes("config", persisted.get(), sizeof(PersistedLights)) == sizeof(PersistedLights) && persisted->version == 1) {
    loaded = migrateDefinitions_ = true;
    for (uint8_t index = 0; index < kLegacySlots; ++index) loadedLights[index] = persisted->lights[index];
  }
  // Other light data without the definitions means they went missing: say so instead of starting empty silently.
  if (!loaded && preferences.getBytesLength("cct") > 0) {
    whisli::system::write(whisli::system::LogLevel::Error, "lights", "Light definitions missing in storage");
  }
  if (loaded) {
    for (uint8_t index = 0; index < kMaxLights; ++index) {
      const LightSnapshot& light = loadedLights[index];
      if (!light.configured || light.id == 0 || light.name[0] == '\0') continue;
      if (isDmxKind(light.kind) && (light.dmxChannel == 0 || light.dmxChannel + dmxChannelCount(light.kind) - 1 > 512)) continue;
      lights_[index] = light;
      // Outputs start off (DMX slots are zeroed, relay lights read their real output), so the stored
      // level from the last settings save must not claim otherwise.
      lights_[index].level = 0;
      if (isDmxKind(light.kind)) {
        for (uint8_t offset = 0; offset < dmxChannelCount(light.kind); ++offset) dmx_.setSlot(light.dmxChannel + offset, 0);
      }
    }
  }
  getUpTo(preferences, "cct", colorTemperatures_, sizeof(colorTemperatures_), sizeof(colorTemperatures_[0]));
  getUpTo(preferences, "rgbw", colors_, sizeof(colors_), sizeof(colors_[0]));
  getUpTo(preferences, "dim", dimming_, sizeof(dimming_), sizeof(dimming_[0]));
  if (preferences.getBytesLength("behav") == sizeof(defaults_)) preferences.getBytes("behav", &defaults_, sizeof(defaults_));
  getUpTo(preferences, "behovr", overrides_, sizeof(overrides_), sizeof(overrides_[0]));
  // Levels to restore: [levels][last-on levels], one byte per slot, possibly saved with fewer slots.
  SavedState saved;
  bool hasSaved = false;
  const size_t stateSize = preferences.getBytesLength("state");
  if (stateSize >= 2 && stateSize % 2 == 0 && stateSize <= sizeof(saved)) {
    std::unique_ptr<uint8_t[]> buffer(new uint8_t[stateSize]);
    if (preferences.getBytes("state", buffer.get(), stateSize) == stateSize) {
      const size_t slots = stateSize / 2;
      memcpy(saved.level, buffer.get(), slots);
      memcpy(saved.lastOn, buffer.get() + slots, slots);
      hasSaved = true;
    }
  }
  preferences.end();
  if (migrateDefinitions_ && save()) {
    whisli::system::write(whisli::system::LogLevel::Info, "lights", "Light definitions moved to the compact storage layout");
  }

  // Restore what was on before the restart, for lights set to do so. DMX lights fade in from dark.
  if (!hasSaved) return;
  memcpy(lastOn_, saved.lastOn, sizeof(lastOn_));
  for (uint8_t index = 0; index < kMaxLights; ++index) {
    const LightSnapshot& light = lights_[index];
    if (!light.configured || saved.level[index] == 0 || !effectiveBehavior(index).restoreAfterBoot) continue;
    if (isDmxKind(light.kind)) {
      lights_[index].level = saved.level[index];
      startTransition(index, saved.level[index]);
    } else if (light.kind == LightKind::RelaySwitch) {
      setRelayTarget(light.relayTarget, true);
    }
  }
}

uint8_t LightManager::count() const { uint8_t result = 0; for (const auto& light : lights_) if (light.configured) ++result; return result; }
bool LightManager::get(uint8_t index, LightSnapshot& output) const {
  uint8_t found = 0;
  for (const auto& light : lights_) if (light.configured) { if (found++ == index) { output = withLiveLevel(light); return true; } }
  return false;
}

bool LightManager::find(uint8_t id, LightSnapshot& output) const {
  const int index = indexForId(id);
  if (index < 0) return false;
  output = withLiveLevel(lights_[index]);
  return true;
}

// A relay light has no state of its own: it is on exactly when its output is, whoever switched it
// (a button, the Devices page, another action). DMX is one-way, so DMX lights keep the level we sent.
LightSnapshot LightManager::withLiveLevel(const LightSnapshot& light) const {
  LightSnapshot copy = light;
  if (light.kind == LightKind::RelaySwitch) copy.level = relayTargetOn(light.relayTarget) ? 255 : 0;
  return copy;
}

bool LightManager::relayTargetOn(const String& target) const {
  if (target.startsWith("controller.output-")) {
    uint8_t channel;
    return parseNumber(target.substring(18), channel) && channel <= kControllerOutputCount && controller_.snapshot().outputs[channel - 1];
  }
  const int separator = target.indexOf(".output-"); uint8_t device; uint8_t channel;
  if (!target.startsWith("relay-") || separator < 6 || !parseNumber(target.substring(6, separator), device) ||
      !parseNumber(target.substring(separator + 8), channel)) return false;
  const RelaySnapshot relay = relays_.snapshot(device);
  return relay.configured && channel <= relay.channelCount && relay.channels[channel - 1];
}

int LightManager::indexForId(uint8_t id) const { for (uint8_t i = 0; i < kMaxLights; ++i) if (lights_[i].configured && lights_[i].id == id) return i; return -1; }

uint8_t LightManager::create() {
  for (uint8_t index = 0; index < kMaxLights; ++index) {
    if (lights_[index].configured) continue;
    LightSnapshot& light = lights_[index];
    light = LightSnapshot{}; light.configured = true; light.id = index + 1; light.kind = LightKind::None;
    strlcpy(light.name, "Nowe światło", sizeof(light.name));
    if (save()) return light.id;
    light = LightSnapshot{}; return 0;
  }
  return 0;
}

bool LightManager::update(uint8_t id, const String& name, const String& kindValue, const String& relayTarget, uint16_t dmxChannel, String* reason) {
  const auto refuse = [reason](const String& text) { if (reason) *reason = text; return false; };
  if (name.isEmpty() || name.length() >= 33) return refuse("Nazwa musi mieć od 1 do 32 znaków.");
  LightKind kind;
  if (kindValue == "relay") kind = LightKind::RelaySwitch;
  else if (kindValue == "dmx") kind = LightKind::DmxDimmable;
  else if (kindValue == "cct") kind = LightKind::DmxCct;
  else if (kindValue == "rgbw") kind = LightKind::DmxRgbw;
  else if (kindValue == "rgb") kind = LightKind::DmxRgb;
  else return false;
  const int currentIndex = indexForId(id);
  if (currentIndex < 0) return false;
  if (isDmxKind(kind) && (dmxChannel == 0 || dmxChannel + dmxChannelCount(kind) - 1 > 512)) return refuse("Kanał DMX poza zakresem 1–512.");
  for (uint8_t index = 0; index < kMaxLights; ++index) {
    if (index == currentIndex || !lights_[index].configured) continue;
    const String other = String("„") + lights_[index].name + "”";
    if (kind == LightKind::RelaySwitch && lights_[index].kind == kind && relayTarget == lights_[index].relayTarget) {
      return refuse("To wyjście jest już przypisane do światła " + other + ".");
    }
    if (isDmxKind(kind) && isDmxKind(lights_[index].kind)) {
      const uint16_t firstEnd = dmxChannel + dmxChannelCount(kind) - 1;
      const uint16_t secondStart = lights_[index].dmxChannel;
      const uint16_t secondEnd = secondStart + dmxChannelCount(lights_[index].kind) - 1;
      if (dmxChannel <= secondEnd && secondStart <= firstEnd) return refuse("Kanały DMX nachodzą na światło " + other + ".");
    }
  }
  if (kind == LightKind::RelaySwitch && !relayTargetAvailable(relayTarget)) return refuse("To wyjście nie jest dostępne (nieużywane albo moduł wyłączony).");
  LightSnapshot& light = lights_[currentIndex];
  const LightSnapshot previous = light;
  const bool sameDmxMapping = isDmxKind(previous.kind) && isDmxKind(kind) && previous.dmxChannel == dmxChannel && previous.kind == kind;
  if (isDmxKind(previous.kind) && !sameDmxMapping) {
    for (uint8_t offset = 0; offset < dmxChannelCount(previous.kind); ++offset) dmx_.setSlot(previous.dmxChannel + offset, 0);
  }
  light.kind = kind; light.dmxChannel = dmxChannel;
  if (!sameDmxMapping) { light.level = 0; fades_[currentIndex] = Fade{}; }
  // A new colour light starts white: the W channel on RGBW, full R+G+B on RGB.
  const RgbwColor previousColor = colors_[currentIndex];
  if (previous.kind != kind && kind == LightKind::DmxRgbw) colors_[currentIndex] = RgbwColor{};
  if (previous.kind != kind && kind == LightKind::DmxRgb) colors_[currentIndex] = RgbwColor{255, 255, 255, 0};
  name.toCharArray(light.name, sizeof(light.name));
  relayTarget.toCharArray(light.relayTarget, sizeof(light.relayTarget));
  if (!save()) { light = previous; colors_[currentIndex] = previousColor; return false; }
  return true;
}

bool LightManager::remove(uint8_t id) { const int index = indexForId(id); if (index < 0) return false; const LightSnapshot old = lights_[index]; const uint8_t oldTemperature = colorTemperatures_[index]; const RgbwColor oldColor = colors_[index]; const DimmingProfile oldDimming = dimming_[index]; lights_[index] = LightSnapshot{}; colorTemperatures_[index] = 50; colors_[index] = RgbwColor{}; dimming_[index] = DimmingProfile{}; fades_[index] = Fade{}; lastOn_[index] = 0; const LightBehaviorOverride oldOverride = overrides_[index]; overrides_[index] = LightBehaviorOverride{}; if (isDmxKind(old.kind)) for (uint8_t offset = 0; offset < dmxChannelCount(old.kind); ++offset) dmx_.setSlot(old.dmxChannel + offset, 0); if (save()) return true; lights_[index] = old; colorTemperatures_[index] = oldTemperature; colors_[index] = oldColor; dimming_[index] = oldDimming; overrides_[index] = oldOverride; return false; }

bool LightManager::setLevel(uint8_t id, uint8_t level, int colorTemperature, const RgbwColor* color) {
  const bool applied = applyLevel(id, level, colorTemperature, color);
  if (applied) notifyChange(kTopicLights | kTopicActions);
  return applied;
}

bool LightManager::applyLevel(uint8_t id, uint8_t level, int colorTemperature, const RgbwColor* color) {
  const int index = indexForId(id); if (index < 0) return false;
  LightSnapshot& light = lights_[index];
  if (light.kind == LightKind::RelaySwitch) {
    if (!setRelayTarget(light.relayTarget, level > 0)) return false;
    light.level = level > 0 ? 255 : 0;
    stateDirty_ = true; stateChangedAt_ = millis();
    return true;
  }
  if (!isDmxKind(light.kind)) return false;

  // Colour and colour temperature apply at once; only the brightness fades.
  bool appearanceChanged = false;
  if (light.kind == LightKind::DmxCct && colorTemperature >= 0) {
    if (colorTemperature > 100) return false;
    colorTemperatures_[index] = static_cast<uint8_t>(colorTemperature);
    if (!chaos_.active) {  // chaos colours are never written to flash
      whisli::system::ConfigStore preferences;
      preferences.begin("whisli-lights", false);
      putIfChanged(preferences, "cct", colorTemperatures_, sizeof(colorTemperatures_));
      preferences.end();
    }
    appearanceChanged = true;
  }
  if (isColorKind(light.kind) && color) {
    colors_[index] = *color;
    if (light.kind == LightKind::DmxRgb) colors_[index].white = 0;
    if (!chaos_.active && !saveColors()) return false;
    appearanceChanged = true;
  }
  if (level != light.level) {
    light.level = level;
    if (level > 0) lastOn_[index] = level;
    stateDirty_ = true; stateChangedAt_ = millis();
    startTransition(index, level);
  } else if (appearanceChanged) {
    writeDmx(index, fades_[index].current);
  }
  return true;
}

bool LightManager::turnOn(uint8_t id) {
  const int index = indexForId(id); if (index < 0) return false;
  if (!isDmxKind(lights_[index].kind)) return setLevel(id, 255);
  const LightBehavior behavior = effectiveBehavior(index);
  const uint8_t fixed = static_cast<uint8_t>(behavior.onLevelPercent * 255 / 100);
  const uint8_t level = behavior.onLevelLast && lastOn_[index] > 0 ? lastOn_[index] : (fixed > 0 ? fixed : 1);
  return setLevel(id, level);
}

void LightManager::startTransition(uint8_t index, uint8_t target) {
  Fade& fade = fades_[index];
  const LightBehavior behavior = effectiveBehavior(index);
  const bool brighter = target > fade.current;
  // A fade takes its full time whatever the distance, like the fade time of a DALI dimmer.
  const uint16_t duration = brighter ? (behavior.fadeIn ? behavior.fadeInMs : 0) : (behavior.fadeOut ? behavior.fadeOutMs : 0);
  if (duration == 0 || target == fade.current) {
    fade.active = false;
    fade.current = target;
    writeDmx(index, target);
    return;
  }
  fade.from = fade.current;
  fade.startedAt = millis();
  fade.durationMs = duration;
  fade.active = true;
}

// Chaos mode: every DMX light changes brightness and colour at random until switched off, then
// everything returns to how it was. Runs from loop() at the lowest priority.
void LightManager::setChaos(bool on) {
  if (on == chaos_.active) return;
  if (on) {
    for (uint8_t index = 0; index < kMaxLights; ++index) {
      chaos_.level[index] = lights_[index].level;
      chaos_.temperature[index] = colorTemperatures_[index];
      chaos_.color[index] = colors_[index];
      chaos_.lastOn[index] = lastOn_[index];
      chaos_.nextAt[index] = millis() + esp_random() % 600;
    }
    chaos_.active = true;
  } else {
    chaos_.active = false;
    for (uint8_t index = 0; index < kMaxLights; ++index) {
      const LightSnapshot& light = lights_[index];
      if (!light.configured || !isDmxKind(light.kind)) continue;
      // The same values that are stored, so nothing new is written.
      applyLevel(light.id, chaos_.level[index], light.kind == LightKind::DmxCct ? chaos_.temperature[index] : -1,
                 isColorKind(light.kind) ? &chaos_.color[index] : nullptr);
      lastOn_[index] = chaos_.lastOn[index];
    }
  }
  notifyChange(kTopicLights | kTopicActions);
}

void LightManager::chaosTick() {
  if (!chaos_.active) return;
  const uint32_t now = millis();
  bool changed = false;
  for (uint8_t index = 0; index < kMaxLights; ++index) {
    const LightSnapshot& light = lights_[index];
    if (!light.configured || !isDmxKind(light.kind) || static_cast<int32_t>(now - chaos_.nextAt[index]) < 0) continue;
    chaos_.nextAt[index] = now + 600 + esp_random() % 2000;
    // Mostly bright, sometimes dark for a moment.
    const uint8_t level = esp_random() % 6 == 0 ? 0 : static_cast<uint8_t>(70 + esp_random() % 186);
    // A fully saturated colour: one channel full, one off, one anywhere between (a hue on the colour wheel).
    RgbwColor color{0, 0, 0, 0};
    uint8_t* channels[3] = {&color.red, &color.green, &color.blue};
    const uint8_t full = esp_random() % 3;
    const uint8_t off = (full + 1 + esp_random() % 2) % 3;
    *channels[full] = 255;
    *channels[3 - full - off] = static_cast<uint8_t>(esp_random() % 256);
    applyLevel(light.id, level, light.kind == LightKind::DmxCct ? static_cast<int>(esp_random() % 101) : -1, isColorKind(light.kind) ? &color : nullptr);
    changed = true;
  }
  if (changed) notifyChange(kTopicLights);
}

bool LightManager::step(uint8_t id, bool brighter, uint8_t percent) {
  const int index = indexForId(id);
  if (index < 0 || !isDmxKind(lights_[index].kind) || percent == 0 || percent > 100) return false;
  const int amount = (percent * 255 + 50) / 100;
  const int level = lights_[index].level;
  if (!brighter && level == 0) return true;  // already off: nothing to dim
  const int next = brighter ? min(255, level + amount) : max(0, level - amount);
  return setLevel(id, static_cast<uint8_t>(next));
}

void LightManager::tick() {
  const uint32_t now = millis();
  for (uint8_t index = 0; index < kMaxLights; ++index) {
    Fade& fade = fades_[index];
    if (!fade.active || !lights_[index].configured) continue;
    const uint8_t target = lights_[index].level;
    const uint32_t elapsed = now - fade.startedAt;
    if (elapsed >= fade.durationMs) {
      fade.current = target;
      fade.active = false;
    } else {
      // Interpolated on the logical level, so the dimming curve makes the fade look even to the eye.
      fade.current = static_cast<uint8_t>(fade.from + (static_cast<int>(target) - fade.from) * static_cast<int>(elapsed) / fade.durationMs);
    }
    writeDmx(index, fade.current);
  }
}

bool LightManager::writeDmx(uint8_t index, uint8_t level) {
  const LightSnapshot& light = lights_[index];
  const uint8_t out = output(index, level);
  if (light.kind == LightKind::DmxDimmable) return dmx_.setSlot(light.dmxChannel, out);
  if (light.kind == LightKind::DmxCct) {
    const uint8_t mix = colorTemperatures_[index];
    const uint8_t warm = static_cast<uint8_t>(static_cast<uint16_t>(out) * (100 - mix) / 100);
    const uint8_t cool = static_cast<uint8_t>(static_cast<uint16_t>(out) * mix / 100);
    return dmx_.setSlot(light.dmxChannel, warm) && dmx_.setSlot(light.dmxChannel + 1, cool);
  }
  const RgbwColor& color = colors_[index];
  const uint8_t values[4] = {scaled(color.red, out), scaled(color.green, out), scaled(color.blue, out), scaled(color.white, out)};
  for (uint8_t offset = 0; offset < dmxChannelCount(light.kind); ++offset) if (!dmx_.setSlot(light.dmxChannel + offset, values[offset])) return false;
  return true;
}

LightBehavior LightManager::effectiveBehavior(uint8_t index) const {
  LightBehavior result = defaults_;
  const LightBehaviorOverride& own = overrides_[index];
  if (own.fields & kBehaviorFadeIn) { result.fadeIn = own.values.fadeIn; result.fadeInMs = own.values.fadeInMs; }
  if (own.fields & kBehaviorFadeOut) { result.fadeOut = own.values.fadeOut; result.fadeOutMs = own.values.fadeOutMs; }
  if (own.fields & kBehaviorOnLevel) { result.onLevelLast = own.values.onLevelLast; result.onLevelPercent = own.values.onLevelPercent; }
  if (own.fields & kBehaviorMax) result.maxPercent = own.values.maxPercent;
  if (own.fields & kBehaviorRestore) result.restoreAfterBoot = own.values.restoreAfterBoot;
  return result;
}

LightBehavior LightManager::behavior(uint8_t id) const {
  const int index = indexForId(id);
  return index < 0 ? defaults_ : effectiveBehavior(index);
}

LightBehaviorOverride LightManager::behaviorOverride(uint8_t id) const {
  const int index = indexForId(id);
  return index < 0 ? LightBehaviorOverride{} : overrides_[index];
}

bool LightManager::setDefaults(const LightBehavior& behavior) {
  const LightBehavior previous = defaults_;
  defaults_ = behavior;
  if (!saveBehavior()) { defaults_ = previous; return false; }
  for (uint8_t index = 0; index < kMaxLights; ++index) if (lights_[index].configured && isDmxKind(lights_[index].kind)) writeDmx(index, fades_[index].current);
  notifyChange(kTopicLights);
  return true;
}

bool LightManager::setBehaviorOverride(uint8_t id, const LightBehaviorOverride& override) {
  const int index = indexForId(id); if (index < 0) return false;
  const LightBehaviorOverride previous = overrides_[index];
  overrides_[index] = override;
  if (!saveBehavior()) { overrides_[index] = previous; return false; }
  if (isDmxKind(lights_[index].kind)) writeDmx(index, fades_[index].current);
  notifyChange(kTopicLights);
  return true;
}

bool LightManager::saveBehavior() {
  whisli::system::ConfigStore preferences;
  preferences.begin("whisli-lights", false);
  const bool saved = putIfChanged(preferences, "behav", &defaults_, sizeof(defaults_)) &&
                     putIfChanged(preferences, "behovr", overrides_, sizeof(overrides_));
  preferences.end();
  return saved;
}

bool LightManager::takeStateToSave(SavedState& output, bool now) {
  // Wait for a quiet moment so a burst of button presses costs one flash write (unless asked now).
  if (!now && (!stateDirty_ || millis() - stateChangedAt_ < 3000)) return false;
  stateDirty_ = false;
  for (uint8_t index = 0; index < kMaxLights; ++index) {
    output.level[index] = lights_[index].configured ? withLiveLevel(lights_[index]).level : 0;
    output.lastOn[index] = lastOn_[index];
  }
  return true;
}

void LightManager::saveState(const SavedState& state) {
  whisli::system::ConfigStore preferences;
  preferences.begin("whisli-lights", false);
  putIfChanged(preferences, "state", &state, sizeof(state));
  preferences.end();
}

uint8_t LightManager::colorTemperature(uint8_t id) const {
  const int index = indexForId(id);
  return index < 0 ? 50 : colorTemperatures_[index];
}

RgbwColor LightManager::color(uint8_t id) const {
  const int index = indexForId(id);
  return index < 0 ? RgbwColor{} : colors_[index];
}

uint8_t LightManager::output(uint8_t index, uint8_t level) const {
  if (level == 0) return 0;
  const DimmingProfile& profile = dimming_[index];
  const float x = level / 255.0f;
  float y = x;
  if (profile.curve == DimCurve::Square) y = x * x;
  else if (profile.curve == DimCurve::Perceptual) {
    // CIE 1931: equal slider steps give equal steps of perceived lightness.
    const float lightness = x * 100.0f;
    y = lightness <= 8.0f ? lightness / 903.3f : powf((lightness + 16.0f) / 116.0f, 3.0f);
  }
  const float minimum = profile.minimumPercent * 255.0f / 100.0f;
  const float maximum = effectiveBehavior(index).maxPercent * 255.0f / 100.0f;
  const long value = lroundf(minimum + (maximum > minimum ? maximum - minimum : 0.0f) * y);
  return static_cast<uint8_t>(value < 1 ? 1 : value > 255 ? 255 : value);
}

bool LightManager::setDimming(uint8_t id, uint8_t minimumPercent, DimCurve curve) {
  const int index = indexForId(id);
  if (index < 0 || !isDmxKind(lights_[index].kind) || minimumPercent > 50 || static_cast<uint8_t>(curve) > 2) return false;
  const DimmingProfile previous = dimming_[index];
  dimming_[index] = DimmingProfile{minimumPercent, curve};
  whisli::system::ConfigStore preferences;
  preferences.begin("whisli-lights", false);
  const bool saved = putIfChanged(preferences, "dim", dimming_, sizeof(dimming_));
  preferences.end();
  if (!saved) { dimming_[index] = previous; return false; }
  writeDmx(index, fades_[index].current);  // re-apply the current level with the new profile
  notifyChange(kTopicLights);
  return true;
}

DimmingProfile LightManager::dimming(uint8_t id) const {
  const int index = indexForId(id);
  return index < 0 ? DimmingProfile{} : dimming_[index];
}

bool LightManager::saveColors() {
  whisli::system::ConfigStore preferences;
  preferences.begin("whisli-lights", false);
  const bool saved = putIfChanged(preferences, "rgbw", colors_, sizeof(colors_));
  preferences.end();
  return saved;
}

bool LightManager::relayTargetAvailable(const String& target) const {
  if (target.startsWith("controller.output-")) { uint8_t channel; return parseNumber(target.substring(18), channel) && channel <= kControllerOutputCount && controller_.snapshot().outputUsed[channel - 1]; }
  if (!target.startsWith("relay-")) return false;
  const int separator = target.indexOf(".output-"); uint8_t device; uint8_t channel;
  if (separator < 6 || !parseNumber(target.substring(6, separator), device) || !parseNumber(target.substring(separator + 8), channel)) return false;
  const RelaySnapshot relay = relays_.snapshot(device);
  return relay.configured && relay.active && channel <= relay.channelCount && relay.channelUsed[channel - 1];
}

bool LightManager::setRelayTarget(const String& target, bool on) {
  if (!relayTargetAvailable(target)) return false;
  if (target.startsWith("controller.output-")) { uint8_t channel; return parseNumber(target.substring(18), channel) && controller_.setOutput(channel - 1, on); }
  const int separator = target.indexOf(".output-"); uint8_t device; uint8_t channel;
  return parseNumber(target.substring(6, separator), device) && parseNumber(target.substring(separator + 8), channel) && relays_.setChannel(device, channel - 1, on);
}

bool LightManager::save() {
  uint8_t count = 0;
  for (const LightSnapshot& light : lights_) if (light.configured) ++count;
  const size_t size = 2 + sizeof(LightSnapshot) * count;
  std::unique_ptr<uint8_t[]> compact(new uint8_t[size]);
  compact[0] = kCompactVersion;
  compact[1] = count;
  size_t offset = 2;
  for (const LightSnapshot& light : lights_) {
    if (!light.configured) continue;
    memcpy(compact.get() + offset, &light, sizeof(light));
    offset += sizeof(light);
  }
  whisli::system::ConfigStore preferences; preferences.begin("whisli-lights", false);
  const bool configSaved = putIfChanged(preferences, "lights", compact.get(), size);
  if (configSaved && preferences.isKey("config")) preferences.remove("config");  // the old full layout
  const bool temperaturesSaved = putIfChanged(preferences, "cct", colorTemperatures_, sizeof(colorTemperatures_));
  const bool colorsSaved = putIfChanged(preferences, "rgbw", colors_, sizeof(colors_));
  const bool dimmingSaved = putIfChanged(preferences, "dim", dimming_, sizeof(dimming_));
  const bool success = configSaved && temperaturesSaved && colorsSaved && dimmingSaved;
  preferences.end();
  if (!success) whisli::system::write(whisli::system::LogLevel::Error, "lights", configSaved ? "Could not save light settings" : "Could not save light definitions");
  if (success) notifyChange(kTopicLights | kTopicActions);
  return success;
}

}  // namespace whisli::app
