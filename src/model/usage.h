#pragma once
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace mico {

// Tokens and, where the agent reports it, money — normalized across claude,
// codex, pi and omp. "input" excludes cache reads and writes, which are kept
// apart so a cache-hit rate and a true prompt size both stay meaningful. The
// four agents disagree on almost every field name; this is the common shape
// they all collapse onto, and an agent that reports none of it reads as zero.
struct UsageStat {
  uint64_t input = 0;        // fresh, non-cached prompt tokens
  uint64_t output = 0;
  uint64_t cache_read = 0;
  uint64_t cache_write = 0;
  uint64_t reasoning = 0;    // subset of output; shown only when reported
  uint64_t total = 0;
  double cost_usd = 0;
  bool has_cost = false;

  void add(const UsageStat& o) {
    input += o.input;
    output += o.output;
    cache_read += o.cache_read;
    cache_write += o.cache_write;
    reasoning += o.reasoning;
    total += o.total;
    cost_usd += o.cost_usd;
    has_cost = has_cost || o.has_cost;
  }
  uint64_t prompt_tokens() const { return input + cache_read + cache_write; }
  double cached_pct() const {
    const uint64_t p = prompt_tokens();
    return p ? 100.0 * double(cache_read) / double(p) : 0.0;
  }
};

// One model's share of a session. claude breaks a session out by model in its
// cost-state record; pi and omp report the model on every message.
struct ModelUsage {
  std::string model;
  UsageStat stat;
};

// One model's cumulative tokens and the dollars claude charged for them, as a
// cost-state record states it. Many of these pin down the model's prices.
struct PriceSample {
  std::string model;
  uint64_t input = 0, output = 0, cache_read = 0, cache_write = 0;
  double cost = 0;
};

// A transcript's usage. `models` is empty when the agent does not break it out.
struct UsageEntry {
  std::string path;
  std::string agent;
  std::string cwd;
  std::string project;  // display name of the tracked folder
  int64_t mtime = 0;
  UsageStat stat;
  std::vector<ModelUsage> models;

  // codex only: the account's rolling rate limit, from its newest token_count
  // record. Every codex session on the account reports the same window. codex
  // records no prices, so its sessions carry tokens and no dollars.
  bool has_quota = false;
  double quota_used_pct = 0;
  int64_t quota_resets_at = 0;       // unix seconds
  int64_t quota_window_minutes = 0;

  // claude only: its cost-state records, for the PriceBook. The dollars on
  // stat and models are filled in from that book, not read off the file.
  std::vector<PriceSample> price_samples;
};

// How far an append-only claude or codex transcript has been read, so a
// session that is still running costs only its new lines on the next pass.
struct UsageResume {
  uint64_t offset = 0;                   // first byte not yet read
  std::unordered_set<std::string> ids;  // claude: messages already counted
  std::string model;                     // codex: the current turn's model
  UsageStat total;                       // codex: the last running total seen
  bool has_total = false;
};

}  // namespace mico
