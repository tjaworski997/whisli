#include "whisli/app/input_ref.hpp"

#include <cstdio>

#include "whisli/app/controller_io.hpp"
#include "whisli/app/modbus_relays.hpp"

namespace whisli::app {
namespace {

// Digits only, 1..max: "07" or "3x" are not ids.
bool parseNumber(const String& text, long max, long& output) {
  if (text.isEmpty() || text.length() > 3) return false;
  for (const char character : text) if (!isDigit(character)) return false;
  output = text.toInt();
  return output >= 1 && output <= max && String(output) == text;
}

}  // namespace

String inputIdOf(InputRef input) {
  if (input.device == 0) return "controller.input-" + String(input.channel + 1);
  return "relay-" + String(input.device) + ".input-" + String(input.channel + 1);
}

bool parseInputId(const String& value, InputRef& output) {
  constexpr char kControllerPrefix[] = "controller.input-";
  long device = 0, channel = 0;
  if (value.startsWith(kControllerPrefix)) {
    if (!parseNumber(value.substring(sizeof(kControllerPrefix) - 1), kControllerInputCount, channel)) return false;
  } else if (value.startsWith("relay-")) {
    const int separator = value.indexOf(".input-");
    if (separator < 7 || !parseNumber(value.substring(6, separator), 255, device) ||
        !parseNumber(value.substring(separator + 7), kMaxRelayChannels, channel)) return false;
  } else {
    return false;
  }
  output.device = static_cast<uint8_t>(device);
  output.channel = static_cast<uint8_t>(channel - 1);
  return true;
}

String inputHardwareName(InputRef input) {
  char text[8];
  if (input.device == 0) snprintf(text, sizeof(text), "DI%02u", input.channel + 1);
  else snprintf(text, sizeof(text), "X%02u", input.channel);
  return text;
}

}  // namespace whisli::app
