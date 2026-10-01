#include "adapters/claude/claude.h"
#include "base/text.h"

#include <cstdlib>
#include <string>

#include "adapters/usage_scan.h"

namespace mico {

using namespace usage;

namespace {

// claude's cost-state record: the dollars its writing process has spent so far,
// per model, with the tokens they bought. Kept as price samples, not totals.
void scan_claude_cost_state(std::string_view raw, UsageEntry& e) {
  js::Value models{};
  bool is_cost_state = false;
  js::scan_object(raw, [&](std::string_view k, const js::Value& v) {
    if (k == "type") { is_cost_state = v.body() == "cost-state"; return is_cost_state; }
    if (k == "modelUsage") { models = v; return false; }
    return true;
  });
  if (!is_cost_state || !models.is_object()) return;
  js::scan_object(models.raw, [&](std::string_view model, const js::Value& o) {
    if (!o.is_object()) return true;
    PriceSample s;
    s.model = std::string(model);
    js::scan_object(o.raw, [&](std::string_view k, const js::Value& v) {
      if (k == "inputTokens") s.input = u64(v);
      else if (k == "outputTokens") s.output = u64(v);
      else if (k == "cacheReadInputTokens") s.cache_read = u64(v);
      else if (k == "cacheCreationInputTokens") s.cache_write = u64(v);
      else if (k == "costUSD") s.cost = f64(v);
      return true;
    });
    // A process that ran on writes the same figures again and again.
    for (const auto& o2 : e.price_samples)
      if (o2.model == s.model && o2.input == s.input && o2.output == s.output &&
          o2.cache_read == s.cache_read && o2.cache_write == s.cache_write)
        return true;
    e.price_samples.push_back(std::move(s));
    return true;
  });
}

// One API response's usage. claude writes a response as one line per content
// block, each repeating the same usage, so it is counted once per message id.
void scan_claude_message(std::string_view raw, UsageEntry& e, UsageResume& r) {
  js::Value message{};
  js::scan_object(raw, [&](std::string_view k, const js::Value& v) {
    if (k == "message") { message = v; return false; }
    return true;
  });
  if (!message.is_object()) return;

  std::string_view role, model, id;
  js::Value usage{};
  js::scan_object(message.raw, [&](std::string_view k, const js::Value& v) {
    if (k == "role") role = v.body();
    else if (k == "model") model = v.body();
    else if (k == "id") id = v.body();
    else if (k == "usage") usage = v;
    return true;
  });
  // "<synthetic>" marks a reply claude made up locally (an API error, say).
  if (role != "assistant" || !usage.is_object() || model == "<synthetic>") return;
  if (!id.empty() && !r.ids.emplace(id).second) return;

  UsageStat s;
  js::Value details{};
  js::scan_object(usage.raw, [&](std::string_view k, const js::Value& v) {
    if (k == "input_tokens") s.input = u64(v);
    else if (k == "output_tokens") s.output = u64(v);
    else if (k == "cache_read_input_tokens") s.cache_read = u64(v);
    else if (k == "cache_creation_input_tokens") s.cache_write = u64(v);
    else if (k == "output_tokens_details") details = v;
    return true;
  });
  if (details.is_object()) {
    js::scan_object(details.raw, [&](std::string_view k, const js::Value& v) {
      if (k == "thinking_tokens") s.reasoning = u64(v);
      return true;
    });
  }
  s.total = s.input + s.output + s.cache_read + s.cache_write;
  e.stat.add(s);
  add_model(e, model, s);
}

}  // namespace

namespace {

// True when the record's head names it a user record: the first "type" in its
// first few hundred bytes reads "user". A user record — most of a transcript's
// bytes, being tool output — carries no usage. Other records cannot be told
// apart this way (an assistant's message, with its own "type", comes first),
// so a head that says anything else proves nothing.
bool head_says_user(std::string_view raw) {
  const std::string_view head = raw.substr(0, 400);
  const size_t at = head.find("\"type\":\"");
  return at != std::string_view::npos && head.substr(at + 8).starts_with("user\"");
}

}  // namespace

void ClaudeAdapter::read_usage(LineReader& j, UsageEntry& e, UsageResume& r) const {
  scan_forward(j, r, [&](std::string_view raw) {
    if (head_says_user(raw)) return;
    if (text::contains(raw, "\"usage\"")) scan_claude_message(raw, e, r);
    else if (text::contains(raw, "cost-state")) scan_claude_cost_state(raw, e);
  });
}

}  // namespace mico
