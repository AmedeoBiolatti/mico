#include "adapters/pi/pi.h"
#include "base/text.h"

#include <cstdlib>
#include <string>

#include "adapters/usage_scan.h"
#include "base/line_reader.h"

namespace mico {

using namespace usage;

// pi and omp: one usage object per assistant message, each already carrying a
// per-message cost. Summing is the only way to a session total; the records are
// interleaved with tool results and telemetry, so the usage substring is
// rejected before anything is parsed.
void PiFamilyAdapter::read_usage(LineReader& j, UsageEntry& e, UsageResume&) const {
  j.each(0, [&](std::string_view raw, uint64_t) {
    if (!text::contains(raw, "\"usage\"")) return;

    std::string_view type;
    js::Value message{};
    js::scan_object(raw, [&](std::string_view k, const js::Value& v) {
      if (k == "type") { type = v.body(); return type == "message"; }
      if (k == "message") { message = v; return false; }
      return true;
    });
    if (type != "message" || !message.is_object()) return;

    std::string_view role, model;
    js::Value usage{};
    js::scan_object(message.raw, [&](std::string_view k, const js::Value& v) {
      if (k == "role") role = v.body();
      else if (k == "model") model = v.body();
      else if (k == "usage") usage = v;
      return true;
    });
    if (role != "assistant" || !usage.is_object()) return;

    UsageStat s;
    js::Value cost{};
    js::scan_object(usage.raw, [&](std::string_view k, const js::Value& v) {
      if (k == "input") s.input = u64(v);
      else if (k == "output") s.output = u64(v);
      else if (k == "cacheRead") s.cache_read = u64(v);
      else if (k == "cacheWrite") s.cache_write = u64(v);
      // pi writes "reasoning"; omp writes "reasoningTokens".
      else if (k == "reasoning" || k == "reasoningTokens") s.reasoning = u64(v);
      else if (k == "totalTokens") s.total = u64(v);
      else if (k == "cost") cost = v;
      return true;
    });
    if (s.total == 0) s.total = s.input + s.output + s.cache_read + s.cache_write;
    if (cost.is_object()) {
      js::scan_object(cost.raw, [&](std::string_view k, const js::Value& v) {
        if (k == "total") { s.cost_usd = f64(v); s.has_cost = true; }
        return true;
      });
    }
    e.stat.add(s);
    add_model(e, model, s);
  });
}

}  // namespace mico
