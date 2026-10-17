#pragma once

#include <Arduino.h>

#include "whisli/app/controller_io.hpp"
#include "whisli/app/dmx_output.hpp"
#include "whisli/app/modbus_relays.hpp"

namespace whisli::app {

// Ids are 1…kMaxLights (the slot + 1). 128 RGBW lights already fill a DMX universe (512 channels).
constexpr uint8_t kMaxLights = 128;

// Keep 0/1 stable: early firmware already persisted these values.
enum class LightKind : uint8_t { RelaySwitch = 0, DmxDimmable = 1, None = 2, DmxCct = 3, DmxRgbw = 4, DmxRgb = 5 };

// Colour of an RGB/RGBW light at full brightness; the level scales every channel. RGB lights ignore white.
struct RgbwColor { uint8_t red = 0; uint8_t green = 0; uint8_t blue = 0; uint8_t white = 255; };

// How the requested level (0–255) maps to DMX output. Perceptual follows CIE 1931 lightness.
enum class DimCurve : uint8_t { Linear = 0, Perceptual = 1, Square = 2 };

// Per-lamp dimming: the lowest non-zero level starts at `minimumPercent`, so lamps that cut out
// below some voltage use the whole slider. Stored apart from LightSnapshot to keep its persisted layout.
struct DimmingProfile { uint8_t minimumPercent = 0; DimCurve curve = DimCurve::Linear; };

// How a light behaves beyond its level. The global defaults apply to every light; a light may override
// any group of them (see LightBehaviorOverride). Relay lights only use `restoreAfterBoot`.
struct LightBehavior {
  bool fadeIn = true;           // brightening is a smooth transition…
  uint16_t fadeInMs = 400;      // …taking this long from off to the target
  bool fadeOut = true;
  uint16_t fadeOutMs = 800;
  bool onLevelLast = true;      // "on" returns to the last brightness, otherwise to onLevelPercent
  uint8_t onLevelPercent = 100;
  uint8_t maxPercent = 100;     // full slider maps to this share of the output
  bool restoreAfterBoot = false;  // after a restart: as before it, otherwise off
};

// Which groups of LightBehavior a light overrides.
enum LightBehaviorField : uint8_t {
  kBehaviorFadeIn = 1 << 0,
  kBehaviorFadeOut = 1 << 1,
  kBehaviorOnLevel = 1 << 2,
  kBehaviorMax = 1 << 3,
  kBehaviorRestore = 1 << 4,
};
struct LightBehaviorOverride { uint8_t fields = 0; LightBehavior values; };

struct LightSnapshot {
  bool configured = false;
  uint8_t id = 0;
  LightKind kind = LightKind::RelaySwitch;
  char name[33] = {};
  char relayTarget[32] = {};
  uint16_t dmxChannel = 0;
  uint8_t level = 0;
};

class LightManager {
 public:
  LightManager(ControllerIo& controller, ModbusRelayManager& relays, DmxOutput& dmx);
  void begin();
  uint8_t count() const;
  bool get(uint8_t index, LightSnapshot& output) const;
  bool find(uint8_t id, LightSnapshot& output) const;
  // Adds an unconfigured light (no type yet); returns its id, 0 when the list is full.
  uint8_t create();
  // `reason`, when given, receives why an update was refused (for the user).
  bool update(uint8_t id, const String& name, const String& kind, const String& relayTarget, uint16_t dmxChannel, String* reason = nullptr);
  bool remove(uint8_t id);
  bool setLevel(uint8_t id, uint8_t level, int colorTemperature = -1, const RgbwColor* color = nullptr);
  uint8_t colorTemperature(uint8_t id) const;
  RgbwColor color(uint8_t id) const;
  bool setDimming(uint8_t id, uint8_t minimumPercent, DimCurve curve);
  DimmingProfile dimming(uint8_t id) const;

  // "On" at the configured on-level (last brightness or a fixed one).
  bool turnOn(uint8_t id);
  // DMX lights: one step brighter or darker by `percent` of the range. Darker may go down to off;
  // brighter from off starts at the step itself.
  bool step(uint8_t id, bool brighter, uint8_t percent);
  // Advances fades; called by the control task every cycle.
  void tick();

  // Chaos mode: random brightness and colours on every DMX light until switched off, then all
  // back as before. Nothing of it is written to flash. chaosTick() runs from loop().
  bool chaos() const { return chaos_.active; }
  void setChaos(bool on);
  void chaosTick();

  const LightBehavior& defaults() const { return defaults_; }
  bool setDefaults(const LightBehavior& behavior);
  LightBehaviorOverride behaviorOverride(uint8_t id) const;
  bool setBehaviorOverride(uint8_t id, const LightBehaviorOverride& override);
  LightBehavior behavior(uint8_t id) const;

  // Levels to restore after a restart are written in the background, never on the control path:
  // takeStateToSave() copies them under the state lock, saveState() writes flash without it.
  struct SavedState { uint8_t level[kMaxLights] = {}; uint8_t lastOn[kMaxLights] = {}; };
  bool takeStateToSave(SavedState& output, bool now = false);
  static void saveState(const SavedState& state);

 private:
  bool save();
  bool applyLevel(uint8_t id, uint8_t level, int colorTemperature, const RgbwColor* color);
  int indexForId(uint8_t id) const;
  bool relayTargetAvailable(const String& target) const;
  bool relayTargetOn(const String& target) const;
  LightSnapshot withLiveLevel(const LightSnapshot& light) const;
  bool setRelayTarget(const String& target, bool on);
  bool saveColors();
  uint8_t output(uint8_t index, uint8_t level) const;
  // Writes the light's DMX slots for a logical level (curve, minimum, maximum, colour applied).
  bool writeDmx(uint8_t index, uint8_t level);
  // Moves a DMX light towards `target`, fading if its behaviour says so.
  void startTransition(uint8_t index, uint8_t target);
  LightBehavior effectiveBehavior(uint8_t index) const;
  bool saveBehavior();

  ControllerIo& controller_;
  ModbusRelayManager& relays_;
  DmxOutput& dmx_;
  // Fades: the level being output now moves from `fadeFrom_` to the light's target level.
  struct Fade { uint8_t from = 0; uint8_t current = 0; uint32_t startedAt = 0; uint16_t durationMs = 0; bool active = false; };
  struct Chaos {
    bool active = false;
    uint8_t level[kMaxLights] = {};
    uint8_t temperature[kMaxLights] = {};
    RgbwColor color[kMaxLights] = {};
    uint8_t lastOn[kMaxLights] = {};
    uint32_t nextAt[kMaxLights] = {};
  };
  // Per-light tables (~20 KB): allocated, so they land in PSRAM rather than the scarce internal RAM.
  // The members below are references into it and read like plain arrays.
  struct Tables {
    LightSnapshot lights[kMaxLights] = {};
    uint8_t colorTemperatures[kMaxLights] = {};
    RgbwColor colors[kMaxLights] = {};
    DimmingProfile dimming[kMaxLights] = {};
    LightBehaviorOverride overrides[kMaxLights] = {};
    Fade fades[kMaxLights] = {};
    Chaos chaos;
    uint8_t lastOn[kMaxLights] = {};
  };
  Tables* tables_ = new Tables();
  LightSnapshot (&lights_)[kMaxLights] = tables_->lights;
  uint8_t (&colorTemperatures_)[kMaxLights] = tables_->colorTemperatures;
  RgbwColor (&colors_)[kMaxLights] = tables_->colors;
  DimmingProfile (&dimming_)[kMaxLights] = tables_->dimming;
  LightBehavior defaults_;
  LightBehaviorOverride (&overrides_)[kMaxLights] = tables_->overrides;
  Fade (&fades_)[kMaxLights] = tables_->fades;
  Chaos& chaos_ = tables_->chaos;
  uint8_t (&lastOn_)[kMaxLights] = tables_->lastOn;  // last non-zero level, for "on returns to last brightness"
  bool stateDirty_ = false;
  bool migrateDefinitions_ = false;  // loaded from the old full layout: save compactly
  uint32_t stateChangedAt_ = 0;
};

}  // namespace whisli::app
