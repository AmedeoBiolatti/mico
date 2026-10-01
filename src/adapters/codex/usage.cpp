#include "adapters/codex/codex.h"
#include "base/text.h"

#include <cstdlib>
#include <string>

#include "adapters/usage_scan.h"

namespace mico {

using namespace usage;

namespace {

// codex's token counts. Each token_count record carries the step's own usage
// and a running total, but the running total starts again from zero whenever
// the session is resumed, so the newest one can be a small fraction of the
// session. The steps are summed instead, each credited to the model of the
// turn it belongs to. codex repeats a record unchanged at times; a repeat has
// the same running total and is skipped.
UsageStat codex_usage(const js::Value& v) {
  uint64_t in = 0, cached = 0, cached_write = 0, out = 0, reasoning = 0, sum = 0;
  js::scan_object(v.raw, [&](std::string_view k, const js::Value& x) {
    if (k == "input_tokens") in = u64(x);
    else if (k == "cached_input_tokens") cached = u64(x);
    else if (k == "cache_write_input_tokens") cached_write = u64(x);
    else if (k == "output_tokens") out = u64(x);
    else if (k == "reasoning_output_tokens") reasoning = u64(x);
    else if (k == "total_tokens") sum = u64(x);
    return true;
  });
  UsageStat s;
  // codex's input_tokens already includes the cached read; split them so the
  // cache percentage means the same thing it does for the other agents.
  s.input = in > cached ? in - cached : 0;
  s.cache_read = cached;
  s.cache_write = cached_write;
  s.output = out;
  s.reasoning = reasoning;
  s.total = sum ? sum : in + out;
  return s;
}

void scan_codex_rate_limit(const js::Value& limits, UsageEntry& e) {
  js::Value primary{};
  js::scan_object(limits.raw, [&](std::string_view k, const js::Value& v) {
    if (k == "primary") { primary = v; return false; }
    return true;
  });
  if (!primary.is_object()) return;
  js::scan_object(primary.raw, [&](std::string_view k, const js::Value& v) {
    if (k == "used_percent") e.quota_used_pct = f64(v);
    else if (k == "resets_at") e.quota_resets_at = int64_t(u64(v));
    else if (k == "window_minutes") e.quota_window_minutes = int64_t(u64(v));
    return true;
  });
  e.has_quota = true;
}

void scan_codex_line(std::string_view raw, UsageEntry& e, UsageResume& r) {
  const bool turn = text::contains(raw, "\"turn_context\"");
  if (!turn && !text::contains(raw, "\"token_count\"")) return;
  js::Value payload{};
  js::scan_object(raw, [&](std::string_view k, const js::Value& v) {
    if (k == "payload") { payload = v; return false; }
    return true;
  });
  if (!payload.is_object()) return;

  std::string_view ptype, model;
  js::Value info{}, limits{};
  js::scan_object(payload.raw, [&](std::string_view k, const js::Value& v) {
    if (k == "type") ptype = v.body();
    else if (k == "model") model = v.body();
    else if (k == "info") info = v;
    else if (k == "rate_limits") limits = v;
    return true;
  });
  if (turn && !model.empty()) {
    r.model = std::string(model);
    return;
  }
  if (ptype != "token_count") return;
  if (limits.is_object()) scan_codex_rate_limit(limits, e);
  if (!info.is_object()) return;

  js::Value total{}, last{};
  js::scan_object(info.raw, [&](std::string_view k, const js::Value& v) {
    if (k == "total_token_usage") total = v;
    else if (k == "last_token_usage") last = v;
    return true;
  });
  if (!total.is_object()) return;
  const UsageStat t = codex_usage(total);
  const bool repeat = r.has_total && t.total == r.total.total && t.output == r.total.output &&
                      t.cache_read == r.total.cache_read;
  const bool restarted = r.has_total && t.total < r.total.total;
  UsageStat step;
  if (repeat) return;
  if (last.is_object()) step = codex_usage(last);
  // No step recorded: what the running total gained is the step.
  else if (!r.has_total || restarted) step = t;
  else {
    step.input = t.input - std::min(t.input, r.total.input);
    step.cache_read = t.cache_read - std::min(t.cache_read, r.total.cache_read);
    step.cache_write = t.cache_write - std::min(t.cache_write, r.total.cache_write);
    step.output = t.output - std::min(t.output, r.total.output);
    step.reasoning = t.reasoning - std::min(t.reasoning, r.total.reasoning);
    step.total = t.total - r.total.total;
  }
  r.total = t;
  r.has_total = true;
  e.stat.add(step);
  add_model(e, r.model, step);
}

}  // namespace

void CodexAdapter::read_usage(LineReader& j, UsageEntry& e, UsageResume& r) const {
  scan_forward(j, r, [&](std::string_view raw) { scan_codex_line(raw, e, r); });
}

}  // namespace mico
