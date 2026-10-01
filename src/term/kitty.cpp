#include "term/kitty.h"

#include <cstdio>

#include "math/deflate.h"
#include "math/math.h"
#include "math/picture.h"

namespace mico::math {
namespace {

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

// The most a terminal is left holding: kitty keeps every image decoded, and
// past its own quota (320 MB by default) drops the ones it showed longest
// ago without saying so, leaving their cells blank.
constexpr uint64_t kHeldBudget = 192u << 20;

// An image as kitty receives it, made once per image however many terminals
// need it: a PNG as it is, or RGBA (an equation tinted in `fg`), compressed,
// base64. Null while a picture's pixels are still being prepared.
const std::string* payload(const Image& im, Color fg) {
  if (!im.wire.empty()) return &im.wire;
  if (im.encoded) return kitty_wire(im) ? &im.wire : nullptr;
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
  base64_encode(reinterpret_cast<const uint8_t*>(z.data()), z.size(), im.wire);
  return &im.wire;
}

// What the terminal holds for an image once sent: its pixels, decoded.
uint64_t held_bytes(const Image& im) {
  return im.wire_png ? uint64_t(im.src_w) * uint64_t(im.src_h) * 4 : uint64_t(im.w) * uint64_t(im.h) * 4;
}

// Transmits one image, then places it virtually: nothing shows until
// placeholder cells name it. q=2 keeps the terminal from answering, since its
// replies would arrive as keyboard input. A PNG is placed in the same cells
// as pixels would be; kitty fits it to them.
void transmit(const Image& im, const std::string& data, bool tmux, std::string& out) {
  constexpr size_t kChunk = 4096;
  char head[128];
  std::string apc;
  for (size_t off = 0; off < data.size() || off == 0; off += kChunk) {
    const bool more = off + kChunk < data.size();
    if (off != 0)
      snprintf(head, sizeof head, "\x1b_Gm=%d,q=2;", more ? 1 : 0);
    else if (im.wire_png)
      snprintf(head, sizeof head, "\x1b_Ga=t,t=d,f=100,i=%u,q=2,m=%d;", unsigned(im.id), more ? 1 : 0);
    else
      snprintf(head, sizeof head, "\x1b_Ga=t,t=d,f=32,o=z,s=%d,v=%d,i=%u,q=2,m=%d;", im.w, im.h,
               unsigned(im.id), more ? 1 : 0);
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

void forget(uint32_t id, KittyHeld& held, bool tmux, std::string& out) {
  const auto it = held.images.find(id);
  if (it == held.images.end()) return;
  held.bytes -= std::min(held.bytes, it->second.bytes);
  held.images.erase(it);
  char buf[64];
  snprintf(buf, sizeof buf, "\x1b_Ga=d,d=I,i=%u,q=2\x1b\\", unsigned(id));
  command(buf, tmux, out);
}

}  // namespace

void send_images(const Surface& s, KittyHeld& held, std::string& out, bool tmux) {
  const uint64_t frame = ++held.frame;
  uint32_t last = 0;
  bool sent = false;
  for (int y = 0; y < s.height(); y++) {
    for (int x = 0; x < s.width(); x++) {
      const Cell& c = s.at(x, y);
      if (!(c.st.a & attr::kImage)) continue;
      const uint32_t id = uint32_t(c.st.fg) & 0xFFFFFF;
      if (id == last) continue;  // a row of one image: checked once
      last = id;
      if (const auto it = held.images.find(id); it != held.images.end()) {
        it->second.frame = frame;
        continue;
      }
      const Image* im = find(id);
      if (!im) continue;
      const std::string* data = payload(*im, config().fg);
      if (!data) continue;  // not ready: a later frame sends it
      transmit(*im, *data, tmux, out);
      const uint64_t bytes = held_bytes(*im);
      held.images[id] = KittyHeld::Entry{bytes, frame};
      held.bytes += bytes;
      sent = true;
    }
  }
  // Over budget: the images drawn longest ago go, never one on screen now.
  while (sent && held.bytes > kHeldBudget) {
    auto oldest = held.images.end();
    for (auto it = held.images.begin(); it != held.images.end(); ++it)
      if (it->second.frame != frame && (oldest == held.images.end() || it->second.frame < oldest->second.frame))
        oldest = it;
    if (oldest == held.images.end()) break;
    forget(oldest->first, held, tmux, out);
  }
}

void free_images(const std::vector<uint32_t>& ids, KittyHeld& held, std::string& out, bool tmux) {
  for (uint32_t id : ids) forget(id, held, tmux, out);
}

}  // namespace mico::math
