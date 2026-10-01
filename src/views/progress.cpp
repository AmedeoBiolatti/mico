#include "views/progress.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "term/text.h"

namespace mico::progress {
namespace {

bool digit(char c) { return c >= '0' && c <= '9'; }

// A number at i (digits, one optional decimal part). Advances i.
bool number(std::string_view s, size_t& i, double& v) {
  const size_t a = i;
  while (i < s.size() && digit(s[i])) i++;
  if (i < s.size() && s[i] == '.' && i + 1 < s.size() && digit(s[i + 1])) {
    i++;
    while (i < s.size() && digit(s[i])) i++;
  }
  if (i == a) return false;
  v = std::strtod(std::string(s.substr(a, i - a)).c_str(), nullptr);
  return true;
}

// [h:]mm:ss at i, as seconds.
bool clock(std::string_view s, size_t i, int& secs) {
  int parts[3], n = 0;
  while (n < 3) {
    const size_t a = i;
    while (i < s.size() && digit(s[i])) i++;
    if (i == a || i - a > 2) return false;
    parts[n++] = std::atoi(std::string(s.substr(a, i - a)).c_str());
    if (i < s.size() && s[i] == ':') i++;
    else break;
  }
  if (n < 2) return false;
  secs = n == 3 ? parts[0] * 3600 + parts[1] * 60 + parts[2] : parts[0] * 60 + parts[1];
  return true;
}

// A run of bar characters: block elements, '#', '=', '━', '■', '>'.
int bar_run(std::string_view s) {
  int best = 0, run = 0;
  for (size_t i = 0; i < s.size();) {
    const char32_t c = text::decode(s, i);
    const bool b = c == U'#' || c == U'=' || c == U'>' || c == U'━' || c == U'■' || c == U'╸' ||
                   c == U'╺' || (c >= 0x2588 && c <= 0x258F);
    run = b ? run + 1 : 0;
    best = std::max(best, run);
  }
  return best;
}

}  // namespace

bool parse(std::string_view s, Progress& out) {
  out = Progress{};
  // A status line about the agent itself, not the command.
  for (std::string_view no : {"context", "Context", "auto-compact", "tokens", "esc to interrupt", "battery"})
    if (s.find(no) != std::string_view::npos) return false;
  // Past whatever the agent draws in front of output: ⎿, │, spaces.
  size_t start = 0;
  while (start < s.size()) {
    size_t j = start;
    const char32_t c = text::decode(s, j);
    if (c == U' ' || c == U'⎿' || c == U'│' || c == U'└' || c == U'\t') start = j;
    else break;
  }
  s = s.substr(start);
  if (s.empty()) return false;

  double pct = -1, done = -1, total = -1;
  bool counter = false;  // [n/m] or [ NN%] leading the line
  if (s[0] == '[') {
    size_t i = 1;
    while (i < s.size() && s[i] == ' ') i++;
    double a = 0, b = 0;
    if (number(s, i, a)) {
      if (i < s.size() && s[i] == '/') {
        size_t k = i + 1;
        if (number(s, k, b) && k < s.size() && s[k] == ']' && b > 0 && a <= b) {
          done = a, total = b, counter = true;
        }
      } else if (i < s.size() && s[i] == '%' && i + 1 < s.size() && s[i + 1] == ']' && a <= 100) {
        pct = a, counter = true;
      }
    }
  }
  // Elsewhere on the line: the last NN% and the first n/m.
  for (size_t i = 0; i < s.size(); i++) {
    if (!digit(s[i]) || (i && (digit(s[i - 1]) || s[i - 1] == '.'))) continue;
    size_t k = i;
    double a = 0;
    if (!number(s, k, a)) continue;
    if (k < s.size() && s[k] == '%') {
      if (a <= 100) pct = a;
    } else if (k < s.size() && s[k] == '/' && done < 0) {
      size_t m = k + 1;
      double b = 0;
      // Not a date (2024/05) nor a path: n/m with n <= m, and m a count.
      if (number(s, m, b) && b > 0 && a <= b && (m == s.size() || !std::isalnum(uint8_t(s[m])) || s[m] == 'M' ||
                                                 s[m] == 'k' || s[m] == 'G'))
        done = a, total = b;
    }
    i = k;
  }
  const bool tqdm = s.find("it/s") != std::string_view::npos || s.find("s/it") != std::string_view::npos ||
                    (s.find('<') != std::string_view::npos && s.find('[') != std::string_view::npos && pct >= 0);
  const bool bar = bar_run(s) >= 3;
  if (!counter && !tqdm && !(bar && (pct >= 0 || done >= 0))) return false;
  if (pct < 0 && done < 0) return false;
  // A whole-number count is shown as one ("123/456"); a size is not.
  if (done >= 0 && done == std::floor(done) && total == std::floor(total)) {
    out.done = int64_t(done);
    out.total = int64_t(total);
  }
  out.fraction = pct >= 0 ? pct / 100 : done / total;
  out.fraction = std::clamp(out.fraction, 0.0, 1.0);
  // The time left: tqdm's [elapsed<left], pip's "eta 0:00:02", "ETA 12s".
  if (const size_t lt = s.find('<'); lt != std::string_view::npos) clock(s, lt + 1, out.eta_s);
  for (std::string_view tag : {"eta ", "ETA ", "eta: ", "ETA: "}) {
    const size_t at = s.find(tag);
    if (at == std::string_view::npos || out.eta_s >= 0) continue;
    size_t k = at + tag.size();
    if (clock(s, k, out.eta_s)) break;
    double v = 0;
    if (number(s, k, v)) {
      const char u = k < s.size() ? s[k] : 's';
      out.eta_s = int(v * (u == 'm' ? 60 : u == 'h' ? 3600 : 1));
    }
  }
  return true;
}

std::string duration(int s) {
  char buf[32];
  if (s >= 3600) snprintf(buf, sizeof buf, "%dh %02dm", s / 3600, s / 60 % 60);
  else if (s >= 60) snprintf(buf, sizeof buf, "%dm %02ds", s / 60, s % 60);
  else snprintf(buf, sizeof buf, "%ds", s);
  return buf;
}

std::string bar(double f, int cols) {
  static const char* const kEighths[] = {"", "\xE2\x96\x8F", "\xE2\x96\x8E", "\xE2\x96\x8D", "\xE2\x96\x8C",
                                         "\xE2\x96\x8B", "\xE2\x96\x8A", "\xE2\x96\x89"};
  const int eighths = int(std::lround(std::clamp(f, 0.0, 1.0) * cols * 8));
  std::string out;
  int used = 0;
  for (int i = 0; i < eighths / 8; i++, used++) out += "\xE2\x96\x88";  // █
  if (eighths % 8 && used < cols) {
    out += kEighths[eighths % 8];
    used++;
  }
  out.append(size_t(cols - used), ' ');
  return out;
}

}  // namespace mico::progress
