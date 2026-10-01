#include "term/links.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace mico::links {
namespace {

struct Table {
  std::unordered_map<std::string, uint16_t> ids;
  std::vector<std::string> urls{std::string()};  // 0 is no link
  uint64_t gen = 1;
};

Table& table() {
  static Table t;
  return t;
}

bool openable(std::string_view u) {
  if (u.size() > 2048) return false;
  for (unsigned char c : u)
    if (c < 0x21 || c == 0x7F) return false;  // no spaces, no escapes
  return u.starts_with("https://") || u.starts_with("http://") || u.starts_with("file://") ||
         u.starts_with("mailto:");
}

}  // namespace

uint16_t intern(std::string_view url) {
  if (!openable(url)) return 0;
  Table& t = table();
  if (auto it = t.ids.find(std::string(url)); it != t.ids.end()) return it->second;
  if (t.urls.size() >= 65535) {
    // Full: start again. Anything laid out with the old ids is relaid out.
    t.ids.clear();
    t.urls.resize(1);
    t.gen++;
  }
  const uint16_t id = uint16_t(t.urls.size());
  t.urls.emplace_back(url);
  t.ids.emplace(t.urls.back(), id);
  return id;
}

std::string_view url(uint16_t id) {
  const Table& t = table();
  return id < t.urls.size() ? std::string_view(t.urls[id]) : std::string_view();
}

uint64_t generation() { return table().gen; }

}  // namespace mico::links
