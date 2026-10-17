#pragma once

#include <Arduino.h>

#include "whisli/app/lights.hpp"

namespace whisli::app {

constexpr uint8_t kMaxPresets = 64;  // both kinds together

// A named light colour to reuse: a white temperature for CCT lights or a colour for RGB/RGBW ones.
// Applying one changes only the colour, never the brightness, and does not switch a light on.
enum class PresetKind : uint8_t { Temperature = 0, Color = 1 };
struct LightPreset {
  uint8_t id = 0;
  PresetKind kind = PresetKind::Temperature;
  char name[25] = {};
  uint16_t kelvin = 2700;  // Temperature
  RgbwColor color;         // Color
};

class PresetManager {
 public:
  explicit PresetManager(LightManager& lights) : lights_(lights) {}
  // First start: a few common whites, so there is something to pick.
  void begin();
  uint8_t count() const { return count_; }
  const LightPreset& at(uint8_t index) const { return presets_[index]; }
  const LightPreset* find(uint8_t id) const;

  uint8_t create(PresetKind kind, const String& name);  // new id, 0 on failure
  bool update(uint8_t id, const String& name, uint16_t kelvin, const RgbwColor* color);
  bool remove(uint8_t id);

  // Whether the preset suits the light: temperatures for CCT, colours for RGB/RGBW.
  bool fits(uint8_t presetId, LightKind kind) const;
  bool apply(uint8_t presetId, uint8_t lightId);

  // CCT lights mix their warm (2700 K) and cool (6500 K) channels by percent; mixing is close to
  // linear in mireds. The panel converts the same way.
  static uint8_t kelvinToMix(uint16_t kelvin);

 private:
  bool save();

  LightManager& lights_;
  LightPreset presets_[kMaxPresets];
  uint8_t count_ = 0;
};

}  // namespace whisli::app
