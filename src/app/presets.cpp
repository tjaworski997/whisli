#include "whisli/app/presets.hpp"

#include <Preferences.h>

#include "whisli/app/state_events.hpp"

namespace whisli::app {
namespace {

constexpr char kNamespace[] = "whisli-presets";
constexpr uint16_t kWarmKelvin = 2700;
constexpr uint16_t kCoolKelvin = 6500;

String clean(const String& name) {
  String value = name;
  value.trim();
  return value;
}

}  // namespace

void PresetManager::begin() {
  Preferences preferences;
  preferences.begin(kNamespace, true);
  const bool stored = preferences.isKey("count");
  count_ = preferences.getUChar("count", 0);
  if (count_ > kMaxPresets || (count_ && preferences.getBytes("presets", presets_, sizeof(LightPreset) * count_) != sizeof(LightPreset) * count_)) count_ = 0;
  preferences.end();
  if (stored) return;
  const struct { const char* name; uint16_t kelvin; } whites[] = {{"Ciepła", 2700}, {"Neutralna", 4000}, {"Dzienna", 5000}, {"Zimna", 6500}};
  for (const auto& white : whites) {
    LightPreset& preset = presets_[count_];
    preset = LightPreset{};
    preset.id = count_ + 1;
    strlcpy(preset.name, white.name, sizeof(preset.name));
    preset.kelvin = white.kelvin;
    ++count_;
  }
  save();
}

bool PresetManager::save() {
  Preferences preferences;
  preferences.begin(kNamespace, false);
  bool ok = preferences.putUChar("count", count_) == 1;
  if (count_) ok = ok && preferences.putBytes("presets", presets_, sizeof(LightPreset) * count_) == sizeof(LightPreset) * count_;
  else preferences.remove("presets");
  preferences.end();
  notifyChange(kTopicLights | kTopicActions);
  return ok;
}

const LightPreset* PresetManager::find(uint8_t id) const {
  for (uint8_t index = 0; index < count_; ++index) if (presets_[index].id == id) return &presets_[index];
  return nullptr;
}

uint8_t PresetManager::create(PresetKind kind, const String& rawName) {
  const String name = clean(rawName);
  if (count_ >= kMaxPresets || name.isEmpty() || name.length() >= sizeof(LightPreset::name)) return 0;
  uint8_t id = 1;
  while (find(id)) ++id;
  LightPreset& preset = presets_[count_++];
  preset = LightPreset{};
  preset.id = id;
  preset.kind = kind;
  strlcpy(preset.name, name.c_str(), sizeof(preset.name));
  if (kind == PresetKind::Color) preset.color = RgbwColor{255, 140, 0, 0};  // something visible to start from
  if (save()) return id;
  --count_;
  return 0;
}

bool PresetManager::update(uint8_t id, const String& rawName, uint16_t kelvin, const RgbwColor* color) {
  LightPreset* preset = const_cast<LightPreset*>(find(id));
  if (!preset) return false;
  const LightPreset previous = *preset;
  if (!rawName.isEmpty()) {
    const String name = clean(rawName);
    if (name.isEmpty() || name.length() >= sizeof(LightPreset::name)) return false;
    strlcpy(preset->name, name.c_str(), sizeof(preset->name));
  }
  if (kelvin) {
    if (preset->kind != PresetKind::Temperature || kelvin < kWarmKelvin || kelvin > kCoolKelvin) { *preset = previous; return false; }
    preset->kelvin = kelvin;
  }
  if (color) {
    if (preset->kind != PresetKind::Color) { *preset = previous; return false; }
    preset->color = *color;
  }
  if (save()) return true;
  *preset = previous;
  return false;
}

bool PresetManager::remove(uint8_t id) {
  for (uint8_t index = 0; index < count_; ++index) {
    if (presets_[index].id != id) continue;
    for (uint8_t next = index + 1; next < count_; ++next) presets_[next - 1] = presets_[next];
    --count_;
    return save();
  }
  return false;
}

bool PresetManager::fits(uint8_t presetId, LightKind kind) const {
  const LightPreset* preset = find(presetId);
  if (!preset) return false;
  return preset->kind == PresetKind::Temperature ? kind == LightKind::DmxCct : kind == LightKind::DmxRgb || kind == LightKind::DmxRgbw;
}

bool PresetManager::apply(uint8_t presetId, uint8_t lightId) {
  LightSnapshot light;
  if (!lights_.find(lightId, light) || !fits(presetId, light.kind)) return false;
  const LightPreset& preset = *find(presetId);
  // The same level as now: only the colour changes, and an off light stays off with its new colour.
  if (preset.kind == PresetKind::Temperature) return lights_.setLevel(lightId, light.level, kelvinToMix(preset.kelvin));
  return lights_.setLevel(lightId, light.level, -1, &preset.color);
}

uint8_t PresetManager::kelvinToMix(uint16_t kelvin) {
  const float warm = 1e6f / kWarmKelvin;
  const float cool = 1e6f / kCoolKelvin;
  const float mix = (warm - 1e6f / constrain(kelvin, kWarmKelvin, kCoolKelvin)) / (warm - cool) * 100.0f;
  return static_cast<uint8_t>(constrain(lroundf(mix), 0L, 100L));
}

}  // namespace whisli::app
