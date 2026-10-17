#include "base/path.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdlib>
#include <vector>

namespace mico {

std::string file_url(std::string_view path) {
  constexpr char hex[] = "0123456789ABCDEF";
  std::string out = "file://";
  for (unsigned char c : path) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
        c == '/' || c == '-' || c == '_' || c == '.' || c == '~') out += char(c);
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
  }
  return out;
}

std::string file_url_path(std::string_view url) {
  if (!url.starts_with("file:///")) return {};
  url.remove_prefix(7);
  url = url.substr(0, url.find('#'));
  const auto hex = [](char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
  };
  std::string out;
  for (size_t i = 0; i < url.size(); i++) {
    char c = url[i];
    if (c == '%' && i + 2 < url.size() && hex(url[i + 1]) >= 0 && hex(url[i + 2]) >= 0) {
      c = char(hex(url[i + 1]) * 16 + hex(url[i + 2]));
      i += 2;
    }
    if (!c) return {};  // exec arguments cannot contain NUL
    out += c;
  }
  return out;
}

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
