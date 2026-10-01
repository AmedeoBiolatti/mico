#include "core/web_access.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>

#include "base/fs.h"
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
    std::string buf;
    const std::string_view v = fs::read_prefix(config_dir() + "/web", 64, buf);
    c.on = v.starts_with("on");
    if (const size_t sp = v.find(' '); sp != std::string_view::npos) {
      const int p = std::atoi(std::string(v.substr(sp + 1)).c_str());
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
  mkdir(config_dir().c_str(), 0700);
  if (FILE* f = fopen((config_dir() + "/web").c_str(), "w")) {
    fprintf(f, "%s %d\n", on ? "on" : "off", c.port);
    fclose(f);
  }
}

std::string web_token() {
  static std::string token;
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
  unsigned char b[32];
  const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  const bool got = fd >= 0 && read(fd, b, sizeof b) == ssize_t(sizeof b);
  if (fd >= 0) ::close(fd);
  if (!got) return {};  // no randomness, no token: the web view stays shut
  static const char kHex[] = "0123456789abcdef";
  for (unsigned char x : b) {
    token += kHex[x >> 4];
    token += kHex[x & 15];
  }
  mkdir(config_dir().c_str(), 0700);
  unlink(path.c_str());
  const int out = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (out >= 0) {
    if (write(out, (token + "\n").data(), token.size() + 1) < 0) {}
    ::close(out);
  }
  return token;
}

std::string web_url() {
  return "http://127.0.0.1:" + std::to_string(web_port()) + "/#token=" + web_token();
}

}  // namespace mico
