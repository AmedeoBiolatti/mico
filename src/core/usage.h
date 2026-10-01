#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "core/store.h"

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

  void add(const UsageStat& o);
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

// Per-token prices learned from claude's own cost-state records, never typed
// in: claude bills every model linearly in the four token kinds, so a handful
// of records per model solves for its four prices exactly. A model seen in
// too few records to solve outright keeps Anthropic's usual shape (output 5x
// input, cache read 0.1x, cache write 2x) scaled to what it was charged. A
// model no record mentions has no price.
class PriceBook {
 public:
  void clear() { models_.clear(); }
  void add(const PriceSample& s);
  // Solves every model's prices from the samples added so far.
  void fit();
  // Dollars for `s` at `model`'s prices; false when the model has none.
  bool cost(std::string_view model, const UsageStat& s, double* out) const;

 private:
  struct Model {
    std::string name;
    std::vector<PriceSample> samples;
    double price[4] = {};  // input, output, cache read, cache write; $/token
    bool known = false;
  };
  std::vector<Model> models_;
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

// Extracts one transcript's usage. Every agent's file is walked, since each
// records usage per message or per step; for claude and codex, `resume` (when
// given) skips what an earlier call already counted into `prev` and is
// advanced past what this one reads.
//
// claude's own cost-state record is no total to trust: it is written only
// now and then, and counts only the process that wrote it, so a resumed
// session or a turn since the last record goes missing. Its dollar figures
// still price the tokens, through a PriceBook.
UsageEntry usage_for_file(const std::string& path, const std::string& agent,
                          const std::string& cwd, const std::string& project,
                          int64_t mtime, UsageResume* resume = nullptr,
                          const UsageEntry* prev = nullptr);

// Fills in claude's dollars on `e` (and its models) from `book`.
void apply_prices(const PriceBook& book, UsageEntry& e);

// An incremental index over the sessions a Store knows about. A pass is started
// with start(); each step() reads a few files and returns, so a slow disk never
// blocks a frame. A file whose mtime and size are unchanged is reused untouched;
// start() rebuilds the visible list from that cache before the pass runs, so a
// refresh after nothing moved is a stat() per session and no parsing.
class UsageIndex {
 public:
  void start(const std::vector<Project>& projects);
  // Scans for up to `budget_ms`. Returns true when the current pass is complete.
  bool step(int budget_ms);
  bool complete() const { return jobs_.empty(); }
  size_t done() const { return entries_.size(); }
  size_t total() const { return entries_.size() + jobs_.size(); }
  const std::vector<UsageEntry>& entries() const { return entries_; }

 private:
  void reprice();

  struct Cache {
    int64_t mtime = 0;
    uint64_t size = 0;
    UsageEntry entry;
    UsageResume resume;
  };
  struct Job {
    std::string path, agent, cwd, project;
    int64_t mtime = 0;
    uint64_t size = 0;
  };
  std::vector<Job> jobs_;
  std::vector<UsageEntry> entries_;
  std::map<std::string, Cache> cache_;
  PriceBook prices_;
};

}  // namespace mico
