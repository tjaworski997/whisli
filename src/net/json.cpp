#include "whisli/net/json.hpp"

namespace whisli::net {

String jsonEscape(const char* value) {
  String escaped;
  if (!value) return escaped;
  for (const unsigned char* character = reinterpret_cast<const unsigned char*>(value); *character; ++character) {
    switch (*character) {
      case '"': escaped += "\\\""; break;
      case '\\': escaped += "\\\\"; break;
      case '\b': escaped += "\\b"; break;
      case '\f': escaped += "\\f"; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case '\t': escaped += "\\t"; break;
      default:
        if (*character < 0x20) {
          constexpr char hex[] = "0123456789abcdef";
          escaped += "\\u00";
          escaped += hex[*character >> 4];
          escaped += hex[*character & 0x0f];
        } else {
          escaped += static_cast<char>(*character);
        }
    }
  }
  return escaped;
}

}  // namespace whisli::net
