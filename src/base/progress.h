#pragma once
#include <cstdint>
#include <string>
#include <string_view>

// Progress a running command prints — tqdm, ninja and cmake counters, cargo,
// pip and the like — read from a line of its output, so the activity row can
// draw it as a bar with an estimate of the time left.
namespace mico::progress {

struct Progress {
  double fraction = 0;          // 0..1
  int64_t done = -1, total = -1;  // a counter, when the line has one
  int eta_s = -1;                 // the time left, when the line says
};

// True when `line` reads as a progress report. Wants more than a bare
// percentage (a status line's "12% context left" is not progress): a bar, a
// [n/m] or [ NN%] counter, or tqdm's own layout.
bool parse(std::string_view line, Progress& out);

// "15s", "2m 05s", "1h 02m".
std::string duration(int seconds);

// `fraction` as a bar `cols` cells wide, in eighth blocks.
std::string bar(double fraction, int cols);

}  // namespace mico::progress
