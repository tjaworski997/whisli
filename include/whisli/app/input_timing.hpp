#pragma once

#include <stdint.h>

namespace whisli::app {

// Domyślne czasy dla interpretacji fizycznych przycisków.
// Każde wejście będzie mogło dostać własne nadpisanie tych wartości.
struct InputTiming {
  uint16_t debounceMs = 30;
  uint16_t doubleClickWindowMs = 300;
  uint16_t longClickMs = 600;
  uint16_t holdRepeatMs = 100;
};

constexpr InputTiming kDefaultInputTiming{};

}  // namespace whisli::app
