#pragma once

#include <Arduino.h>

namespace whisli::app {

// One button input anywhere in the installation: an input of the controller board (`device` 0) or of
// a Modbus input module (`device` = its id). `channel` counts from 0. In the API and in stored
// actions it is a text id: "controller.input-3" or "relay-2.input-5" (both count from 1).
struct InputRef {
  uint8_t device = 0;
  uint8_t channel = 0;
};

String inputIdOf(InputRef input);
bool parseInputId(const String& value, InputRef& output);
// The label printed on the hardware: "DI03" on the board, "X04" on a module (counted from 0, like
// the N4DIH32's terminals).
String inputHardwareName(InputRef input);

}  // namespace whisli::app
