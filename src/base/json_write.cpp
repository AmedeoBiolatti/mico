#include "base/json_write.h"

#include <cstdio>

#include "base/text.h"

namespace mico::jw {

void string(std::string& out, std::string_view s) {
  out += '"';
  size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = uint8_t(s[i]);
    if (c < 0x80) {
      i++;
      switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
          if (c < 0x20 || c == 0x7f) {
            char buf[8];
            snprintf(buf, sizeof buf, "\\u%04x", c);
            out += buf;
          } else {
            out += char(c);
          }
      }
      continue;
    }
    // Re-encoding what decodes keeps valid text as it was and turns anything
    // else into U+FFFD. The decoder lets surrogates and overlong forms
    // through, which would come out invalid or as a raw control byte.
    char32_t cp = text::decode(s, i);
    if (cp < 0x80 || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) cp = 0xFFFD;
    text::encode(cp, out);
  }
  out += '"';
}

Writer& Writer::id(uint64_t v) {
  char buf[24];
  snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
  return str(buf);
}

}  // namespace mico::jw
