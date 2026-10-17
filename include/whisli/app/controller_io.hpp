#pragma once

#include <Arduino.h>
#include "whisli/app/button_input.hpp"

namespace whisli::app {

constexpr uint8_t kControllerOutputCount = 8;
constexpr uint8_t kControllerInputCount = 8;

// Dane stałego urządzenia Device z wyjściami wbudowanymi w płytkę Whisli.
struct ControllerDevice {
  bool online = false;
  char name[33] = "Whisli Controller";
  bool outputs[kControllerOutputCount] = {};
  bool outputUsed[kControllerOutputCount] = {};
  char outputNames[kControllerOutputCount][33] = {};
  bool inputsHigh[kControllerInputCount] = {};
  bool inputUsed[kControllerInputCount] = {};
  char inputNames[kControllerInputCount][33] = {};
  ButtonEvent inputLastEvents[kControllerInputCount] = {};
};

class ControllerIo {
 public:
  using InputEventHandler = void (*)(void* context, uint8_t input, ButtonEvent event);

  bool begin();
  ControllerDevice snapshot();
  bool setName(const String& name);
  bool setOutput(uint8_t channel, bool on);
  bool setAllOutputs(bool on);
  bool setOutputName(uint8_t channel, const String& name);
  bool setOutputUsed(uint8_t channel, bool used);
  bool setInputName(uint8_t channel, const String& name);
  bool setInputUsed(uint8_t channel, bool used);
  void setInputEventHandler(InputEventHandler handler, void* context);
  void poll();

 private:
  bool writeOutputsLocked(uint8_t state);
  bool save();

  SemaphoreHandle_t lock_ = nullptr;
  ControllerDevice device_;
  uint8_t outputState_ = 0;
  ButtonInput buttonInputs_[kControllerInputCount];
  uint32_t lastInputPollAt_ = 0;
  InputEventHandler inputEventHandler_ = nullptr;
  void* inputEventContext_ = nullptr;
};

}  // namespace whisli::app
