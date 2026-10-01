#include "core/usage.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <chrono>

#include "base/json.h"
#include "base/jsonl.h"

namespace mico {

void UsageStat::add(const UsageStat& o) {
  input += o.input;
  output += o.output;
  cache_read += o.cache_read;
  cache_write += o.cache_write;
  reasoning += o.reasoning;
  total += o.total;
  cost_usd += o.cost_usd;
  has_cost = has_cost || o.has_cost;
}

namespace {

uint64_t u64(const js::Value& v) {
  if (v.type != js::Type::Number) return 0;
  uint64_t n = 0;
  std::from_chars(v.raw.data(), v.raw.data() + v.raw.size(), n);
  return n;
}

double f64(const js::Value& v) {
  if (v.type != js::Type::Number) return 0;
  double d = 0;
  std::from_chars(v.raw.data(), v.raw.data() + v.raw.size(), d);
  return d;
}

void add_model(UsageEntry& e, std::string_view name, const UsageStat& s) {
  if (name.empty()) name = "(unknown)";
  for (auto& m : e.models)
    if (m.model == name) { m.stat.add(s); return; }
  e.models.push_back(ModelUsage{std::string(name), s});
}

// pi and omp: one usage object per assistant message, each already carrying a
// per-message cost. Summing is the only way to a session total; the records are
// interleaved with tool results and telemetry, so the usage substring is
// rejected before anything is parsed.
void scan_pi(Jsonl& j, UsageEntry& e) {
  while (!j.complete()) j.extend_back();
  for (size_t i = 0; i < j.line_count(); i++) {
    std::string_view raw = j.line(i);
    if (raw.find("\"usage\"") == std::string_view::npos) continue;

    std::string_view type;
    js::Value message{};
    js::scan_object(raw, [&](std::string_view k, const js::Value& v) {
      if (k == "type") { type = v.body(); return type == "message"; }
      if (k == "message") { message = v; return false; }
      return true;
    });
    if (type != "message" || !message.is_object()) continue;

    std::string_view role, model;
    js::Value usage{};
    js::scan_object(message.raw, [&](std::string_view k, const js::Value& v) {
      if (k == "role") role = v.body();
      else if (k == "model") model = v.body();
      else if (k == "usage") usage = v;
      return true;
    });
    if (role != "assistant" || !usage.is_object()) continue;

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
  }
}

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

void scan_claude(Jsonl& j, UsageEntry& e, UsageResume& r) {
  scan_forward(j, r, [&](std::string_view raw) {
    if (raw.find("\"usage\"") != std::string_view::npos) scan_claude_message(raw, e, r);
    else if (raw.find("cost-state") != std::string_view::npos) scan_claude_cost_state(raw, e);
  });
}

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
  const bool turn = raw.find("\"turn_context\"") != std::string_view::npos;
  if (!turn && raw.find("\"token_count\"") == std::string_view::npos) return;
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

int64_t elapsed_ms(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - t0)
      .count();
}

}  // namespace

void PriceBook::add(const PriceSample& s) {
  for (auto& m : models_)
    if (m.name == s.model) { m.samples.push_back(s); return; }
  models_.push_back(Model{s.model, {s}});
}

namespace {

// Least squares for x in A x = b, A being n x 4. False when the samples do
// not pin all four prices down. Columns are scaled first: cache reads run to
// billions of tokens while fresh input is a handful.
bool solve4(const std::vector<PriceSample>& ss, double x[4]) {
  double scale[4] = {};
  const auto row = [](const PriceSample& s, int k) {
    const uint64_t v[4] = {s.input, s.output, s.cache_read, s.cache_write};
    return double(v[k]);
  };
  for (const auto& s : ss)
    for (int k = 0; k < 4; k++) scale[k] = std::max(scale[k], row(s, k));
  for (double& v : scale)
    if (v == 0) return false;  // a token kind never seen: its price is unknown
  double m[4][5] = {};
  for (const auto& s : ss)
    for (int a = 0; a < 4; a++) {
      const double ra = row(s, a) / scale[a];
      for (int b = 0; b < 4; b++) m[a][b] += ra * row(s, b) / scale[b];
      m[a][4] += ra * s.cost;
    }
  for (int c = 0; c < 4; c++) {
    int piv = c;
    for (int r = c + 1; r < 4; r++)
      if (std::fabs(m[r][c]) > std::fabs(m[piv][c])) piv = r;
    if (std::fabs(m[piv][c]) < 1e-9) return false;
    std::swap(m[c], m[piv]);
    for (int r = 0; r < 4; r++) {
      if (r == c) continue;
      const double f = m[r][c] / m[c][c];
      for (int k = c; k < 5; k++) m[r][k] -= f * m[c][k];
    }
  }
  for (int k = 0; k < 4; k++) {
    x[k] = m[k][4] / m[k][k] / scale[k];
    if (!(x[k] >= 0)) return false;
  }
  // claude's billing is exactly linear; a poor fit means the samples lied.
  for (const auto& s : ss) {
    double got = 0;
    for (int k = 0; k < 4; k++) got += x[k] * row(s, k);
    if (std::fabs(got - s.cost) > 0.01 * s.cost + 1e-4) return false;
  }
  return true;
}

}  // namespace

void PriceBook::fit() {
  // Anthropic's usual ratios, for a model with too few records to solve.
  constexpr double kShape[4] = {1, 5, 0.1, 2};
  for (auto& m : models_) {
    m.known = solve4(m.samples, m.price);
    if (m.known) continue;
    double num = 0, den = 0;
    for (const auto& s : m.samples) {
      const double u = kShape[0] * double(s.input) + kShape[1] * double(s.output) +
                       kShape[2] * double(s.cache_read) + kShape[3] * double(s.cache_write);
      num += u * s.cost;
      den += u * u;
    }
    if (den <= 0) continue;
    for (int k = 0; k < 4; k++) m.price[k] = kShape[k] * num / den;
    m.known = true;
  }
}

bool PriceBook::cost(std::string_view model, const UsageStat& s, double* out) const {
  for (const auto& m : models_) {
    if (m.name != model || !m.known) continue;
    *out = m.price[0] * double(s.input) + m.price[1] * double(s.output) +
           m.price[2] * double(s.cache_read) + m.price[3] * double(s.cache_write);
    return true;
  }
  return false;
}

void apply_prices(const PriceBook& book, UsageEntry& e) {
  if (e.agent != "claude") return;
  e.stat.cost_usd = 0;
  e.stat.has_cost = false;
  for (auto& m : e.models) {
    m.stat.has_cost = book.cost(m.model, m.stat, &m.stat.cost_usd);
    if (!m.stat.has_cost) m.stat.cost_usd = 0;
    e.stat.cost_usd += m.stat.cost_usd;
    e.stat.has_cost = e.stat.has_cost || m.stat.has_cost;
  }
}

UsageEntry usage_for_file(const std::string& path, const std::string& agent,
                          const std::string& cwd, const std::string& project, int64_t mtime,
                          UsageResume* resume, const UsageEntry* prev) {
  UsageResume fresh;
  UsageResume& r = resume ? *resume : fresh;
  UsageEntry e;
  if ((agent == "claude" || agent == "codex") && prev && r.offset > 0) e = *prev;
  else r = UsageResume{};
  e.path = path;
  e.agent = agent;
  e.cwd = cwd;
  e.project = project;
  e.mtime = mtime;

  Jsonl j;
  if (!j.open(path)) return e;
  if (agent == "pi" || agent == "omp") scan_pi(j, e);
  else if (agent == "claude") scan_claude(j, e, r);
  else if (agent == "codex") scan_forward(j, r, [&](std::string_view raw) { scan_codex_line(raw, e, r); });
  return e;
}

void UsageIndex::start(const std::vector<Project>& projects) {
  jobs_.clear();
  entries_.clear();

  for (const auto& p : projects) {
    for (const auto& s : p.sessions) {
      auto it = cache_.find(s.path);
      if (it != cache_.end() && it->second.mtime == s.mtime && it->second.size == s.bytes) {
        entries_.push_back(it->second.entry);
        continue;
      }
      jobs_.push_back(Job{s.path, s.agent, s.cwd, p.name, s.mtime, s.bytes});
    }
  }
  reprice();
}

bool UsageIndex::step(int budget_ms) {
  if (jobs_.empty()) return true;
  const auto t0 = std::chrono::steady_clock::now();
  while (!jobs_.empty()) {
    Job job = std::move(jobs_.back());
    jobs_.pop_back();
    Cache& c = cache_[job.path];
    // A transcript only grows; one that shrank was rewritten and is read anew.
    if (job.size < c.size) c.resume = UsageResume{};
    UsageEntry e = usage_for_file(job.path, job.agent, job.cwd, job.project, job.mtime,
                                  &c.resume, &c.entry);
    c.mtime = job.mtime;
    c.size = job.size;
    c.entry = e;
    entries_.push_back(std::move(e));
    if (elapsed_ms(t0) >= budget_ms) break;
  }
  reprice();
  return jobs_.empty();
}

void UsageIndex::reprice() {
  prices_.clear();
  for (const auto& [path, c] : cache_)
    for (const auto& s : c.entry.price_samples) prices_.add(s);
  prices_.fit();
  for (auto& e : entries_) apply_prices(prices_, e);
}

}  // namespace mico
