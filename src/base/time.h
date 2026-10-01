#pragma once
#include <cstdint>
#include <string_view>

namespace mico {

// "2026-09-17T18:11:23.637Z" -> unix milliseconds. 0 when it is not one.
inline int64_t parse_time(std::string_view s) {
  if (s.size() < 19 || s[4] != '-' || s[10] != 'T') return 0;
  const auto num = [&](size_t at, size_t n) {
    int v = 0;
    for (size_t i = at; i < at + n; i++) {
      if (s[i] < '0' || s[i] > '9') return -1;
      v = v * 10 + (s[i] - '0');
    }
    return v;
  };
  const int Y = num(0, 4), M = num(5, 2), D = num(8, 2), h = num(11, 2), m = num(14, 2), sec = num(17, 2);
  if (Y < 0 || M < 1 || D < 1 || h < 0 || m < 0 || sec < 0) return 0;
  // Days from the civil date (Howard Hinnant's algorithm).
  const int y = Y - (M <= 2);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = unsigned(y - era * 400);
  const unsigned doy = unsigned((153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1);
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const int64_t days = int64_t(era) * 146097 + int64_t(doe) - 719468;
  int64_t ms = ((days * 24 + h) * 60 + m) * 60000 + int64_t(sec) * 1000;
  if (s.size() > 20 && s[19] == '.') {
    int frac = 0, digits = 0;
    for (size_t i = 20; i < s.size() && s[i] >= '0' && s[i] <= '9' && digits < 3; i++, digits++)
      frac = frac * 10 + (s[i] - '0');
    while (digits++ < 3) frac *= 10;
    ms += frac;
  }
  return ms;
}

}  // namespace mico
