#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "core/store.h"
#include "model/usage.h"

namespace mico {

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
