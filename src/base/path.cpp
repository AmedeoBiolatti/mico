#include "base/path.h"

#include <cstdlib>
#include <vector>

namespace mico {

std::string resolve_path(std::string_view cwd, std::string_view p) {
  if (p.empty()) return {};
  std::string full;
  if (p[0] == '/') {
    full = p;
  } else if (p.starts_with("~/")) {
    const char* h = getenv("HOME");
    full = std::string(h ? h : "") + std::string(p.substr(1));
  } else {
    full = std::string(cwd) + "/" + std::string(p);
  }
  std::vector<std::string_view> parts;
  std::string_view s = full;
  for (size_t i = 0; i <= s.size();) {
    size_t e = s.find('/', i);
    if (e == std::string_view::npos) e = s.size();
    const std::string_view part = s.substr(i, e - i);
    if (part == "..") {
      if (!parts.empty()) parts.pop_back();
    } else if (!part.empty() && part != ".") {
      parts.push_back(part);
    }
    i = e + 1;
  }
  std::string out;
  for (std::string_view part : parts) out += "/" + std::string(part);
  return out.empty() ? "/" : out;
}

}  // namespace mico
