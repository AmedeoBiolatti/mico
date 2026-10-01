#include "term/encoder.h"

#include <string>

#include "term/links.h"
#include "base/text.h"

namespace mico {
namespace {

// snprintf costs a few hundred nanoseconds per call and the present path makes
// several per changed cell run. These are all small non-negative integers.
void append_uint(std::string& o, unsigned v) {
  char buf[12];
  char* p = buf + sizeof buf;
  do { *--p = char('0' + v % 10); v /= 10; } while (v);
  o.append(p, size_t(buf + sizeof buf - p));
}

void append_color(std::string& o, Color c, bool fg) {
  if (c == kDefaultColor) {
    o += fg ? "39" : "49";
    return;
  }
  o += fg ? "38;2;" : "48;2;";
  append_uint(o, unsigned((c >> 16) & 0xFF));
  o += ';';
  append_uint(o, unsigned((c >> 8) & 0xFF));
  o += ';';
  append_uint(o, unsigned(c & 0xFF));
}

void append_style(std::string& o, const Style& st);

// Emits only what changed since `prev`. A style change used to cost a full
// reset plus both truecolor channels — 34 bytes — and style changes were over
// half of every frame. Most transitions change one colour.
void append_style_delta(std::string& o, const Style& prev, const Style& cur) {
  // Turning an attribute *off* has no incremental form, so fall back to a
  // reset that restates everything.
  constexpr uint16_t kVisual = attr::kBold | attr::kDim | attr::kItalic | attr::kUnderline | attr::kReverse | attr::kStrike;
  if (prev.a & ~cur.a & kVisual) {
    append_style(o, cur);
    return;
  }

  const size_t mark = o.size();
  o += "\x1b[";
  bool first = true;
  auto sep = [&] {
    if (!first) o += ';';
    first = false;
  };

  const uint16_t on = uint16_t(cur.a & ~prev.a);
  if (on & attr::kBold) { sep(); o += '1'; }
  if (on & attr::kDim) { sep(); o += '2'; }
  if (on & attr::kItalic) { sep(); o += '3'; }
  if (on & attr::kUnderline) { sep(); o += '4'; }
  if (on & attr::kReverse) { sep(); o += '7'; }
  if (on & attr::kStrike) { sep(); o += '9'; }
  if (cur.fg != prev.fg) { sep(); append_color(o, cur.fg, true); }
  if (cur.bg != prev.bg) { sep(); append_color(o, cur.bg, false); }

  if (first) {
    o.resize(mark);  // identical styles: say nothing
    return;
  }
  o += 'm';
}

void append_style(std::string& o, const Style& st) {
  o += "\x1b[0";
  if (st.a & attr::kBold) o += ";1";
  if (st.a & attr::kDim) o += ";2";
  if (st.a & attr::kItalic) o += ";3";
  if (st.a & attr::kUnderline) o += ";4";
  if (st.a & attr::kReverse) o += ";7";
  if (st.a & attr::kStrike) o += ";9";
  o += ';';
  append_color(o, st.fg, true);
  o += ';';
  append_color(o, st.bg, false);
  o += 'm';
}

// kitty's row/column diacritics: the n-th combining mark names row or
// column n of the image a placeholder cell shows.
constexpr char32_t kDiacritics[] = {
    0x0305, 0x030D, 0x030E, 0x0310, 0x0312, 0x033D, 0x033E, 0x033F, 0x0346, 0x034A,
    0x034B, 0x034C, 0x0350, 0x0351, 0x0352, 0x0357, 0x035B, 0x0363, 0x0364, 0x0365,
    0x0366, 0x0367, 0x0368, 0x0369, 0x036A, 0x036B, 0x036C, 0x036D, 0x036E, 0x036F,
    0x0483, 0x0484, 0x0485, 0x0486, 0x0487, 0x0592, 0x0593, 0x0594, 0x0595, 0x0597,
    0x0598, 0x0599, 0x059C, 0x059D, 0x059E, 0x059F, 0x05A0, 0x05A1, 0x05A8, 0x05A9,
    0x05AB, 0x05AC, 0x05AF, 0x05C4, 0x0610, 0x0611, 0x0612, 0x0613, 0x0614, 0x0615,
    0x0616, 0x0617, 0x0657, 0x0658, 0x0659, 0x065A, 0x065B, 0x065D, 0x065E, 0x06D6,
    0x06D7, 0x06D8, 0x06D9, 0x06DA, 0x06DB, 0x06DC, 0x06DF, 0x06E0, 0x06E1, 0x06E2,
    0x06E4, 0x06E7, 0x06E8, 0x06EB, 0x06EC, 0x0730, 0x0732, 0x0733, 0x0735, 0x0736,
    0x073A, 0x073D, 0x073F, 0x0740, 0x0741, 0x0743, 0x0745, 0x0747, 0x0749, 0x074A,
    0x07EB, 0x07EC, 0x07ED, 0x07EE, 0x07EF, 0x07F0, 0x07F1, 0x07F3, 0x0816, 0x0817,
    0x0818, 0x0819, 0x081B, 0x081C, 0x081D, 0x081E, 0x081F, 0x0820, 0x0821, 0x0822,
    0x0823, 0x0825, 0x0826, 0x0827, 0x0829, 0x082A, 0x082B, 0x082C, 0x082D, 0x0951,
    0x0953, 0x0954, 0x0F82, 0x0F83, 0x0F86, 0x0F87, 0x135D, 0x135E, 0x135F, 0x17DD,
    0x193A, 0x1A17, 0x1A75, 0x1A76, 0x1A77, 0x1A78, 0x1A79, 0x1A7A, 0x1A7B, 0x1A7C,
    0x1B6B, 0x1B6D, 0x1B6E, 0x1B6F, 0x1B70, 0x1B71, 0x1B72, 0x1B73, 0x1CD0, 0x1CD1,
    0x1CD2, 0x1CDA, 0x1CDB, 0x1CE0, 0x1DC0, 0x1DC1, 0x1DC3, 0x1DC4, 0x1DC5, 0x1DC6,
    0x1DC7, 0x1DC8, 0x1DC9, 0x1DCB, 0x1DCC, 0x1DD1, 0x1DD2, 0x1DD3, 0x1DD4, 0x1DD5,
    0x1DD6, 0x1DD7, 0x1DD8, 0x1DD9, 0x1DDA, 0x1DDB, 0x1DDC, 0x1DDD, 0x1DDE, 0x1DDF,
    0x1DE0, 0x1DE1, 0x1DE2, 0x1DE3, 0x1DE4, 0x1DE5, 0x1DE6, 0x1DFE, 0x20D0, 0x20D1,
    0x20D4, 0x20D5, 0x20D6, 0x20D7, 0x20DB, 0x20DC, 0x20E1, 0x20E7, 0x20E9, 0x20F0,
    0x2CEF, 0x2CF0, 0x2CF1, 0x2DE0, 0x2DE1, 0x2DE2, 0x2DE3, 0x2DE4, 0x2DE5, 0x2DE6,
    0x2DE7, 0x2DE8, 0x2DE9, 0x2DEA, 0x2DEB, 0x2DEC, 0x2DED, 0x2DEE, 0x2DEF, 0x2DF0,
    0x2DF1, 0x2DF2, 0x2DF3, 0x2DF4, 0x2DF5, 0x2DF6, 0x2DF7, 0x2DF8, 0x2DF9, 0x2DFA,
    0x2DFB, 0x2DFC, 0x2DFD, 0x2DFE, 0x2DFF, 0xA66F, 0xA67C, 0xA67D, 0xA6F0, 0xA6F1,
    0xA8E0, 0xA8E1, 0xA8E2, 0xA8E3, 0xA8E4, 0xA8E5, 0xA8E6, 0xA8E7, 0xA8E8, 0xA8E9,
    0xA8EA, 0xA8EB, 0xA8EC, 0xA8ED, 0xA8EE, 0xA8EF, 0xA8F0, 0xA8F1, 0xAAB0, 0xAAB2,
    0xAAB3, 0xAAB7, 0xAAB8, 0xAABE, 0xAABF, 0xAAC1, 0xFE20, 0xFE21, 0xFE22, 0xFE23,
    0xFE24, 0xFE25, 0xFE26, 0x10A0F, 0x10A38, 0x1D185, 0x1D186, 0x1D187, 0x1D188, 0x1D189,
    0x1D1AA, 0x1D1AB, 0x1D1AC, 0x1D1AD, 0x1D242, 0x1D243, 0x1D244,
};
constexpr int kDiacriticCount = int(sizeof kDiacritics / sizeof kDiacritics[0]);

// One cell's glyph. An image cell is U+10EEEE with its row and column as
// diacritics; its colour (the image id) was set with the style.
void append_glyph(std::string& o, const Cell& c, bool images) {
  if (c.st.a & attr::kImage) {
    if (!images) { o += ' '; return; }
    const int row = int(c.cp >> 16), col = int(c.cp & 0xFFFF);
    if (row >= kDiacriticCount || col >= kDiacriticCount) { o += ' '; return; }
    text::encode(0x10EEEE, o);
    text::encode(kDiacritics[row], o);
    text::encode(kDiacritics[col], o);
    return;
  }
  text::encode(c.cp == 0 ? U' ' : c.cp, o);
}

// OSC 8: the cells written from here on are (or, with id 0, are no longer)
// part of a hyperlink. The id groups a link's cells across rows, so the
// terminal lights all of a wrapped link when one part is hovered.
void append_link(std::string& o, uint16_t id) {
  const std::string_view url = links::url(id);
  if (!id || url.empty()) {
    o += "\x1b]8;;\x1b\\";
    return;
  }
  o += "\x1b]8;id=";
  append_uint(o, id);
  o += ';';
  o += url;
  o += "\x1b\\";
}

void append_cup(std::string& o, int x, int y) {
  o += "\x1b[";
  append_uint(o, unsigned(y + 1));
  o += ';';
  append_uint(o, unsigned(x + 1));
  o += 'H';
}

}  // namespace

bool encode_frame(const Surface& back, Surface& front, std::string& out, bool full,
                  ImageMode mode) {
  const bool images = mode == ImageMode::Kitty;
  if (full || front.width() != back.width() || front.height() != back.height()) {
    front.resize(back.width(), back.height());
    // A sentinel that cannot compare equal to a real cell, so the first pass
    // after an attach or a resize repaints everything.
    for (int y = 0; y < front.height(); y++)
      for (int x = 0; x < front.width(); x++) front.at(x, y).cp = 0xFFFFFFFFu;
  }

  const size_t mark = out.size();
  out += "\x1b[?2026h";  // synchronized update: no tearing mid-frame
  const size_t after_open = out.size();

  Style cur{};
  bool style_set = false;
  int cx = -1, cy = -1;
  uint16_t link = 0;  // the hyperlink open in the terminal

  for (int y = 0; y < back.height(); y++) {
    for (int x = 0; x < back.width(); x++) {
      const Cell& c = back.at(x, y);
      if (c.width == 0) continue;  // trailing half of a wide glyph
      if (front.at(x, y) == c) continue;
      if (mode == ImageMode::Sixel && (c.st.a & attr::kImage)) continue;  // the sixel pass's

      // A short hop is cheaper to walk than to jump: a cursor address costs
      // about seven bytes, so re-emitting a few unchanged cells wins — but only
      // while they carry the style we are already in.
      if (cy == y && x > cx && x - cx <= 4 && style_set) {
        bool walkable = true;
        for (int k = cx; k < x; k++) {
          const Cell& g = back.at(k, y);
          if (g.width != 1 || !(g.st == cur) || g.link != link) { walkable = false; break; }
        }
        if (walkable) {
          for (int k = cx; k < x; k++) append_glyph(out, back.at(k, y), images);
          cx = x;
        }
      }

      if (cx != x || cy != y) { append_cup(out, x, y); cx = x; cy = y; }
      if (!style_set) {
        append_style(out, c.st);
        style_set = true;
      } else {
        append_style_delta(out, cur, c.st);
      }
      cur = c.st;
      if (c.link != link) {
        append_link(out, c.link);
        link = c.link;
      }
      append_glyph(out, c, images);
      cx += c.width;

      front.at(x, y) = c;
      if (c.width == 2 && x + 1 < back.width()) front.at(x + 1, y) = back.at(x + 1, y);
    }
  }

  if (out.size() == after_open) {
    out.resize(mark);  // nothing changed; do not even send the sync wrapper
    return false;
  }
  if (link) append_link(out, 0);  // no link left open for whatever comes next
  out += "\x1b[?2026l";
  return true;
}

void encode_cell_at(const Cell& c, int x, int y, std::string& out) {
  if (c.width == 0) return;
  append_cup(out, x, y);
  append_style(out, c.st);
  if (c.link) append_link(out, c.link);
  append_glyph(out, c, false);
  if (c.link) append_link(out, 0);
}

std::string mouse_mode_seq(bool on, bool any_event) {
  if (!on) return "\x1b[?1002l\x1b[?1003l\x1b[?1006l";
  // 1002 reports motion only while a button is held; 1003 reports every move.
  // The latter floods events for the whole terminal, so it is asked for only
  // while something — a popup menu — actually needs hover with no button down.
  return any_event ? "\x1b[?1003h\x1b[?1006h" : "\x1b[?1002h\x1b[?1006h";
}

std::string clipboard_seq(std::string_view text) {
  static constexpr char kB64[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string b64;
  b64.reserve((text.size() + 2) / 3 * 4);
  for (size_t i = 0; i < text.size(); i += 3) {
    const unsigned n = (unsigned(uint8_t(text[i])) << 16) |
                       (i + 1 < text.size() ? unsigned(uint8_t(text[i + 1])) << 8 : 0) |
                       (i + 2 < text.size() ? unsigned(uint8_t(text[i + 2])) : 0);
    b64.push_back(kB64[(n >> 18) & 63]);
    b64.push_back(kB64[(n >> 12) & 63]);
    b64.push_back(i + 1 < text.size() ? kB64[(n >> 6) & 63] : '=');
    b64.push_back(i + 2 < text.size() ? kB64[n & 63] : '=');
  }
  return "\x1b]52;c;" + b64 + "\x07";
}

}  // namespace mico
