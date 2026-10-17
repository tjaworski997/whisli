#include "whisli/app/input_actions.hpp"

#include <Preferences.h>
#include <cstdio>
#include <cstring>
#include "esp_err.h"
#include "nvs.h"
#include "whisli/app/state_events.hpp"
#include "whisli/system/log.hpp"

namespace whisli::app {
namespace {

constexpr char kPreferencesNamespace[] = "whisli-actions";
constexpr char kConfigKey[] = "config";

bool parseNumber(const String& value, uint8_t& output) {
  if (value.isEmpty()) return false;
  const long number = value.toInt();
  if (number < 1 || number > 255 || String(number) != value) return false;
  output = static_cast<uint8_t>(number);
  return true;
}

bool parseInputId(const String& value, uint8_t& channel) {
  constexpr char prefix[] = "controller.input-";
  if (!value.startsWith(prefix)) return false;
  return parseNumber(value.substring(sizeof(prefix) - 1), channel) && channel <= kControllerInputCount;
}

bool parseControllerOutputId(const String& value, uint8_t& channel) {
  constexpr char prefix[] = "controller.output-";
  if (!value.startsWith(prefix)) return false;
  return parseNumber(value.substring(sizeof(prefix) - 1), channel) && channel <= kControllerOutputCount;
}

bool parseRelayOutputId(const String& value, uint8_t& id, uint8_t& channel) {
  if (!value.startsWith("relay-")) return false;
  const int separator = value.indexOf(".output-");
  if (separator < 6) return false;
  return parseNumber(value.substring(6, separator), id) &&
      parseNumber(value.substring(separator + 8), channel);
}

}  // namespace

namespace {
// Light targets look like "light-3".
bool parseLightId(const String& targetId, uint8_t& id) {
  if (!targetId.startsWith("light-") || targetId.length() < 7) return false;
  for (unsigned int index = 6; index < targetId.length(); ++index) if (!isDigit(targetId[index])) return false;
  const long value = targetId.substring(6).toInt();
  if (value < 1 || value > 255) return false;
  id = static_cast<uint8_t>(value);
  return true;
}
}  // namespace

InputActionManager::InputActionManager(ControllerIo& controller, ModbusRelayManager& relays, LightManager& lights, PresetManager& presets)
    : controller_(controller), relays_(relays), lights_(lights), presets_(presets) {}

void InputActionManager::begin() {
  Preferences preferences;
  preferences.begin(kPreferencesNamespace, true);
  const size_t savedSize = preferences.getBytesLength(kConfigKey);
  bool migrate = false;
  if (savedSize == sizeof(PersistedActions)) {
    if (preferences.getBytes(kConfigKey, &persistedBuffer_, sizeof(persistedBuffer_)) == sizeof(persistedBuffer_) &&
        persistedBuffer_.version == 2 && persistedBuffer_.count <= kMaxInputActions &&
        persistedBuffer_.eventCount <= kMaxInputEvents && persistedBuffer_.nextId != 0 &&
        persistedBuffer_.nextEventId != 0) {
      count_ = persistedBuffer_.count;
      nextId_ = persistedBuffer_.nextId;
      eventCount_ = persistedBuffer_.eventCount;
      nextEventId_ = persistedBuffer_.nextEventId;
      for (uint8_t index = 0; index < count_; ++index) {
        if (persistedBuffer_.actions[index].id == 0 || persistedBuffer_.actions[index].inputId[0] == '\0') {
          count_ = 0;
          nextId_ = 1;
          eventCount_ = 0;
          nextEventId_ = 1;
          break;
        }
        actions_[index] = persistedBuffer_.actions[index];
      }
      for (uint8_t index = 0; index < eventCount_; ++index) {
        if (persistedBuffer_.events[index].id == 0 || persistedBuffer_.events[index].inputId[0] == '\0') {
          eventCount_ = 0;
          nextEventId_ = 1;
          break;
        }
        events_[index] = persistedBuffer_.events[index];
      }
      migrate = true;
    }
  } else if (savedSize == sizeof(PersistedActionsV1)) {
    if (preferences.getBytes(kConfigKey, &legacyBuffer_, sizeof(legacyBuffer_)) == sizeof(legacyBuffer_) &&
        legacyBuffer_.version == 1 && legacyBuffer_.count <= kMaxInputActions && legacyBuffer_.nextId != 0) {
      count_ = legacyBuffer_.count;
      nextId_ = legacyBuffer_.nextId;
      for (uint8_t index = 0; index < count_; ++index) {
        if (legacyBuffer_.actions[index].id == 0 || legacyBuffer_.actions[index].inputId[0] == '\0') {
          count_ = 0;
          nextId_ = 1;
          break;
        }
        actions_[index] = legacyBuffer_.actions[index];
      }
      migrate = true;
    }
  } else if (savedSize >= sizeof(PersistedActionsHeader) && savedSize <= sizeof(compactBuffer_)) {
    PersistedActionsHeader header;
    if (preferences.getBytes(kConfigKey, compactBuffer_, savedSize) == savedSize) {
      memcpy(&header, compactBuffer_, sizeof(header));
      const size_t expectedSize = sizeof(header) + sizeof(InputAction) * header.count +
          sizeof(InputEventBinding) * header.eventCount;
      if (header.version == 3 && header.count <= kMaxInputActions &&
          header.eventCount <= kMaxInputEvents && header.nextId != 0 &&
          header.nextEventId != 0 && savedSize == expectedSize) {
        count_ = header.count;
        nextId_ = header.nextId;
        eventCount_ = header.eventCount;
        nextEventId_ = header.nextEventId;
        size_t offset = sizeof(header);
        for (uint8_t index = 0; index < count_; ++index) {
          memcpy(&actions_[index], compactBuffer_ + offset, sizeof(InputAction));
          offset += sizeof(InputAction);
          if (actions_[index].id == 0 || actions_[index].inputId[0] == '\0') {
            count_ = 0; nextId_ = 1; eventCount_ = 0; nextEventId_ = 1; break;
          }
        }
        for (uint8_t index = 0; index < eventCount_; ++index) {
          memcpy(&events_[index], compactBuffer_ + offset, sizeof(InputEventBinding));
          offset += sizeof(InputEventBinding);
          if (events_[index].id == 0 || events_[index].inputId[0] == '\0') {
            eventCount_ = 0; nextEventId_ = 1; break;
          }
        }
      }
    }
  }
  preferences.end();

  if (migrate) migrateLegacyConfiguration();
}

uint8_t InputActionManager::count() const { return count_; }

bool InputActionManager::get(uint8_t index, InputAction& output) const {
  if (index >= count_) return false;
  output = actions_[index];
  return true;
}

uint8_t InputActionManager::eventCount() const { return eventCount_; }

bool InputActionManager::getEvent(uint8_t index, InputEventBinding& output) const {
  if (index >= eventCount_) return false;
  output = events_[index];
  return true;
}

bool InputActionManager::addEvent(const String& inputId, const String& triggerValue) {
  if (eventCount_ >= kMaxInputEvents || !inputAvailable(inputId)) return false;
  ActionTrigger trigger;
  if (!parseTrigger(triggerValue, trigger) || eventExists(inputId, trigger)) return false;

  InputEventBinding event;
  event.id = nextEventId_++;
  if (nextEventId_ == 0) nextEventId_ = 1;
  inputId.toCharArray(event.inputId, sizeof(event.inputId));
  event.trigger = trigger;
  events_[eventCount_++] = event;
  if (save()) return true;
  --eventCount_;
  events_[eventCount_] = InputEventBinding{};
  nextEventId_ = event.id;
  return false;
}

bool InputActionManager::removeEvent(uint16_t id) {
  uint8_t index = 0;
  while (index < eventCount_ && events_[index].id != id) ++index;
  if (index >= eventCount_) return false;

  const InputEventBinding removed = events_[index];
  static InputAction previousActions[kMaxInputActions];
  memcpy(previousActions, actions_, sizeof(actions_));
  const uint8_t previousActionCount = count_;

  uint8_t actionWriteIndex = 0;
  for (uint8_t actionReadIndex = 0; actionReadIndex < count_; ++actionReadIndex) {
    const InputAction& action = actions_[actionReadIndex];
    if (strcmp(action.inputId, removed.inputId) == 0 && action.trigger == removed.trigger) continue;
    if (actionWriteIndex != actionReadIndex) actions_[actionWriteIndex] = action;
    ++actionWriteIndex;
  }
  count_ = actionWriteIndex;
  for (uint8_t actionIndex = count_; actionIndex < previousActionCount; ++actionIndex)
    actions_[actionIndex] = InputAction{};

  for (uint8_t move = index + 1; move < eventCount_; ++move) events_[move - 1] = events_[move];
  events_[--eventCount_] = InputEventBinding{};
  if (save()) return true;

  memcpy(actions_, previousActions, sizeof(actions_));
  count_ = previousActionCount;
  for (uint8_t move = eventCount_; move > index; --move) events_[move] = events_[move - 1];
  events_[index] = removed;
  ++eventCount_;
  return false;
}

bool InputActionManager::add(const String& inputId, const String& triggerValue,
                             const String& commandValue, const String& targetId, bool enabled, int value) {
  if (count_ >= kMaxInputActions || !inputAvailable(inputId) ||
      (!targetId.isEmpty() && !targetAvailable(targetId))) return false;
  ActionTrigger trigger;
  ActionCommand command;
  if (!parseTrigger(triggerValue, trigger) || !parseCommand(commandValue, command) || !eventExists(inputId, trigger) ||
      !fits(command, valueFor(command, value, 0), targetId)) return false;

  InputAction action;
  action.id = nextId_++;
  if (nextId_ == 0) nextId_ = 1;
  inputId.toCharArray(action.inputId, sizeof(action.inputId));
  action.trigger = trigger;
  action.command = command;
  targetId.toCharArray(action.targetId, sizeof(action.targetId));
  action.enabled = enabled;
  action.value = valueFor(command, value, 0);
  actions_[count_++] = action;
  if (save()) return true;
  --count_;
  nextId_ = action.id;
  return false;
}

bool InputActionManager::setEnabled(uint16_t id, bool enabled) {
  for (uint8_t index = 0; index < count_; ++index) {
    if (actions_[index].id != id) continue;
    const bool previous = actions_[index].enabled;
    actions_[index].enabled = enabled;
    if (save()) return true;
    actions_[index].enabled = previous;
    return false;
  }
  return false;
}

bool InputActionManager::update(uint16_t id, const String& commandValue, const String& targetId, int value) {
  ActionCommand command;
  if (!parseCommand(commandValue, command) ||
      (!targetId.isEmpty() && !targetAvailable(targetId))) return false;
  for (uint8_t index = 0; index < count_; ++index) {
    if (actions_[index].id != id) continue;
    const InputAction previous = actions_[index];
    const uint8_t newValue = valueFor(command, value, previous.command == command ? previous.value : 0);
    if (!fits(command, newValue, targetId)) return false;
    actions_[index].command = command;
    targetId.toCharArray(actions_[index].targetId, sizeof(actions_[index].targetId));
    actions_[index].value = newValue;
    if (save()) return true;
    actions_[index] = previous;
    return false;
  }
  return false;
}

// A step defaults to 10 %, a level to 100 %, a preset to none; a value already set stays when none is sent.
uint8_t InputActionManager::valueFor(ActionCommand command, int requested, uint8_t current) {
  if (!usesValue(command)) return 0;
  if (command == ActionCommand::SetPreset) return requested >= 0 && requested <= 255 ? static_cast<uint8_t>(requested) : current;
  if (requested >= 1 && requested <= 100) return static_cast<uint8_t>(requested);
  if (current >= 1 && current <= 100) return current;
  return command == ActionCommand::SetLevel ? 100 : 10;
}

bool InputActionManager::fits(ActionCommand command, uint8_t value, const String& targetId) const {
  if (!dimmingOnly(command) || targetId.isEmpty()) return true;  // no target yet: checked when one is chosen
  uint8_t lightId = 0;
  LightSnapshot light;
  if (!parseLightId(targetId, lightId) || !lights_.find(lightId, light)) return false;
  if (command == ActionCommand::SetPreset) {
    if (light.kind != LightKind::DmxCct && light.kind != LightKind::DmxRgb && light.kind != LightKind::DmxRgbw) return false;
    return value == 0 || presets_.fits(value, light.kind);  // no preset yet: chosen next
  }
  return light.kind == LightKind::DmxDimmable || light.kind == LightKind::DmxCct || light.kind == LightKind::DmxRgb || light.kind == LightKind::DmxRgbw;
}

void InputActionManager::forgetPreset(uint8_t presetId) {
  bool changed = false;
  for (uint8_t index = 0; index < count_; ++index) {
    if (actions_[index].command != ActionCommand::SetPreset || actions_[index].value != presetId) continue;
    actions_[index].value = 0;
    changed = true;
  }
  if (changed) save();
}

bool InputActionManager::remove(uint16_t id) {
  for (uint8_t index = 0; index < count_; ++index) {
    if (actions_[index].id != id) continue;
    const InputAction removed = actions_[index];
    for (uint8_t move = index + 1; move < count_; ++move) actions_[move - 1] = actions_[move];
    --count_;
    actions_[count_] = InputAction{};
    if (save()) return true;
    for (uint8_t move = count_; move > index; --move) actions_[move] = actions_[move - 1];
    actions_[index] = removed;
    ++count_;
    return false;
  }
  return false;
}

bool InputActionManager::test(uint16_t id) {
  for (uint8_t index = 0; index < count_; ++index) {
    if (actions_[index].id != id) continue;
    const InputAction& action = actions_[index];
    if (!execute(action)) return false;
    // Testing a preset on an off light would show nothing: the test switches it on to show the colour.
    // A real button press still leaves the light as it is.
    uint8_t lightId = 0;
    LightSnapshot light;
    if (action.command == ActionCommand::SetPreset && parseLightId(action.targetId, lightId) && lights_.find(lightId, light) && light.level == 0) {
      return lights_.turnOn(lightId);
    }
    return true;
  }
  return false;
}

void InputActionManager::handleInputEvent(uint8_t input, ButtonEvent event) {
  if (input >= kControllerInputCount || !controller_.snapshot().inputUsed[input]) return;
  const String inputId = "controller.input-" + String(input + 1);
  if (event == ButtonEvent::HoldEnd) return;  // the release itself starts nothing; LongClick follows
  ActionTrigger trigger;
  if (event == ButtonEvent::HoldStart) trigger = ActionTrigger::Hold;
  else if (event == ButtonEvent::Click) trigger = ActionTrigger::Click;
  else if (event == ButtonEvent::DoubleClick) trigger = ActionTrigger::DoubleClick;
  else if (event == ButtonEvent::TripleClick) trigger = ActionTrigger::TripleClick;
  else if (event == ButtonEvent::LongClick) trigger = ActionTrigger::LongClick;
  else return;  // Hold repeat samples are reserved for continuous actuators such as dimmers.

  char message[48];
  snprintf(message, sizeof(message), "DI%02u recognized %s", input + 1, buttonEventName(event));
  whisli::system::write(whisli::system::LogLevel::Info, "inputs", message);

  // A long click is the same press as a hold; when the button has hold actions, the hold wins.
  if (trigger == ActionTrigger::LongClick) {
    for (uint8_t index = 0; index < count_; ++index) {
      const InputAction& action = actions_[index];
      if (action.enabled && action.trigger == ActionTrigger::Hold && inputId == action.inputId && action.command != ActionCommand::None) return;
    }
  }
  for (uint8_t index = 0; index < count_; ++index) {
    const InputAction action = actions_[index];
    if (action.enabled && action.trigger == trigger && inputId == action.inputId &&
        action.command != ActionCommand::None && action.targetId[0] != '\0') {
      const bool executed = execute(action);
      whisli::system::write(executed ? whisli::system::LogLevel::Info : whisli::system::LogLevel::Error,
                            "actions", executed ? "Button action executed" : "Button action failed");
    }
  }
}

bool InputActionManager::parseTrigger(const String& value, ActionTrigger& output) {
  if (value == "click") output = ActionTrigger::Click;
  else if (value == "doubleClick") output = ActionTrigger::DoubleClick;
  else if (value == "tripleClick") output = ActionTrigger::TripleClick;
  else if (value == "longClick") output = ActionTrigger::LongClick;
  else if (value == "hold") output = ActionTrigger::Hold;
  else return false;
  return true;
}

bool InputActionManager::parseCommand(const String& value, ActionCommand& output) {
  if (value == "toggle") output = ActionCommand::Toggle;
  else if (value == "on") output = ActionCommand::TurnOn;
  else if (value == "off") output = ActionCommand::TurnOff;
  else if (value == "stepUp") output = ActionCommand::StepUp;
  else if (value == "stepDown") output = ActionCommand::StepDown;
  else if (value == "setLevel") output = ActionCommand::SetLevel;
  else if (value == "preset") output = ActionCommand::SetPreset;
  else if (value.isEmpty()) output = ActionCommand::None;
  else return false;
  return true;
}

const char* InputActionManager::triggerName(ActionTrigger trigger) {
  switch (trigger) {
    case ActionTrigger::Click: return "click";
    case ActionTrigger::DoubleClick: return "doubleClick";
    case ActionTrigger::TripleClick: return "tripleClick";
    case ActionTrigger::LongClick: return "longClick";
    case ActionTrigger::Hold: return "hold";
  }
  return "click";
}

const char* InputActionManager::commandName(ActionCommand command) {
  switch (command) {
    case ActionCommand::Toggle: return "toggle";
    case ActionCommand::TurnOn: return "on";
    case ActionCommand::TurnOff: return "off";
    case ActionCommand::StepUp: return "stepUp";
    case ActionCommand::StepDown: return "stepDown";
    case ActionCommand::RetiredHoldDim:
    case ActionCommand::RetiredHoldUp:
    case ActionCommand::RetiredHoldDown: return "";  // shown as an action still to choose
    case ActionCommand::SetLevel: return "setLevel";
    case ActionCommand::SetPreset: return "preset";
    case ActionCommand::None: return "";
  }
  return "toggle";
}

bool InputActionManager::save() {
  PersistedActionsHeader header;
  header.count = count_;
  header.nextId = nextId_;
  header.eventCount = eventCount_;
  header.nextEventId = nextEventId_;
  size_t offset = sizeof(header);
  memcpy(compactBuffer_, &header, sizeof(header));
  for (uint8_t index = 0; index < count_; ++index) {
    memcpy(compactBuffer_ + offset, &actions_[index], sizeof(InputAction));
    offset += sizeof(InputAction);
  }
  for (uint8_t index = 0; index < eventCount_; ++index) {
    memcpy(compactBuffer_ + offset, &events_[index], sizeof(InputEventBinding));
    offset += sizeof(InputEventBinding);
  }

  nvs_handle_t handle;
  esp_err_t error = nvs_open(kPreferencesNamespace, NVS_READWRITE, &handle);
  if (error == ESP_OK) {
    error = nvs_set_blob(handle, kConfigKey, compactBuffer_, offset);
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
  }
  if (error != ESP_OK) {
    char message[96];
    snprintf(message, sizeof(message), "Could not save action configuration to NVS: %s", esp_err_to_name(error));
    whisli::system::write(whisli::system::LogLevel::Error, "actions", message);
    return false;
  }
  notifyChange(kTopicActions);
  return true;
}

void InputActionManager::migrateLegacyConfiguration() {
  for (uint8_t index = 0; index < count_; ++index) {
    const InputAction& action = actions_[index];
    if (eventExists(action.inputId, action.trigger) || eventCount_ >= kMaxInputEvents) continue;
    InputEventBinding& event = events_[eventCount_++];
    event.id = nextEventId_++;
    strlcpy(event.inputId, action.inputId, sizeof(event.inputId));
    event.trigger = action.trigger;
  }
  if (save()) {
    whisli::system::write(whisli::system::LogLevel::Info, "actions", "Legacy action configuration migrated to current format");
  }
}

bool InputActionManager::eventExists(const String& inputId, ActionTrigger trigger) const {
  for (uint8_t index = 0; index < eventCount_; ++index) {
    if (inputId == events_[index].inputId && trigger == events_[index].trigger) return true;
  }
  return false;
}

bool InputActionManager::inputAvailable(const String& inputId) const {
  uint8_t channel = 0;
  if (!parseInputId(inputId, channel)) return false;
  return controller_.snapshot().inputUsed[channel - 1];
}

bool InputActionManager::targetAvailable(const String& targetId) const {
  uint8_t lightId = 0;
  LightSnapshot light;
  if (parseLightId(targetId, lightId)) return lights_.find(lightId, light) && light.kind != LightKind::None;
  uint8_t channel = 0;
  if (parseControllerOutputId(targetId, channel)) return controller_.snapshot().outputUsed[channel - 1];

  uint8_t id = 0;
  if (!parseRelayOutputId(targetId, id, channel)) return false;
  const RelaySnapshot relay = relays_.snapshot(id);
  return relay.configured && relay.active && channel <= relay.channelCount && relay.channelUsed[channel - 1];
}

bool InputActionManager::execute(const InputAction& action) {
  if (action.command == ActionCommand::None || action.targetId[0] == '\0') return false;
  uint8_t lightId = 0;
  if (parseLightId(action.targetId, lightId)) {
    LightSnapshot light;
    if (!lights_.find(lightId, light) || light.kind == LightKind::None) return false;
    switch (action.command) {
      case ActionCommand::StepUp: return lights_.step(lightId, true, action.value);
      case ActionCommand::StepDown: return lights_.step(lightId, false, action.value);
      case ActionCommand::RetiredHoldDim:
      case ActionCommand::RetiredHoldUp:
      case ActionCommand::RetiredHoldDown: return false;
      case ActionCommand::SetLevel: return lights_.setLevel(lightId, static_cast<uint8_t>((action.value * 255 + 50) / 100));
      case ActionCommand::SetPreset: return presets_.apply(action.value, lightId);
      default: break;
    }
    const bool desired = action.command == ActionCommand::TurnOn ? true :
        action.command == ActionCommand::TurnOff ? false : light.level == 0;
    return desired ? lights_.turnOn(lightId) : lights_.setLevel(lightId, 0);
  }
  if (dimmingOnly(action.command)) return false;  // outputs only switch
  uint8_t channel = 0;
  if (parseControllerOutputId(action.targetId, channel)) {
    const ControllerDevice controller = controller_.snapshot();
    if (!controller.outputUsed[channel - 1]) return false;
    const bool current = controller.outputs[channel - 1];
    const bool desired = action.command == ActionCommand::TurnOn ? true :
        action.command == ActionCommand::TurnOff ? false : !current;
    return controller_.setOutput(channel - 1, desired);
  }

  uint8_t id = 0;
  if (!parseRelayOutputId(action.targetId, id, channel)) return false;
  const RelaySnapshot relay = relays_.snapshot(id);
  if (!relay.configured || !relay.active || channel > relay.channelCount || !relay.channelUsed[channel - 1]) return false;
  const bool current = relay.channels[channel - 1];
  const bool desired = action.command == ActionCommand::TurnOn ? true :
      action.command == ActionCommand::TurnOff ? false : !current;
  return relays_.setChannel(id, channel - 1, desired);
}

}  // namespace whisli::app
