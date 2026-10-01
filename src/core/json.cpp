#include "core/json.h"

#include <cstdio>

#include "term/text.h"

namespace mico::js {

std::string quote(std::string_view s) {
  std::string out = "\"";
  for (const char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (uint8_t(c) < 0x20) {
          char b[8];
          snprintf(b, sizeof b, "\\u%04x", unsigned(uint8_t(c)));
          out += b;
        } else {
          out += c;
        }
    }
  }
  return out + "\"";
}

void unescape_append(std::string_view body, std::string& out) {
  out.reserve(out.size() + body.size());
  size_t i = 0;
  while (i < body.size()) {
    // Escapes are rare; copy the run up to the next backslash in one go.
    const char* b =
        static_cast<const char*>(memchr(body.data() + i, '\\', body.size() - i));
    if (!b) { out.append(body.substr(i)); return; }
    size_t bi = size_t(b - body.data());
    out.append(body.substr(i, bi - i));
    i = bi + 1;
    if (i >= body.size()) return;

    switch (body[i]) {
      case 'n': out.push_back('\n'); break;
      case 't': out.push_back('\t'); break;
      case 'r': out.push_back('\r'); break;
      case 'b': out.push_back('\b'); break;
      case 'f': out.push_back('\f'); break;
      case 'u': {
        if (i + 4 >= body.size()) return;
        auto hex = [&](size_t k) -> int {
          char h = body[k];
          if (h >= '0' && h <= '9') return h - '0';
          if (h >= 'a' && h <= 'f') return h - 'a' + 10;
          if (h >= 'A' && h <= 'F') return h - 'A' + 10;
          return -1;
        };
        int cp = 0;
        for (size_t k = 1; k <= 4; k++) {
          int d = hex(i + k);
          if (d < 0) return;
          cp = cp * 16 + d;
        }
        i += 4;
        // Surrogate pair: agents emit these for anything above the BMP.
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < body.size() && body[i + 1] == '\\' &&
            body[i + 2] == 'u') {
          int lo = 0;
          bool ok = true;
          for (size_t k = 3; k <= 6; k++) {
            int d = hex(i + k);
            if (d < 0) { ok = false; break; }
            lo = lo * 16 + d;
          }
          if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            i += 6;
          }
        }
        text::encode(char32_t(cp), out);
        break;
      }
      default: out.push_back(body[i]); break;
    }
    i++;
  }
}

}  // namespace mico::js
