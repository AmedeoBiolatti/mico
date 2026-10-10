#include "adapters/cmdline.h"

#include <algorithm>

#include "base/uuid.h"
#include "base/path.h"

namespace mico::cmdline {

std::string_view program(const std::vector<std::string>& argv) {
  if (argv.empty()) return {};
  const std::string_view a = argv[0];
  return a.substr(a.rfind('/') + 1);
}

bool mentions(const std::vector<std::string>& argv, std::string_view needle) {
  return std::any_of(argv.begin(), argv.end(),
                     [&](const std::string& a) { return a.find(needle) != std::string::npos; });
}

bool has_flag(const std::vector<std::string>& argv, std::string_view flag) {
  return std::any_of(argv.begin(), argv.end(), [&](const std::string& a) {
    return a == flag || (a.starts_with(flag) && a.size() > flag.size() && a[flag.size()] == '=');
  });
}

void adopt_cwd(std::vector<std::string>& argv, std::string& cwd,
               const std::vector<std::string_view>& flags) {
  size_t index = argv.size(), prefix = 0;
  for (size_t i = 1; i < argv.size(); i++) {
    if (argv[i] == "--") break;
    for (const auto flag : flags) {
      if (argv[i] == flag && i + 1 < argv.size()) { index = ++i; prefix = 0; break; }
      if (argv[i].starts_with(flag) && argv[i].size() > flag.size() && argv[i][flag.size()] == '=') {
        index = i; prefix = flag.size() + 1; break;
      }
      if (flag.size() == 2 && argv[i].starts_with(flag) && argv[i].size() > 2) {
        index = i; prefix = 2; break;
      }
    }
  }
  if (index == argv.size() || argv[index].size() == prefix) return;
  const std::string resolved = resolve_path(cwd, std::string_view(argv[index]).substr(prefix));
  argv[index] = argv[index].substr(0, prefix) + resolved;
  cwd = resolved;
}

void adopt_session_id(std::vector<std::string>& argv, std::string& id) {
  bool has = false;
  for (size_t i = 0; i < argv.size(); i++) {
    if (argv[i] == "--session-id" && i + 1 < argv.size()) {
      has = true;
      id = argv[i + 1];
    } else if (argv[i].starts_with("--session-id=")) {
      has = true;
      id = argv[i].substr(13);
    }
  }
  if (!has) {
    id = make_uuid_v4();
    argv.push_back("--session-id");
    argv.push_back(id);
  }
}

std::string toml_string(std::string_view v) {
  std::string out = "\"";
  for (const char c : v) {
    if (c == '"' || c == '\\') out += '\\';
    if (c == '\n') { out += "\\n"; continue; }
    out += c;
  }
  return out + '"';
}

std::string json_string(std::string_view v) {
  std::string out = "\"";
  for (const char c : v) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out + '"';
}

}  // namespace mico::cmdline
