#include "net/web.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <optional>

#include "base/json.h"
#include "base/log.h"
#include "core/web_access.h"

// The page, its script and its styles, linked in whole, as the math atlas is.
#define MICO_WEB_ASSET(name, file)                  \
  extern "C" const unsigned char name[];            \
  extern "C" const unsigned char name##_end[];      \
  asm(".section .rodata\n"                          \
      ".global " #name "\n"                         \
      ".global " #name "_end\n"                     \
      #name ":\n"                                   \
      ".incbin \"" MICO_WEB_DIR "/" file "\"\n"     \
      #name "_end:\n"                               \
      ".previous\n");
MICO_WEB_ASSET(mico_web_index, "index.html")
MICO_WEB_ASSET(mico_web_app_js, "app.js")
MICO_WEB_ASSET(mico_web_app_css, "app.css")

namespace mico {
namespace {

constexpr size_t kMaxRequest = 16u << 10;   // request line and headers
constexpr size_t kMaxMessage = 1u << 20;    // one WebSocket message
constexpr size_t kMaxQueued = 64u << 20;    // unsent output before a client is dropped
constexpr size_t kMaxConns = 32;            // connections at once; more are turned away
constexpr int64_t kHttpTimeoutMs = 10'000;  // for a request to arrive whole

int64_t now_ms() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// The token a browser offers: the subprotocol "mico-token.<hex>" among those
// it lists. A header, not the address, so no proxy in front writes it to a log.
// Empty when it names none, and then it is asked a challenge instead.
std::optional<std::string> offered_token(std::string_view protocols) {
  while (!protocols.empty()) {
    const size_t comma = protocols.find(',');
    std::string_view p = protocols.substr(0, comma);
    while (!p.empty() && (p.front() == ' ' || p.front() == '\t')) p.remove_prefix(1);
    while (!p.empty() && (p.back() == ' ' || p.back() == '\t')) p.remove_suffix(1);
    if (p.starts_with("mico-token.")) return std::string(p.substr(11));
    if (comma == std::string_view::npos) break;
    protocols.remove_prefix(comma + 1);
  }
  return std::nullopt;
}

std::string_view asset(const unsigned char* a, const unsigned char* b) {
  return {reinterpret_cast<const char*>(a), size_t(b - a)};
}

std::string lower(std::string_view s) {
  std::string out(s);
  for (char& c : out)
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  return out;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
  return s;
}

// Compares in time that does not depend on where the strings differ.
bool same_secret(std::string_view a, std::string_view b) {
  if (a.size() != b.size() || a.empty()) return false;
  unsigned char diff = 0;
  for (size_t i = 0; i < a.size(); i++) diff |= uint8_t(a[i] ^ b[i]);
  return diff == 0;
}

// ---------------------------------------------------------------- SHA-1, base64

std::string sha1(std::string_view msg) {
  uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  std::string m(msg);
  const uint64_t bits = uint64_t(msg.size()) * 8;
  m += char(0x80);
  while (m.size() % 64 != 56) m += char(0);
  for (int i = 7; i >= 0; i--) m += char(uint8_t(bits >> (i * 8)));
  auto rol = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
  for (size_t off = 0; off < m.size(); off += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
      w[i] = uint32_t(uint8_t(m[off + 4 * i])) << 24 | uint32_t(uint8_t(m[off + 4 * i + 1])) << 16 |
             uint32_t(uint8_t(m[off + 4 * i + 2])) << 8 | uint32_t(uint8_t(m[off + 4 * i + 3]));
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
      uint32_t f, k;
      if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
      else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
      else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
      else { f = b ^ c ^ d; k = 0xCA62C1D6; }
      const uint32_t t = rol(a, 5) + f + e + k + w[i];
      e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
  }
  std::string out;
  for (uint32_t x : h)
    for (int i = 3; i >= 0; i--) out += char(uint8_t(x >> (i * 8)));
  return out;
}

std::string sha256(std::string_view msg) {
  static constexpr uint32_t k[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
      0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
      0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
      0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
      0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::string m(msg);
  const uint64_t bits = uint64_t(msg.size()) * 8;
  m += char(0x80);
  while (m.size() % 64 != 56) m += char(0);
  for (int i = 7; i >= 0; i--) m += char(uint8_t(bits >> (i * 8)));
  auto ror = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
  for (size_t off = 0; off < m.size(); off += 64) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
      w[i] = uint32_t(uint8_t(m[off + 4 * i])) << 24 | uint32_t(uint8_t(m[off + 4 * i + 1])) << 16 |
             uint32_t(uint8_t(m[off + 4 * i + 2])) << 8 | uint32_t(uint8_t(m[off + 4 * i + 3]));
    for (int i = 16; i < 64; i++) {
      const uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
      const uint32_t t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
      const uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }
  std::string out;
  for (uint32_t x : h)
    for (int i = 3; i >= 0; i--) out += char(uint8_t(x >> (i * 8)));
  return out;
}

std::string hex(std::string_view bytes) {
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  for (const char ch : bytes) {
    out += kHex[uint8_t(ch) >> 4];
    out += kHex[uint8_t(ch) & 15];
  }
  return out;
}

// A challenge's nonce: 32 random bytes as hex; empty when there is no
// randomness, and then the socket is refused.
std::string nonce() {
  unsigned char b[32];
  const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  const bool got = fd >= 0 && read(fd, b, sizeof b) == ssize_t(sizeof b);
  if (fd >= 0) ::close(fd);
  return got ? hex(std::string_view(reinterpret_cast<const char*>(b), sizeof b)) : std::string();
}

std::string base64(std::string_view in) {
  static const char kAlpha[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    const uint32_t v = uint32_t(uint8_t(in[i])) << 16 | uint32_t(uint8_t(in[i + 1])) << 8 | uint8_t(in[i + 2]);
    out += kAlpha[v >> 18]; out += kAlpha[(v >> 12) & 63]; out += kAlpha[(v >> 6) & 63]; out += kAlpha[v & 63];
  }
  if (i + 1 == in.size()) {
    const uint32_t v = uint32_t(uint8_t(in[i])) << 16;
    out += kAlpha[v >> 18]; out += kAlpha[(v >> 12) & 63]; out += "==";
  } else if (i + 2 == in.size()) {
    const uint32_t v = uint32_t(uint8_t(in[i])) << 16 | uint32_t(uint8_t(in[i + 1])) << 8;
    out += kAlpha[v >> 18]; out += kAlpha[(v >> 12) & 63]; out += kAlpha[(v >> 6) & 63]; out += '=';
  }
  return out;
}

}  // namespace

std::string websocket_accept(std::string_view key) {
  return base64(sha1(std::string(key) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
}

std::string hmac_sha256_hex(std::string_view key, std::string_view message) {
  std::string k = key.size() > 64 ? sha256(key) : std::string(key);
  k.resize(64, '\0');
  std::string inner(64, '\0'), outer(64, '\0');
  for (int i = 0; i < 64; i++) {
    inner[i] = char(k[i] ^ 0x36);
    outer[i] = char(k[i] ^ 0x5c);
  }
  return hex(sha256(outer + sha256(inner + std::string(message))));
}

std::string web_auth_proof(std::string_view token, std::string_view nonce) {
  return hmac_sha256_hex(token, "mico-auth " + std::string(nonce));
}

// One browser connection: an HTTP request, then, once upgraded, a WebSocket
// with a protocol client behind it.
struct WebServer::Conn {
  int fd = -1;
  std::string in, out;
  bool ws = false;
  bool dead = false;
  bool closing = false;  // close once `out` drains
  int64_t opened_ms = 0;
  std::string host;      // the Host it asked for, once read
  std::string token;     // the one it was let in with; another made since lets it go
  std::string nonce;     // the challenge it has yet to answer
  std::string message;   // a fragmented message, so far
  std::unique_ptr<api::Client> api;
};

WebServer::WebServer(Workspace& ws) : ws_(ws) {}

WebServer::~WebServer() {
  for (auto& c : conns_) close(c->fd);
  if (lfd_ >= 0) close(lfd_);
}

void WebServer::sync() {
  const bool want = web_enabled() && !web_token().empty();
  if (want && lfd_ >= 0 && port_ == web_port() && lan_ == web_lan()) return;
  // A port that could not be had is not tried again every turn of the loop:
  // only once the setting changes.
  if (want && lfd_ < 0 && failed_port_ == web_port()) return;
  if (!want) failed_port_ = 0;
  if (!want && lfd_ < 0) return;
  // Off, or moving to another port or onto (off) the network: let everything go.
  for (auto& c : conns_) close(c->fd);
  conns_.clear();
  if (lfd_ >= 0) {
    close(lfd_);
    lfd_ = -1;
    MLOG("web: stopped");
  }
  if (!want) return;
  const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0) return;
  const int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(uint16_t(web_port()));
  const bool lan = web_lan();
  const char* where = lan ? "every address" : "127.0.0.1";
  addr.sin_addr.s_addr = htonl(lan ? INADDR_ANY : INADDR_LOOPBACK);
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || listen(fd, 16) != 0) {
    MLOG("web: cannot listen on %s, port %d: %s", where, web_port(), strerror(errno));
    close(fd);
    // Remembered here, not written to the settings: a port another process
    // holds now is no reason to turn the web view off for good.
    failed_port_ = web_port();
    return;
  }
  lfd_ = fd;
  port_ = web_port();
  lan_ = lan;
  MLOG("web: listening on %s, port %d", where, port_);
}

void WebServer::add_fds(std::vector<pollfd>& fds) {
  fds_from_ = fds.size();
  polled_ = 0;
  if (lfd_ < 0) return;
  fds.push_back(pollfd{lfd_, POLLIN, 0});
  for (auto& c : conns_) fds.push_back(pollfd{c->fd, short(POLLIN | (c->out.empty() ? 0 : POLLOUT)), 0});
  polled_ = conns_.size();
}

void WebServer::handle(const std::vector<pollfd>& fds) {
  if (lfd_ < 0 || fds_from_ >= fds.size()) return;
  if (fds[fds_from_].revents & POLLIN) accept_all();
  for (size_t i = 0; i < polled_ && i < conns_.size(); i++) {
    Conn& c = *conns_[i];
    const short re = fds[fds_from_ + 1 + i].revents;
    if (re & (POLLHUP | POLLERR | POLLNVAL)) c.dead = true;
    if (re & POLLOUT) flush(c);
    if (re & POLLIN) {
      char buf[16384];
      for (;;) {
        const ssize_t n = read(c.fd, buf, sizeof buf);
        if (n > 0) { c.in.append(buf, size_t(n)); continue; }
        if (n == 0) c.dead = true;
        else if (errno == EINTR) continue;
        break;
      }
      if (c.ws) read_ws(c);
      else read_http(c);
    }
  }
  std::erase_if(conns_, [](const std::unique_ptr<Conn>& c) {
    if (!c->dead) return false;
    close(c->fd);
    return true;
  });
}

void WebServer::pump() {
  std::vector<std::string> msgs;
  const int64_t now = now_ms();
  const std::string token = web_token();
  for (auto& c : conns_) {
    // A request that never comes whole holds a connection for nothing.
    // Nor does an answer to a challenge that never comes.
    if (!c->api && !c->dead && now - c->opened_ms > kHttpTimeoutMs) c->dead = true;
    // The token changed (:web new-token): whoever came in with the old one goes.
    if (c->api && !c->dead && !same_secret(c->token, token)) {
      MLOG("web: a browser let go: the token changed");
      send_frame(*c, 8, "\x03\xf0");  // 1008: policy
      c->closing = true;
      flush(*c);
      continue;
    }
    if (!c->ws || !c->api || c->dead) continue;
    msgs.clear();
    c->api->poll(msgs);
    for (const auto& m : msgs) send_text(*c, m);
    flush(*c);
  }
}

void WebServer::accept_all() {
  for (;;) {
    const int fd = accept4(lfd_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (fd < 0) return;
    if (conns_.size() >= kMaxConns) {
      close(fd);
      continue;
    }
    auto c = std::make_unique<Conn>();
    c->fd = fd;
    c->opened_ms = now_ms();
    conns_.push_back(std::move(c));
  }
}

bool WebServer::allowed_host(std::string_view host) const {
  const std::string port = ":" + std::to_string(port_);
  if (host == "127.0.0.1" + port || host == "localhost" + port) return true;
  // The name something in front of this (tailscale serve) forwards under, and
  // no other: a page on a site that points its own name here is refused.
  const std::string remote = web_host();
  return (!remote.empty() && lower(host) == remote) || lan_host(host);
}

// This machine's own address on the network, with the port, while it listens
// there: what a browser on the WiFi names. Plain http, so the token stays home.
bool WebServer::lan_host(std::string_view host) const {
  const std::string port = ":" + std::to_string(port_);
  if (!lan_ || !host.ends_with(port)) return false;
  host.remove_suffix(port.size());
  return host != "127.0.0.1" && own_lan_address(host);
}

void WebServer::read_http(Conn& c) {
  const size_t end = c.in.find("\r\n\r\n");
  if (end == std::string::npos) {
    if (c.in.size() > kMaxRequest) respond(c, 431, "text/plain", "request too large\n");
    return;
  }
  const std::string_view req(c.in.data(), end);
  const size_t eol = req.find("\r\n");
  const std::string_view line = req.substr(0, eol);
  std::string method, target;
  {
    const size_t a = line.find(' '), b = line.rfind(' ');
    if (a == std::string_view::npos || b <= a) return respond(c, 400, "text/plain", "bad request\n");
    method = std::string(line.substr(0, a));
    target = std::string(line.substr(a + 1, b - a - 1));
  }
  std::string host, origin, upgrade, connection, key, version, protocols;
  for (size_t at = eol == std::string_view::npos ? req.size() : eol + 2; at < req.size();) {
    size_t e = req.find("\r\n", at);
    if (e == std::string_view::npos) e = req.size();
    const std::string_view h = req.substr(at, e - at);
    at = e + 2;
    const size_t colon = h.find(':');
    if (colon == std::string_view::npos) continue;
    const std::string name = lower(trim(h.substr(0, colon)));
    const std::string_view value = trim(h.substr(colon + 1));
    if (name == "host") host = value;
    else if (name == "origin") origin = value;
    else if (name == "upgrade") upgrade = lower(value);
    else if (name == "connection") connection = lower(value);
    else if (name == "sec-websocket-key") key = value;
    else if (name == "sec-websocket-version") version = value;
    else if (name == "sec-websocket-protocol") protocols += std::string(protocols.empty() ? "" : ",") + std::string(value);
  }
  c.in.erase(0, end + 4);

  // A page on another site that resolves its own name to 127.0.0.1 still
  // sends that name as the host.
  if (!allowed_host(host)) return respond(c, 421, "text/plain", "wrong host\n");
  c.host = host;
  if (method != "GET") return respond(c, 405, "text/plain", "GET only\n");
  std::string path = target;
  if (const size_t q = target.find('?'); q != std::string::npos) path = target.substr(0, q);

  if (path == "/ws") {
    // The page came over http from 127.0.0.1, or over https from the name
    // that is forwarded here.
    const std::string own = "http://" + host;
    const std::string remote = web_host();
    const bool forwarded = !remote.empty() && lower(host) == remote;
    const std::optional<std::string> token = offered_token(protocols);
    if (upgrade != "websocket" || connection.find("upgrade") == std::string::npos || key.empty() || version != "13")
      return respond(c, 400, "text/plain", "a WebSocket is expected here\n");
    if (origin != own && !(forwarded && lower(origin) == "https://" + remote))
      return respond(c, 403, "text/plain", "wrong origin\n");
    // Nor in the address, which proxies and histories keep.
    if (target.find("token=") != std::string::npos) return respond(c, 403, "text/plain", "no token in the address\n");
    // A token in a header over the WiFi has already been seen by whoever
    // listens: refused, so that no client gets used to sending it.
    if (token && lan_host(host))
      return respond(c, 403, "text/plain", "no token over plain http: answer the challenge\n");
    if (token && !same_secret(*token, web_token())) return respond(c, 403, "text/plain", "wrong token\n");
    if (!token && (c.nonce = nonce()).empty()) return respond(c, 403, "text/plain", "no challenge\n");
    // The browser wants one of its subprotocols named back; "mico" is the one
    // that says nothing secret.
    c.out += "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
             "Sec-WebSocket-Protocol: mico\r\nSec-WebSocket-Accept: " + websocket_accept(key) + "\r\n\r\n";
    c.ws = true;
    if (token) {
      c.token = *token;
      c.api = std::make_unique<api::Client>(ws_);
      MLOG("web: a browser connected");
    } else {
      send_text(c, "{\"type\":\"challenge\",\"nonce\":\"" + c.nonce + "\"}");
    }
    flush(c);
    if (!c.in.empty()) read_ws(c);
    return;
  }
  if (path == "/" || path == "/index.html")
    return respond(c, 200, "text/html; charset=utf-8", asset(mico_web_index, mico_web_index_end));
  if (path == "/app.js")
    return respond(c, 200, "text/javascript; charset=utf-8", asset(mico_web_app_js, mico_web_app_js_end));
  if (path == "/app.css")
    return respond(c, 200, "text/css; charset=utf-8", asset(mico_web_app_css, mico_web_app_css_end));
  respond(c, 404, "text/plain", "not found\n");
}

void WebServer::respond(Conn& c, int status, std::string_view type, std::string_view body) {
  const char* reason = status == 200 ? "OK" : status == 404 ? "Not Found" : status == 403 ? "Forbidden"
                       : status == 405 ? "Method Not Allowed" : status == 421 ? "Misdirected Request"
                       : status == 431 ? "Request Header Fields Too Large" : "Bad Request";
  const std::string port = std::to_string(port_);
  c.out += "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\nContent-Type: " + std::string(type) +
           "\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer"
           "\r\nContent-Security-Policy: default-src 'self'; img-src 'self' data:; connect-src ws://127.0.0.1:" + port +
           " ws://localhost:" + port + (web_host().empty() ? "" : " wss://" + web_host()) +
           (lan_host(c.host) ? " ws://" + c.host : "") +
           "; frame-ancestors 'none'\r\nConnection: close\r\n\r\n";
  c.out += body;
  c.closing = true;
  flush(c);
}

void WebServer::read_ws(Conn& c) {
  for (;;) {
    if (c.in.size() < 2) return;
    const uint8_t b0 = uint8_t(c.in[0]), b1 = uint8_t(c.in[1]);
    const bool fin = b0 & 0x80;
    const int opcode = b0 & 0x0F;
    size_t len = b1 & 0x7F, at = 2;
    if (!(b1 & 0x80)) { c.dead = true; return; }  // a client's frames are always masked
    if (len == 126) {
      if (c.in.size() < 4) return;
      len = size_t(uint8_t(c.in[2])) << 8 | uint8_t(c.in[3]);
      at = 4;
    } else if (len == 127) {
      if (c.in.size() < 10) return;
      len = 0;
      for (int i = 0; i < 8; i++) len = len << 8 | uint8_t(c.in[2 + i]);
      at = 10;
    }
    if (len > kMaxMessage || c.message.size() + len > kMaxMessage) {
      send_frame(c, 8, "\x03\xf1");  // 1009: too big
      c.closing = true;
      return;
    }
    if (c.in.size() < at + 4 + len) return;
    const char* mask = c.in.data() + at;
    std::string payload = c.in.substr(at + 4, len);
    for (size_t i = 0; i < len; i++) payload[i] = char(payload[i] ^ mask[i % 4]);
    c.in.erase(0, at + 4 + len);

    switch (opcode) {
      case 0x0:  // continuation
      case 0x1:  // text
        c.message += payload;
        if (fin) {
          if (c.api) c.api->receive(c.message);
          else authenticate(c, c.message);
          c.message.clear();
          if (c.closing) return;
        }
        break;
      case 0x8:  // close
        send_frame(c, 8, payload.substr(0, 2));
        c.closing = true;
        return;
      case 0x9:  // ping
        send_frame(c, 0xA, payload);
        break;
      case 0xA: break;  // pong
      default:          // binary, or something unknown: the protocol is text
        send_frame(c, 8, "\x03\xeb");  // 1003: unsupported data
        c.closing = true;
        return;
    }
  }
}

// The answer to the challenge, and the first message read: the right proof
// lets the client in, anything else closes the socket.
void WebServer::authenticate(Conn& c, std::string_view message) {
  std::string type, proof;
  js::scan_object(message, [&](std::string_view k, const js::Value& v) {
    if (k == "type" && v.literal()) type = v.body();
    else if (k == "proof" && v.literal()) proof = v.body();
    return true;
  });
  const std::string token = web_token();
  if (type != "auth" || c.nonce.empty() || !same_secret(proof, web_auth_proof(token, c.nonce))) {
    MLOG("web: a browser failed the challenge");
    send_frame(c, 8, "\x03\xf0");  // 1008: policy
    c.closing = true;
    flush(c);
    return;
  }
  c.nonce.clear();
  c.token = token;
  c.api = std::make_unique<api::Client>(ws_);
  MLOG("web: a browser connected%s", lan_host(c.host) ? " from the local network" : "");
}

void WebServer::send_text(Conn& c, std::string_view text) { send_frame(c, 1, text); }

void WebServer::send_frame(Conn& c, int opcode, std::string_view payload) {
  if (c.out.size() > kMaxQueued) {
    // A browser this far behind is not reading; better to drop it than to
    // hold the daemon's memory for it.
    c.dead = true;
    return;
  }
  c.out += char(0x80 | opcode);
  const size_t n = payload.size();
  if (n < 126) {
    c.out += char(n);
  } else if (n < 65536) {
    c.out += char(126);
    c.out += char(n >> 8);
    c.out += char(n & 0xFF);
  } else {
    c.out += char(127);
    for (int i = 7; i >= 0; i--) c.out += char((uint64_t(n) >> (i * 8)) & 0xFF);
  }
  c.out += payload;
}

void WebServer::flush(Conn& c) {
  while (!c.out.empty()) {
    const ssize_t n = write(c.fd, c.out.data(), c.out.size());
    if (n > 0) { c.out.erase(0, size_t(n)); continue; }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
    c.dead = true;
    return;
  }
  if (c.closing) c.dead = true;
}

}  // namespace mico
