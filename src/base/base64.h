#pragma once
#include <cstddef>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

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

// Standard or URL-safe base64, whitespace ignored, into `out`. False on
// anything else, or on nothing.
inline bool base64_decode(std::string_view in, std::string& out) {
  // 0..63 a digit; 64 skipped (whitespace, the backslash of a JSON "\/");
  // 65 the end ('='); 66 anything else. URL-safe digits read too.
  static const auto table = [] {
    std::array<uint8_t, 256> t;
    t.fill(66);
    for (int i = 0; i < 26; i++) t[size_t('A' + i)] = uint8_t(i), t[size_t('a' + i)] = uint8_t(26 + i);
    for (int i = 0; i < 10; i++) t[size_t('0' + i)] = uint8_t(52 + i);
    t['+'] = t['-'] = 62;
    t['/'] = t['_'] = 63;
    t[' '] = t['\n'] = t['\r'] = t['\t'] = t['\\'] = 64;
    t['='] = 65;
    return t;
  }();
  out.clear();
  out.resize(in.size() / 4 * 3 + 3);
  char* o = out.data();
  const auto* p = reinterpret_cast<const uint8_t*>(in.data());
  const auto* end = p + in.size();
  // Four plain digits at a time, the whole of an unwrapped payload.
  while (end - p >= 4) {
    const uint32_t a = table[p[0]], b = table[p[1]], c = table[p[2]], d = table[p[3]];
    if ((a | b | c | d) >= 64) break;
    const uint32_t v = a << 18 | b << 12 | c << 6 | d;
    o[0] = char(v >> 16), o[1] = char(v >> 8), o[2] = char(v);
    o += 3, p += 4;
  }
  uint32_t acc = 0;
  int bits = 0;
  for (; p < end; p++) {
    const uint8_t v = table[*p];
    if (v == 64) continue;
    if (v == 65) break;
    if (v == 66) {
      out.clear();
      return false;
    }
    acc = (acc << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      *o++ = char((acc >> bits) & 0xFF);
    }
  }
  out.resize(size_t(o - out.data()));
  return !out.empty();
}

}  // namespace mico
