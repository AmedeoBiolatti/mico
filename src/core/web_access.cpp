#include "core/web_access.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
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
  bool lan = false;
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
    c.lan = c.on && v.ends_with(" lan");
  }
  return c;
}

void save(const WebConfig& c) {
  write_setting("web", std::string(c.on ? "on " : "off ") + std::to_string(c.port) + (c.lan ? " lan" : ""));
}

}  // namespace

bool web_enabled() { return config().on; }
int web_port() { return config().port; }

void set_web(bool on, int port) {
  WebConfig& c = config();
  c.on = on;
  if (!on) c.lan = false;
  if (port > 0 && port < 65536) c.port = port;
  save(c);
}

bool web_lan() { return config().on && config().lan; }

void set_web_lan(bool lan) {
  WebConfig& c = config();
  c.lan = lan;
  if (lan) c.on = true;
  save(c);
}

namespace {

// 10/8, 172.16/12, 192.168/16: an address a home or office network hands out.
bool private_v4(uint32_t a) {
  return (a >> 24) == 10 || (a >> 20) == (172u << 4 | 1) || (a >> 16) == (192u << 8 | 168);
}

// Docker, libvirt, LXC bridges and VPNs have private addresses too, but no
// phone on the WiFi reaches them.
bool virtual_interface(std::string_view name) {
  for (std::string_view p : {"docker", "br-", "veth", "virbr", "lxc", "lxd", "tailscale", "tun", "tap", "wg", "zt"})
    if (name.starts_with(p)) return true;
  return false;
}

template <class F>
void each_v4(F&& fn) {
  ifaddrs* list = nullptr;
  if (getifaddrs(&list) != 0) return;
  for (const ifaddrs* i = list; i; i = i->ifa_next) {
    if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK))
      continue;
    const uint32_t a = ntohl(reinterpret_cast<const sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr);
    if (!fn(std::string_view(i->ifa_name ? i->ifa_name : ""), a)) break;
  }
  freeifaddrs(list);
}

std::string dotted(uint32_t a) {
  return std::to_string(a >> 24) + "." + std::to_string((a >> 16) & 255) + "." + std::to_string((a >> 8) & 255) + "." +
         std::to_string(a & 255);
}

}  // namespace

std::string lan_address() {
  std::string found;
  each_v4([&](std::string_view name, uint32_t a) {
    if (!private_v4(a) || virtual_interface(name)) return true;
    found = dotted(a);
    return false;
  });
  return found;
}

bool own_lan_address(std::string_view ip) {
  in_addr want{};
  if (ip.empty() || ip.size() > 15 || inet_pton(AF_INET, std::string(ip).c_str(), &want) != 1) return false;
  bool own = false;
  each_v4([&](std::string_view, uint32_t a) {
    own = a == ntohl(want.s_addr);
    return !own;
  });
  return own;
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

std::string web_lan_url() {
  if (!web_lan()) return {};
  const std::string ip = lan_address();
  return ip.empty() ? std::string() : "http://" + ip + ":" + std::to_string(web_port()) + "/#token=" + web_token();
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

namespace {
std::string trimmed(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
  return s.substr(a, b - a);
}
}  // namespace

TailscaleServe tailscale_serve(int port) {
  const std::string p = std::to_string(port);
  proc::Options opt;
  opt.cap = 1u << 20;
  opt.timeout_ms = 5000;
  const proc::Result st = proc::capture({"tailscale", "serve", "status", "--json"}, opt);
  if (!st.ran) return {false, false, "tailscale is not installed (https://tailscale.com/download)"};
  const std::string body = trimmed(st.out);
  if (st.ok() && !body.empty() && body != "{}") {
    // Ours, and forwarding to this port: nothing to do. Ours but to another
    // port (the web view has moved since): put up again below, which replaces it.
    const bool here = body.find("127.0.0.1:" + p + "\"") != std::string::npos ||
                      body.find("localhost:" + p + "\"") != std::string::npos;
    if (here) return {true, true, {}};
    if (read_setting("web-serve") != "owned")
      return {false, false,
              "tailscale serve already has a configuration, left alone: make sure it forwards to http://localhost:" + p +
                  " (tailscale serve status)"};
  }
  opt.timeout_ms = 12000;
  const proc::Result r = proc::capture({"tailscale", "serve", "--bg", p}, opt);
  if (r.timed_out)
    return {false, false,
            "tailscale serve did not answer: Serve may be off for your tailnet. Run `tailscale serve --bg " + p +
                "` once by hand to see the link that turns it on"};
  if (!r.ok())
    return {false, false,
            "`tailscale serve --bg " + p +
                "` failed: it may need `sudo tailscale set --operator=$USER` once, or HTTPS enabled in the Tailscale admin console"};
  write_setting("web-serve", "owned");
  return {true, true, {}};
}

void tailscale_unserve() {
  if (read_setting("web-serve") != "owned") return;
  proc::Options opt;
  opt.timeout_ms = 8000;
  proc::capture({"tailscale", "serve", "--https=443", "off"}, opt);
  write_setting("web-serve", "");
}

}  // namespace mico
