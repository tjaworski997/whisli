#pragma once

#include <Arduino.h>

namespace whisli::net {

// Escapes a value for use inside a JSON string literal.
String jsonEscape(const char* value);
inline String jsonEscape(const String& value) { return jsonEscape(value.c_str()); }

}  // namespace whisli::net
