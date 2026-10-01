#pragma once
#include <algorithm>
#include <string_view>

#include "base/json.h"
#include "base/jsonl.h"
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
void scan_forward(Jsonl& j, UsageResume& r, F&& fn) {
  while (!j.complete() && j.indexed_from() > r.offset) j.extend_back();
  const size_t n = j.line_count();
  size_t i = r.offset ? j.line_at_byte(r.offset) : 0;
  while (i < n && j.line_offset(i) < r.offset) i++;
  constexpr size_t kChunk = 4096;
  for (; i < n; i++) {
    if (i % kChunk == 0 || i == 0) j.will_read(i, std::min(n, i + kChunk));
    const std::string_view raw = j.line(i);
    if (j.line_offset(i) + raw.size() >= j.line_offset(i + 1)) break;  // no newline yet
    r.offset = j.line_offset(i + 1);
    fn(raw);
  }
}

}  // namespace mico::usage
