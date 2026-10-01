#include "net/proto.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>

namespace mico::proto {
namespace {

void put_u32(std::string& out, uint32_t v) {
  char b[4] = {char(v & 0xFF), char((v >> 8) & 0xFF), char((v >> 16) & 0xFF),
               char((v >> 24) & 0xFF)};
  out.append(b, 4);
}

uint32_t get_u32(const char* p) {
  return uint32_t(uint8_t(p[0])) | uint32_t(uint8_t(p[1])) << 8 |
         uint32_t(uint8_t(p[2])) << 16 | uint32_t(uint8_t(p[3])) << 24;
}

}  // namespace

void encode(Type t, std::string_view payload, std::string& out) {
  out.push_back(char(t));
  put_u32(out, uint32_t(payload.size()));
  out.append(payload);
}

void encode_size(Type t, int w, int h, std::string& out) {
  char b[4] = {char(w & 0xFF), char((w >> 8) & 0xFF), char(h & 0xFF), char((h >> 8) & 0xFF)};
  encode(t, std::string_view(b, 4), out);
}

void encode_size(Type t, int w, int h, const GfxCaps& caps, std::string& out) {
  const char b[9] = {char(w & 0xFF), char((w >> 8) & 0xFF), char(h & 0xFF), char((h >> 8) & 0xFF),
                     char((caps.kitty ? 1 : 0) | (caps.sixel ? 2 : 0) | (caps.tmux ? 4 : 0)),
                     char(caps.cell_w & 0xFF), char((caps.cell_w >> 8) & 0xFF),
                     char(caps.cell_h & 0xFF), char((caps.cell_h >> 8) & 0xFF)};
  encode(t, std::string_view(b, 9), out);
}

bool decode(std::string& buf, Type* t, std::string* payload) {
  if (buf.size() < 5) return false;
  uint32_t len = get_u32(buf.data() + 1);
  if (len > kMaxPayload) { buf.clear(); return false; }
  if (buf.size() < size_t(5) + len) return false;
  *t = Type(uint8_t(buf[0]));
  payload->assign(buf, 5, len);
  buf.erase(0, size_t(5) + len);
  return true;
}

bool decode_caps(std::string_view p, GfxCaps* caps) {
  if (p.size() < 9) return false;
  caps->kitty = (uint8_t(p[4]) & 1) != 0;
  caps->sixel = (uint8_t(p[4]) & 2) != 0;
  caps->tmux = (uint8_t(p[4]) & 4) != 0;
  caps->cell_w = int(uint8_t(p[5])) | int(uint8_t(p[6])) << 8;
  caps->cell_h = int(uint8_t(p[7])) | int(uint8_t(p[8])) << 8;
  return true;
}

bool decode_size(std::string_view p, int* w, int* h) {
  if (p.size() < 4) return false;
  *w = int(uint8_t(p[0])) | int(uint8_t(p[1])) << 8;
  *h = int(uint8_t(p[2])) | int(uint8_t(p[3])) << 8;
  return true;
}

namespace {
constexpr const char* kSocketName = "/default.sock";
bool fits(const std::string& path) { return path.size() < sizeof(sockaddr_un::sun_path); }
}  // namespace

std::string socket_dir() {
  if (const char* rt = getenv("XDG_RUNTIME_DIR"); rt && *rt) {
    std::string dir = std::string(rt) + "/mico";
    if (fits(dir + kSocketName)) return dir;
  }
  return "/tmp/mico-" + std::to_string(getuid());
}

std::string socket_path() { return socket_dir() + kSocketName; }

bool socket_addr(const std::string& path, sockaddr_un* out) {
  *out = sockaddr_un{};
  out->sun_family = AF_UNIX;
  if (!fits(path)) return false;
  memcpy(out->sun_path, path.c_str(), path.size() + 1);
  return true;
}

bool private_dir(const std::string& dir, bool create) {
  if (create) mkdir(dir.c_str(), 0700);
  struct stat st{};
  // lstat: a symlink planted at the path must not redirect the socket.
  if (lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;
  return st.st_uid == getuid() && (st.st_mode & 077) == 0;
}

}  // namespace mico::proto
