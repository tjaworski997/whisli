#pragma once

#include <stdint.h>

#include "whisli/app/input_timing.hpp"

namespace whisli::app {

enum class ButtonEvent : uint8_t {
  None,
  Click,
  DoubleClick,
  TripleClick,
  LongClick,
  HoldStart,  // held for longClickMs: "hold" actions run, a dimming ramp starts
  Hold,       // repeats while still held
  HoldEnd,    // released after a hold: a dimming ramp stops (followed by LongClick)
};

// Interprets a debounced logical button state. One instance belongs to one
// input, regardless of which hardware module supplies that input.
class ButtonInput {
 public:
  explicit ButtonInput(InputTiming timing = kDefaultInputTiming);

  void begin(bool pressed, uint32_t nowMs);
  void update(bool pressed, uint32_t nowMs);

  bool isPressed() const;
  ButtonEvent lastEvent() const;
  uint8_t takeEvents(ButtonEvent* output, uint8_t capacity);

 private:
  void processStableChange(bool pressed, uint32_t nowMs);
  void registerClick(uint32_t nowMs);
  void emit(ButtonEvent event, uint32_t nowMs);
  void emitPendingClick(uint32_t nowMs);

  InputTiming timing_;
  bool initialized_ = false;
  bool rawCandidatePressed_ = false;
  bool stablePressed_ = false;
  bool ignoreUntilReleased_ = false;
  bool longClickEmitted_ = false;
  uint8_t pendingClickCount_ = 0;
  uint32_t rawChangedAtMs_ = 0;
  uint32_t pressedAtMs_ = 0;
  uint32_t lastClickAtMs_ = 0;
  uint32_t lastHoldAtMs_ = 0;
  ButtonEvent lastEvent_ = ButtonEvent::None;
  ButtonEvent pendingEvents_[4] = {};
  uint8_t pendingEventCount_ = 0;
};

const char* buttonEventName(ButtonEvent event);

}  // namespace whisli::app
