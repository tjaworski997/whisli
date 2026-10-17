#pragma once

#include <Arduino.h>

#include "whisli/app/button_input.hpp"
#include "whisli/app/controller_io.hpp"
#include "whisli/app/http_hooks.hpp"
#include "whisli/app/lights.hpp"
#include "whisli/app/modbus_relays.hpp"
#include "whisli/app/presets.hpp"

namespace whisli::app {

// Generous on purpose: the configuration lives in files and these tables in PSRAM, so the limits
// only guard against runaway growth (e.g. 128 buttons with every gesture used).
constexpr uint16_t kMaxInputActions = 1024;
constexpr uint16_t kMaxInputEvents = 640;

enum class ActionTrigger : uint8_t { Click, DoubleClick, TripleClick, LongClick, Hold };
// Keep the values: they are stored. The dimming ones work on DMX lights only.
enum class ActionCommand : uint8_t {
  Toggle, TurnOn, TurnOff, None,
  StepUp, StepDown,                  // one step of `value` percent; down may switch off
  RetiredHoldDim, RetiredHoldUp, RetiredHoldDown,  // dimming while held: removed, such actions do nothing
  SetLevel,                          // on at `value` percent
  SetPreset,                         // colour or white of preset `value`; brightness unchanged
  RunHook,                           // sends an HTTP integration ("hook-<id>")
};

struct InputAction {
  uint16_t id = 0;
  char inputId[32] = {};
  ActionTrigger trigger = ActionTrigger::Click;
  ActionCommand command = ActionCommand::None;
  char targetId[32] = {};
  bool enabled = true;
  // Step or level in percent, or a preset id. It fills what used to be padding, so the stored layout keeps its size;
  // older actions have no meaningful value there, but their commands never read it.
  uint8_t value = 0;
};
static_assert(sizeof(InputAction) == 70, "stored action layout changed: add a migration");

struct InputEventBinding {
  uint16_t id = 0;
  char inputId[32] = {};
  ActionTrigger trigger = ActionTrigger::Click;
};

class InputActionManager {
 public:
  InputActionManager(ControllerIo& controller, ModbusRelayManager& relays, LightManager& lights, PresetManager& presets, HttpHookManager& hooks);

  void begin();
  uint16_t count() const;
  bool get(uint16_t index, InputAction& output) const;
  uint16_t eventCount() const;
  bool getEvent(uint16_t index, InputEventBinding& output) const;
  bool addEvent(const String& inputId, const String& trigger);
  bool removeEvent(uint16_t id);
  bool add(const String& inputId, const String& trigger, const String& command, const String& targetId, bool enabled = true, int value = -1);
  bool update(uint16_t id, const String& command, const String& targetId, int value = -1);
  bool setEnabled(uint16_t id, bool enabled);
  bool remove(uint16_t id);
  bool test(uint16_t id);
  // A removed preset: actions that used it are left without one (and do nothing until one is chosen).
  void forgetPreset(uint8_t presetId);
  void handleInputEvent(uint8_t input, ButtonEvent event);

  static bool parseTrigger(const String& value, ActionTrigger& output);
  static bool parseCommand(const String& value, ActionCommand& output);
  static bool usesValue(ActionCommand command) { return command == ActionCommand::StepUp || command == ActionCommand::StepDown || command == ActionCommand::SetLevel || command == ActionCommand::SetPreset; }
  static bool dimmingOnly(ActionCommand command) { return command >= ActionCommand::StepUp && command <= ActionCommand::SetPreset; }
  static const char* triggerName(ActionTrigger trigger);
  static const char* commandName(ActionCommand command);

 private:
  // Stored as [header][actions][events]. Version 3 (settings storage, up to 255 of each) is read to
  // migrate; version 4 has 16-bit counts.
  struct PersistedHeaderV3 {
    uint8_t version = 3;
    uint8_t count = 0;
    uint16_t nextId = 1;
    uint8_t eventCount = 0;
    uint16_t nextEventId = 1;
  };
  struct PersistedHeader {
    uint8_t version = 4;
    uint16_t count = 0;
    uint16_t nextId = 1;
    uint16_t eventCount = 0;
    uint16_t nextEventId = 1;
  };

  bool save();
  bool eventExists(const String& inputId, ActionTrigger trigger) const;
  bool inputAvailable(const String& inputId) const;
  bool targetAvailable(const String& targetId) const;
  // Whether `command` with `value` can drive `targetId`: brightness needs a DMX light, a preset a
  // light of its kind (white for CCT, colour for RGB/RGBW).
  bool fits(ActionCommand command, uint8_t value, const String& targetId) const;
  static uint8_t valueFor(ActionCommand command, int requested, uint8_t current);
  bool execute(const InputAction& action);

  ControllerIo& controller_;
  ModbusRelayManager& relays_;
  LightManager& lights_;
  PresetManager& presets_;
  HttpHookManager& hooks_;
  // Tens of kilobytes: allocated, so they land in PSRAM rather than the scarce internal RAM.
  InputAction* actions_;
  uint16_t count_ = 0;
  uint16_t nextId_ = 1;
  InputEventBinding* events_;
  uint16_t eventCount_ = 0;
  uint16_t nextEventId_ = 1;
};

}  // namespace whisli::app
