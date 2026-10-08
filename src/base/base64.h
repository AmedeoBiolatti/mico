#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace mico {

// Appends `n` bytes as standard base64, padded.
inline void base64_append(const uint8_t* p, size_t n, std::string& out) {
  static constexpr char kDigits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  out.reserve(out.size() + (n + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < n; i += 3) {
    const uint32_t v = uint32_t(p[i]) << 16 | uint32_t(p[i + 1]) << 8 | p[i + 2];
    out += kDigits[v >> 18];
    out += kDigits[(v >> 12) & 63];
    out += kDigits[(v >> 6) & 63];
    out += kDigits[v & 63];
  }
  if (i < n) {
    uint32_t v = uint32_t(p[i]) << 16;
    if (i + 1 < n) v |= uint32_t(p[i + 1]) << 8;
    out += kDigits[v >> 18];
    out += kDigits[(v >> 12) & 63];
    out += i + 1 < n ? kDigits[(v >> 6) & 63] : '=';
    out += '=';
  }
}

}  // namespace mico
