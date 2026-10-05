#include "base/path.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdlib>
#include <vector>

namespace mico {

namespace {
std::string home_dir() {
  const char* h = getenv("HOME");
  return h ? h : ".";
}
}  // namespace

std::string config_dir() {
  if (const char* x = getenv("XDG_CONFIG_HOME"); x && *x) return std::string(x) + "/mico";
  return home_dir() + "/.config/mico";
}

std::string state_dir() {
  if (const char* x = getenv("XDG_STATE_HOME"); x && *x) return std::string(x) + "/mico";
  return home_dir() + "/.local/state/mico";
}

bool write_file_atomic(const std::string& path, std::string_view data) {
  const std::string tmp = path + ".tmp." + std::to_string(getpid());
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) return false;
  bool ok = true;
  for (size_t at = 0; ok && at < data.size();) {
    const ssize_t n = ::write(fd, data.data() + at, data.size() - at);
    if (n > 0) at += size_t(n);
    else ok = false;
  }
  ok = ::close(fd) == 0 && ok;
  if (ok && rename(tmp.c_str(), path.c_str()) == 0) return true;
  unlink(tmp.c_str());
  return false;
}

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
