#include "whisli/app/button_input.hpp"

namespace whisli::app {

ButtonInput::ButtonInput(InputTiming timing) : timing_(timing) {}

void ButtonInput::begin(bool pressed, uint32_t nowMs) {
  initialized_ = true;
  rawCandidatePressed_ = pressed;
  stablePressed_ = pressed;
  ignoreUntilReleased_ = pressed;
  rawChangedAtMs_ = nowMs;
}

void ButtonInput::update(bool pressed, uint32_t nowMs) {
  if (!initialized_) {
    begin(pressed, nowMs);
    return;
  }

  if (pressed != rawCandidatePressed_) {
    rawCandidatePressed_ = pressed;
    rawChangedAtMs_ = nowMs;
  } else if (pressed != stablePressed_ && nowMs - rawChangedAtMs_ >= timing_.debounceMs) {
    stablePressed_ = pressed;
    processStableChange(pressed, nowMs);
  }

  if (stablePressed_ && !ignoreUntilReleased_ && !longClickEmitted_ &&
      nowMs - pressedAtMs_ >= timing_.longClickMs) {
    emitPendingClick(nowMs);
    longClickEmitted_ = true;
    lastHoldAtMs_ = nowMs;
    emit(ButtonEvent::HoldStart, nowMs);
  } else if (stablePressed_ && !ignoreUntilReleased_ && longClickEmitted_ &&
             nowMs - lastHoldAtMs_ >= timing_.holdRepeatMs) {
    lastHoldAtMs_ = nowMs;
    emit(ButtonEvent::Hold, nowMs);
  }

  if (!stablePressed_ && pendingClickCount_ > 0 &&
      nowMs - lastClickAtMs_ >= timing_.doubleClickWindowMs) {
    emitPendingClick(nowMs);
  }
}

bool ButtonInput::isPressed() const { return stablePressed_; }

ButtonEvent ButtonInput::lastEvent() const { return lastEvent_; }

uint8_t ButtonInput::takeEvents(ButtonEvent* output, uint8_t capacity) {
  const uint8_t count = pendingEventCount_ < capacity ? pendingEventCount_ : capacity;
  for (uint8_t index = 0; index < count; ++index) output[index] = pendingEvents_[index];
  for (uint8_t index = count; index < pendingEventCount_; ++index) pendingEvents_[index - count] = pendingEvents_[index];
  pendingEventCount_ -= count;
  return count;
}

void ButtonInput::processStableChange(bool pressed, uint32_t nowMs) {
  if (pressed) {
    if (ignoreUntilReleased_) return;
    pressedAtMs_ = nowMs;
    longClickEmitted_ = false;
    return;
  }

  if (ignoreUntilReleased_) {
    ignoreUntilReleased_ = false;
    return;
  }
  // A long press ends on release: the hold stops, then it counts as a long click.
  if (longClickEmitted_) {
    longClickEmitted_ = false;
    emit(ButtonEvent::HoldEnd, nowMs);
    emit(ButtonEvent::LongClick, nowMs);
    return;
  }
  registerClick(nowMs);
}

void ButtonInput::registerClick(uint32_t nowMs) {
  if (pendingClickCount_ > 0 && nowMs - lastClickAtMs_ > timing_.doubleClickWindowMs) {
    emitPendingClick(nowMs);
  }

  if (pendingClickCount_ == 0) {
    pendingClickCount_ = 1;
  } else if (pendingClickCount_ < 3) {
    ++pendingClickCount_;
  }
  lastClickAtMs_ = nowMs;

  if (pendingClickCount_ == 3) {
    emit(ButtonEvent::TripleClick, nowMs);
    pendingClickCount_ = 0;
  }
}

void ButtonInput::emit(ButtonEvent event, uint32_t) {
  if (event != ButtonEvent::HoldEnd) lastEvent_ = event == ButtonEvent::HoldStart ? ButtonEvent::Hold : event;
  if (pendingEventCount_ < sizeof(pendingEvents_) / sizeof(pendingEvents_[0])) {
    pendingEvents_[pendingEventCount_++] = event;
  }
}

void ButtonInput::emitPendingClick(uint32_t nowMs) {
  if (pendingClickCount_ == 1) emit(ButtonEvent::Click, nowMs);
  else if (pendingClickCount_ == 2) emit(ButtonEvent::DoubleClick, nowMs);
  pendingClickCount_ = 0;
}

const char* buttonEventName(ButtonEvent event) {
  switch (event) {
    case ButtonEvent::Click: return "click";
    case ButtonEvent::DoubleClick: return "doubleClick";
    case ButtonEvent::TripleClick: return "tripleClick";
    case ButtonEvent::LongClick: return "longClick";
    case ButtonEvent::HoldStart: return "hold";
    case ButtonEvent::Hold: return "hold";
    case ButtonEvent::HoldEnd: return "holdEnd";
    case ButtonEvent::None: return "none";
  }
  return "none";
}

}  // namespace whisli::app
