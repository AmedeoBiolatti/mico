#include "core/web_access.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>

#include "base/fs.h"
#include "base/process.h"
#include "core/store.h"

namespace mico {
namespace {

struct WebConfig {
  bool loaded = false;
  bool on = false;
  int port = kDefaultWebPort;
};

WebConfig& config() {
  static WebConfig c;
  if (!c.loaded) {
    c.loaded = true;
    const std::string v = read_setting("web");
    c.on = v.starts_with("on");
    if (const size_t sp = v.find(' '); sp != std::string::npos) {
      const int p = std::atoi(v.c_str() + sp + 1);
      if (p > 0 && p < 65536) c.port = p;
    }
  }
  return c;
}

}  // namespace

bool web_enabled() { return config().on; }
int web_port() { return config().port; }

void set_web(bool on, int port) {
  WebConfig& c = config();
  c.on = on;
  if (port > 0 && port < 65536) c.port = port;
  write_setting("web", std::string(on ? "on " : "off ") + std::to_string(c.port));
}

namespace {

std::string& cached_token() {
  static std::string token;
  return token;
}

// 32 random bytes as hex, written to `path` for the user alone; empty when
// there is no randomness to be had, and then the web view stays shut.
std::string make_token(const std::string& path) {
  unsigned char b[32];
  const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  const bool got = fd >= 0 && read(fd, b, sizeof b) == ssize_t(sizeof b);
  if (fd >= 0) ::close(fd);
  if (!got) return {};
  static const char kHex[] = "0123456789abcdef";
  std::string token;
  for (unsigned char x : b) {
    token += kHex[x >> 4];
    token += kHex[x & 15];
  }
  // Written beside it and renamed over it: the old token stands until the new
  // one is whole.
  mkdir(config_dir().c_str(), 0700);
  const std::string tmp = path + ".new";
  unlink(tmp.c_str());
  const int out = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (out < 0) return {};  // a token that could not be kept would not survive a restart
  bool ok = write(out, (token + "\n").data(), token.size() + 1) == ssize_t(token.size() + 1);
  ok = ::close(out) == 0 && ok;
  if (ok && rename(tmp.c_str(), path.c_str()) == 0) return token;
  unlink(tmp.c_str());
  return {};
}

}  // namespace

std::string web_token() {
  std::string& token = cached_token();
  if (!token.empty()) return token;
  const std::string path = config_dir() + "/web-token";
  std::string buf;
  std::string_view v = fs::read_prefix(path, 128, buf);
  while (!v.empty() && (v.back() == '\n' || v.back() == ' ')) v.remove_suffix(1);
  struct stat st{};
  // A token anyone else could read is no secret: make a new one.
  if (v.size() == 64 && stat(path.c_str(), &st) == 0 && (st.st_mode & 077) == 0 && st.st_uid == getuid()) {
    token = std::string(v);
    return token;
  }
  token = make_token(path);
  return token;
}

bool new_web_token() {
  std::string t = make_token(config_dir() + "/web-token");
  if (t.empty()) return false;
  cached_token() = std::move(t);
  return true;
}

std::string web_url() {
  return "http://127.0.0.1:" + std::to_string(web_port()) + "/#token=" + web_token();
}

namespace {
bool valid_host(const std::string& h) {
  if (h.empty() || h.size() > 253) return false;
  for (const char c : h)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
  return true;
}
}  // namespace

std::string web_host() {
  const std::string h = read_setting("web-host");
  return valid_host(h) ? h : std::string();
}

void set_web_host(const std::string& host) {
  std::string h;
  for (const char c : host) h.push_back(c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c);
  while (!h.empty() && h.back() == '.') h.pop_back();  // a DNS name's trailing dot
  write_setting("web-host", valid_host(h) ? h : std::string());
}

std::string web_remote_url() {
  const std::string h = web_host();
  return h.empty() ? std::string() : "https://" + h + "/#token=" + web_token();
}

std::string tailscale_name() {
  proc::Options opt;
  opt.cap = 4u << 20;
  opt.timeout_ms = 5000;
  const std::string out = proc::capture({"tailscale", "status", "--json"}, opt).out;
  // "Self": {…, "DNSName": "machine.tailnet.ts.net.", …}
  const size_t self = out.find("\"Self\"");
  if (self == std::string::npos) return {};
  const size_t key = out.find("\"DNSName\"", self);
  if (key == std::string::npos) return {};
  const size_t open = out.find('"', out.find(':', key) + 1);
  if (open == std::string::npos) return {};
  const size_t close = out.find('"', open + 1);
  if (close == std::string::npos) return {};
  std::string name = out.substr(open + 1, close - open - 1);
  while (!name.empty() && name.back() == '.') name.pop_back();
  return valid_host(name) ? name : std::string();
}

}  // namespace mico
