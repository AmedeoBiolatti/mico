#include "term/kitty.h"

#include <cstdio>

#include "math/deflate.h"
#include "math/math.h"
#include "math/picture.h"

namespace mico::math {
namespace {

void base64(const uint8_t* p, size_t n, std::string& out) {
  static constexpr char kB64[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  out.reserve(out.size() + (n + 2) / 3 * 4);
  for (size_t i = 0; i < n; i += 3) {
    const unsigned v = (unsigned(p[i]) << 16) | (i + 1 < n ? unsigned(p[i + 1]) << 8 : 0) |
                       (i + 2 < n ? unsigned(p[i + 2]) : 0);
    out.push_back(kB64[(v >> 18) & 63]);
    out.push_back(kB64[(v >> 12) & 63]);
    out.push_back(i + 1 < n ? kB64[(v >> 6) & 63] : '=');
    out.push_back(i + 2 < n ? kB64[v & 63] : '=');
  }
}

// One graphics command. Inside tmux it is wrapped for passthrough, every ESC
// in it doubled, or tmux would swallow it.
void command(std::string_view apc, bool tmux, std::string& out) {
  if (!tmux) {
    out += apc;
    return;
  }
  out += "\x1bPtmux;";
  for (char ch : apc) {
    if (ch == '\x1b') out += '\x1b';
    out += ch;
  }
  out += "\x1b\\";
}

// An image as kitty receives it: RGBA (an equation tinted in `fg`),
// compressed, base64. Made once per image, however many terminals need it.
const std::string& payload(const Image& im, Color fg) {
  if (!im.wire.empty()) return im.wire;
  if (im.encoded && !pixels(im)) return im.wire;  // a picture that no longer decodes: nothing to send
  std::vector<uint8_t> tinted;
  if (im.rgba.empty()) {
    tinted.resize(size_t(im.w) * size_t(im.h) * 4);
    const uint8_t r = uint8_t(fg >> 16), g = uint8_t(fg >> 8), b = uint8_t(fg);
    for (size_t i = 0; i < im.alpha.size(); i++) {
      tinted[i * 4] = r;
      tinted[i * 4 + 1] = g;
      tinted[i * 4 + 2] = b;
      tinted[i * 4 + 3] = im.alpha[i];
    }
  }
  const std::vector<uint8_t>& rgba = im.rgba.empty() ? tinted : im.rgba;
  const std::string z = zlib_compress(rgba.data(), rgba.size());
  base64(reinterpret_cast<const uint8_t*>(z.data()), z.size(), im.wire);
  return im.wire;
}

// Transmits one image, then places it virtually: nothing shows until
// placeholder cells name it. q=2 keeps the terminal from answering, since its
// replies would arrive as keyboard input.
void transmit(const Image& im, Color fg, bool tmux, std::string& out) {
  const std::string& data = payload(im, fg);

  constexpr size_t kChunk = 4096;
  char head[128];
  std::string apc;
  for (size_t off = 0; off < data.size() || off == 0; off += kChunk) {
    const bool more = off + kChunk < data.size();
    if (off == 0)
      snprintf(head, sizeof head, "\x1b_Ga=t,t=d,f=32,o=z,s=%d,v=%d,i=%u,q=2,m=%d;", im.w, im.h,
               unsigned(im.id), more ? 1 : 0);
    else
      snprintf(head, sizeof head, "\x1b_Gm=%d,q=2;", more ? 1 : 0);
    apc = head;
    apc.append(data, off, std::min(kChunk, data.size() - off));
    apc += "\x1b\\";
    command(apc, tmux, out);
    if (!more) break;
  }
  snprintf(head, sizeof head, "\x1b_Ga=p,U=1,i=%u,c=%d,r=%d,C=1,q=2\x1b\\", unsigned(im.id),
           im.cols, im.rows);
  command(head, tmux, out);
}

}  // namespace

void send_images(const Surface& s, std::unordered_set<uint32_t>& sent, std::string& out,
                 bool tmux) {
  uint32_t last = 0;
  for (int y = 0; y < s.height(); y++) {
    for (int x = 0; x < s.width(); x++) {
      const Cell& c = s.at(x, y);
      if (!(c.st.a & attr::kImage)) continue;
      const uint32_t id = uint32_t(c.st.fg) & 0xFFFFFF;
      if (id == last) continue;  // a row of one image: checked once
      last = id;
      if (sent.count(id)) continue;
      const Image* im = find(id);
      if (!im) continue;
      transmit(*im, config().fg, tmux, out);
      sent.insert(id);
    }
  }
}

void free_images(const std::vector<uint32_t>& ids, std::unordered_set<uint32_t>& sent,
                 std::string& out, bool tmux) {
  char buf[64];
  for (uint32_t id : ids) {
    if (!sent.erase(id)) continue;
    snprintf(buf, sizeof buf, "\x1b_Ga=d,d=I,i=%u,q=2\x1b\\", unsigned(id));
    command(buf, tmux, out);
  }
}

}  // namespace mico::math
