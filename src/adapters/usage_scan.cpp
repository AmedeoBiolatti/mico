#include "adapters/usage_scan.h"

#include <charconv>
#include <cstdlib>
#include <string>

namespace mico::usage {

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

}  // namespace mico::usage
