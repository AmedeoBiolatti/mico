#include "core/usage.h"

#include "adapters/adapters.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <chrono>

#include "base/json.h"
#include "base/jsonl.h"
#include "base/line_reader.h"

namespace mico {


namespace {

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
  const Adapter* adapter = adapter_for(e.agent);
  if (!adapter || !adapter->prices_from_samples()) return;
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
  const Adapter* adapter = adapter_for(agent);
  if (adapter && adapter->resumes_usage() && prev && r.offset > 0) e = *prev;
  else r = UsageResume{};
  e.path = path;
  e.agent = agent;
  e.cwd = cwd;
  e.project = project;
  e.mtime = mtime;

  LineReader j(path);
  if (adapter) adapter->read_usage(j, e, r);
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
