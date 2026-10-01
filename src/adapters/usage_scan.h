#pragma once
#include <algorithm>
#include <string_view>

#include "base/json.h"
#include "base/line_reader.h"
#include "model/usage.h"

// Reading token usage out of transcripts: what several agents' readers share.
namespace mico::usage {

// A JSON number as an unsigned count; 0 for anything else.
uint64_t u64(const js::Value& v);
// A JSON number as a double; 0 for anything else.
double f64(const js::Value& v);
// Adds `s` to `e`'s total and to its line for `name`.
void add_model(UsageEntry& e, std::string_view name, const UsageStat& s);

// Reads the complete lines from r.offset on, handing each to `fn`. A last line
// still being written has no newline yet and is left for the next pass.
template <class F>
void scan_forward(LineReader& j, UsageResume& r, F&& fn) {
  r.offset = j.each(r.offset, [&](std::string_view raw, uint64_t) { fn(raw); });
}

}  // namespace mico::usage
