#include "vt/vt.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "base/text.h"

namespace mico {
namespace {

constexpr size_t kMaxScrollback = 10000;

// xterm's 256-colour cube, so SGR 38;5;n renders the same colour the agent meant.
Color xterm256(int n) {
  static constexpr Color kBase[16] = {
      0x000000, 0xCD0000, 0x00CD00, 0xCDCD00, 0x0000EE, 0xCD00CD, 0x00CDCD, 0xE5E5E5,
      0x7F7F7F, 0xFF0000, 0x00FF00, 0xFFFF00, 0x5C5CFF, 0xFF00FF, 0x00FFFF, 0xFFFFFF};
  if (n < 0) return kDefaultColor;
  if (n < 16) return kBase[n];
  if (n < 232) {
    int i = n - 16;
    static constexpr int kSteps[6] = {0, 95, 135, 175, 215, 255};
    return (kSteps[i / 36] << 16) | (kSteps[(i / 6) % 6] << 8) | kSteps[i % 6];
  }
  int g = 8 + (n - 232) * 10;
  return (g << 16) | (g << 8) | g;
}

}  // namespace

void Vt::resize(int w, int h) {
  w = std::max(1, w);
  h = std::max(1, h);
  if (w == w_ && h == h_ && !screen_.empty()) return;
  gen_ = next_generation();
  w_ = w;
  h_ = h;
  screen_.resize(size_t(h_));
  alt_buf_.resize(size_t(h_));
  for (auto& r : screen_) r.resize(size_t(w_), Cell{U' ', Style{}, 1});
  for (auto& r : alt_buf_) r.resize(size_t(w_), Cell{U' ', Style{}, 1});
  top_ = 0;
  bot_ = h_ - 1;
  cx_ = std::min(cx_, w_ - 1);
  cy_ = std::min(cy_, h_ - 1);
}

const VtRow& Vt::row(int i) const {
  static const VtRow kEmpty;
  int sb = int(scrollback_.size());
  if (i < 0) return kEmpty;
  if (i < sb) return scrollback_[size_t(i)];
  int y = i - sb;
  const auto& buf = alt_ ? alt_buf_ : screen_;
  if (y >= int(buf.size())) return kEmpty;
  return buf[size_t(y)];
}

VtRow& Vt::line(int y) {
  auto& buf = alt_ ? alt_buf_ : screen_;
  y = std::clamp(y, 0, int(buf.size()) - 1);
  return buf[size_t(y)];
}

void Vt::set_cursor(int x, int y) {
  cx_ = std::clamp(x, 0, w_ - 1);
  cy_ = std::clamp(y, 0, h_ - 1);
  wrap_pending_ = false;
}

void Vt::scroll_up(int n) {
  auto& buf = alt_ ? alt_buf_ : screen_;
  // Only lines leaving the top of a full-height region are real history;
  // a scrolling region is the app repainting, not the conversation moving.
  const bool archive = !alt_ && top_ == 0 && bot_ == h_ - 1;
  for (int i = 0; i < n; i++) {
    // The row about to leave the top: either it carries its allocation down to
    // become the new blank bottom row, or it is archived and the buffer we
    // recycle comes from the scrollback line that just aged out. Steady-state
    // scrolling then costs no allocation at all.
    VtRow fresh;
    if (archive) {
      scrollback_.push_back(std::move(buf[size_t(top_)]));
      if (scrollback_.size() > kMaxScrollback) {
        fresh = std::move(scrollback_.front());
        scrollback_.pop_front();
      }
    } else {
      fresh = std::move(buf[size_t(top_)]);
    }
    for (int y = top_; y < bot_; y++) buf[size_t(y)] = std::move(buf[size_t(y) + 1]);
    blank(fresh);
    buf[size_t(bot_)] = std::move(fresh);
  }
}

void Vt::scroll_down(int n) {
  auto& buf = alt_ ? alt_buf_ : screen_;
  for (int i = 0; i < n; i++) {
    VtRow fresh = std::move(buf[size_t(bot_)]);  // reuse the row falling off the bottom
    for (int y = bot_; y > top_; y--) buf[size_t(y)] = std::move(buf[size_t(y) - 1]);
    blank(fresh);
    buf[size_t(top_)] = std::move(fresh);
  }
}

void Vt::index() {
  if (cy_ == bot_) scroll_up(1);
  else if (cy_ < h_ - 1) cy_++;
  wrap_pending_ = false;
}

void Vt::reverse_index() {
  if (cy_ == top_) scroll_down(1);
  else if (cy_ > 0) cy_--;
  wrap_pending_ = false;
}

void Vt::newline() {
  index();
}

void Vt::put(char32_t cp, int w) {
  if (w == 0) return;
  if (wrap_pending_ && autowrap_) {
    carriage_return();
    index();
  }
  if (cx_ + w > w_) {
    if (!autowrap_) { cx_ = w_ - w; }
    else { carriage_return(); index(); }
  }
  VtRow& r = line(cy_);
  if (size_t(cx_) >= r.size()) return;

  // Overwriting half of a wide glyph must clear its partner.
  if (r[size_t(cx_)].width == 0 && cx_ > 0) r[size_t(cx_ - 1)] = Cell{U' ', cur_, 1};
  if (r[size_t(cx_)].width == 2 && cx_ + 1 < w_) r[size_t(cx_ + 1)] = Cell{U' ', cur_, 1};

  r[size_t(cx_)] = Cell{cp, cur_, uint8_t(w)};
  if (w == 2 && cx_ + 1 < w_) r[size_t(cx_ + 1)] = Cell{U' ', cur_, 0};

  last_cp_ = cp;
  last_x_ = cx_;
  last_y_ = cy_;
  cx_ += w;
  if (cx_ >= w_) { cx_ = w_ - 1; wrap_pending_ = true; }
  after_x_ = cx_;
  after_wrap_ = wrap_pending_;
}

bool Vt::extend(char32_t cp) {
  const bool zero = text::cp_width(cp) == 0;
  const bool flag = cp >= 0x1F1E6 && cp <= 0x1F1FF;
  const bool tone = cp >= 0x1F3FB && cp <= 0x1F3FF;
  // Anything else may still follow a ZWJ; the glyph rules below decide.
  if (!zero && !flag && !tone && !(last_cp_ >= text::kGlyphBase && text::glyph_text(last_cp_).ends_with("\u200D")))
    return false;
  if (last_y_ != cy_ || after_x_ != cx_ || after_wrap_ != wrap_pending_ || last_x_ < 0) return zero;
  VtRow& r = line(cy_);
  if (size_t(last_x_) >= r.size() || r[size_t(last_x_)].cp != last_cp_) return zero;
  Cell& c = r[size_t(last_x_)];

  std::string seq;
  text::encode(c.cp, seq);
  text::encode(cp, seq);
  size_t i = 0;
  int w;
  const char32_t g = text::next_glyph(seq, i, &w);
  if (i != seq.size()) return zero;  // not one glyph: a mark on ASCII is dropped as before
  c.cp = last_cp_ = g;
  // The emoji selector or a flag's second letter widens it, when there is room.
  if (w == 2 && c.width == 1 && last_x_ + 1 < w_) {
    Cell& next = r[size_t(last_x_ + 1)];
    if (next.width == 2 && last_x_ + 2 < w_) r[size_t(last_x_ + 2)] = Cell{U' ', cur_, 1};
    c.width = 2;
    next = Cell{U' ', c.st, 0};
    if (!wrap_pending_) {
      cx_++;
      if (cx_ >= w_) { cx_ = w_ - 1; wrap_pending_ = true; }
    }
    after_x_ = cx_;
    after_wrap_ = wrap_pending_;
  }
  return true;
}

// Writes a run of single-width ASCII without going through put() per byte.
// Agent output is overwhelmingly plain text, and the per-character path spends
// most of its time re-deriving state that cannot have changed mid-run.
void Vt::put_ascii_run(std::string_view s) {
  size_t k = 0;
  while (k < s.size()) {
    if (wrap_pending_ && autowrap_) {
      carriage_return();
      index();
    }
    if (cx_ >= w_) {
      if (!autowrap_) return;
      carriage_return();
      index();
    }

    VtRow& r = line(cy_);
    const size_t room = size_t(w_ - cx_);
    const size_t n = std::min(room, s.size() - k);

    // Landing on the tail of a double-width glyph must blank its head.
    if (r[size_t(cx_)].width == 0 && cx_ > 0) r[size_t(cx_ - 1)] = Cell{U' ', cur_, 1};

    for (size_t t = 0; t < n; t++) {
      Cell& dst = r[size_t(cx_) + t];
      // Only the rare wide-glyph case needs the slow fixup.
      if (dst.width == 2 && size_t(cx_) + t + 1 < r.size())
        r[size_t(cx_) + t + 1] = Cell{U' ', cur_, 1};
      dst = Cell{char32_t(uint8_t(s[k + t])), cur_, 1};
    }

    cx_ += int(n);
    k += n;
    if (cx_ >= w_) {
      cx_ = w_ - 1;
      wrap_pending_ = true;
    }
  }
}

void Vt::erase_in_line(int mode) {
  VtRow& r = line(cy_);
  int from = mode == 0 ? cx_ : 0;
  int to = mode == 1 ? cx_ + 1 : w_;
  if (mode == 2) { from = 0; to = w_; }
  for (int x = from; x < to && x < int(r.size()); x++) r[size_t(x)] = Cell{U' ', cur_, 1};
}

void Vt::erase_in_display(int mode) {
  if (mode == 3) { scrollback_.clear(); return; }
  int from = mode == 0 ? cy_ + 1 : 0;
  int to = mode == 1 ? cy_ : h_;
  if (mode == 2) { from = 0; to = h_; }
  for (int y = from; y < to; y++) blank(line(y));
  if (mode == 0) erase_in_line(0);
  else if (mode == 1) erase_in_line(1);
}

void Vt::insert_lines(int n) {
  if (cy_ < top_ || cy_ > bot_) return;
  auto& buf = alt_ ? alt_buf_ : screen_;
  for (int i = 0; i < n; i++) {
    buf.erase(buf.begin() + bot_);
    VtRow fresh;
    blank(fresh);
    buf.insert(buf.begin() + cy_, std::move(fresh));
  }
}

void Vt::delete_lines(int n) {
  if (cy_ < top_ || cy_ > bot_) return;
  auto& buf = alt_ ? alt_buf_ : screen_;
  for (int i = 0; i < n; i++) {
    buf.erase(buf.begin() + cy_);
    VtRow fresh;
    blank(fresh);
    buf.insert(buf.begin() + bot_, std::move(fresh));
  }
}

void Vt::insert_chars(int n) {
  VtRow& r = line(cy_);
  for (int i = 0; i < n; i++) {
    r.pop_back();
    r.insert(r.begin() + cx_, Cell{U' ', cur_, 1});
  }
}

void Vt::delete_chars(int n) {
  VtRow& r = line(cy_);
  for (int i = 0; i < n; i++) {
    if (size_t(cx_) < r.size()) r.erase(r.begin() + cx_);
    r.push_back(Cell{U' ', cur_, 1});
  }
}

void Vt::erase_chars(int n) {
  VtRow& r = line(cy_);
  for (int i = 0; i < n && cx_ + i < int(r.size()); i++) r[size_t(cx_ + i)] = Cell{U' ', cur_, 1};
}

int Vt::param(size_t i, int fallback) const {
  return i < nums_.size() && nums_[i] >= 0 ? nums_[i] : fallback;
}

void Vt::exec_sgr() {
  if (nums_.empty()) { cur_ = Style{}; return; }
  for (size_t i = 0; i < nums_.size(); i++) {
    int n = nums_[i] < 0 ? 0 : nums_[i];
    switch (n) {
      case 0: cur_ = Style{}; break;
      case 1: cur_.a |= attr::kBold; break;
      case 2: cur_.a |= attr::kDim; break;
      case 3: cur_.a |= attr::kItalic; break;
      case 4: cur_.a |= attr::kUnderline; break;
      case 7: cur_.a |= attr::kReverse; break;
      case 22: cur_.a &= ~(attr::kBold | attr::kDim); break;
      case 23: cur_.a &= ~attr::kItalic; break;
      case 24: cur_.a &= ~attr::kUnderline; break;
      case 27: cur_.a &= ~attr::kReverse; break;
      case 39: cur_.fg = kDefaultColor; break;
      case 49: cur_.bg = kDefaultColor; break;
      case 38:
      case 48: {
        bool fg = n == 38;
        int kind = param(i + 1, 0);
        if (kind == 2) {
          Color c = (param(i + 2, 0) << 16) | (param(i + 3, 0) << 8) | param(i + 4, 0);
          (fg ? cur_.fg : cur_.bg) = c;
          i += 4;
        } else if (kind == 5) {
          (fg ? cur_.fg : cur_.bg) = xterm256(param(i + 2, 0));
          i += 2;
        }
        break;
      }
      default:
        if (n >= 30 && n <= 37) cur_.fg = xterm256(n - 30);
        else if (n >= 40 && n <= 47) cur_.bg = xterm256(n - 40);
        else if (n >= 90 && n <= 97) cur_.fg = xterm256(n - 90 + 8);
        else if (n >= 100 && n <= 107) cur_.bg = xterm256(n - 100 + 8);
        break;
    }
  }
}

void Vt::set_mode(bool on) {
  bool priv = !intermediates_.empty() && intermediates_[0] == '?';
  for (size_t i = 0; i < nums_.size(); i++) {
    int n = nums_[i];
    if (!priv) continue;
    switch (n) {
      case 1: app_cursor_ = on; break;
      case 7: autowrap_ = on; break;
      case 25: cursor_visible_ = on; break;
      case 1000: case 1002: case 1003: mouse_mode_ = on ? n : 0; break;
      case 1006: sgr_mouse_ = on; break;
      case 2004: bracketed_paste_ = on; break;
      case 1047: case 1049: case 47:
        if (on != alt_) {
          alt_ = on;
          if (on) {
            for (auto& r : alt_buf_) blank(r);
            saved_cx_ = cx_;
            saved_cy_ = cy_;
            set_cursor(0, 0);
          } else {
            set_cursor(saved_cx_, saved_cy_);
          }
        }
        break;
      default: break;  // 2026 sync, 1004 focus, 2031 theme: nothing to model
    }
  }
}

void Vt::exec_csi(char final) {
  // A private marker (?, >, <, =) makes a different command of the same final
  // byte: "CSI ? u" asks for the kitty keyboard flags, "CSI > 0 q" for the
  // terminal's name. Run as plain CSI u / CSI s they restored or saved the
  // cursor, and claude asks exactly that after drawing its first frame: the
  // cursor jumped to the top-left and every relative redraw after it (a menu
  // moving its ❯) landed on the wrong rows. Only modes (h/l) and selective
  // erase (? J, ? K) keep their meaning.
  if (!intermediates_.empty() && final != 'h' && final != 'l' && final != 'J' && final != 'K')
    return;
  switch (final) {
    case 'A': set_cursor(cx_, cy_ - std::max(1, param(0, 1))); break;
    case 'B': set_cursor(cx_, cy_ + std::max(1, param(0, 1))); break;
    case 'C': set_cursor(cx_ + std::max(1, param(0, 1)), cy_); break;
    case 'D': set_cursor(cx_ - std::max(1, param(0, 1)), cy_); break;
    case 'E': set_cursor(0, cy_ + std::max(1, param(0, 1))); break;
    case 'F': set_cursor(0, cy_ - std::max(1, param(0, 1))); break;
    case 'G': case '`': set_cursor(param(0, 1) - 1, cy_); break;
    case 'd': set_cursor(cx_, param(0, 1) - 1); break;
    case 'H': case 'f': set_cursor(param(1, 1) - 1, param(0, 1) - 1); break;
    case 'J': erase_in_display(param(0, 0)); break;
    case 'K': erase_in_line(param(0, 0)); break;
    case 'L': insert_lines(std::max(1, param(0, 1))); break;
    case 'M': delete_lines(std::max(1, param(0, 1))); break;
    case 'P': delete_chars(std::max(1, param(0, 1))); break;
    case '@': insert_chars(std::max(1, param(0, 1))); break;
    case 'X': erase_chars(std::max(1, param(0, 1))); break;
    case 'S': scroll_up(std::max(1, param(0, 1))); break;
    case 'T': scroll_down(std::max(1, param(0, 1))); break;
    case 'm': exec_sgr(); break;
    case 'h': set_mode(true); break;
    case 'l': set_mode(false); break;
    case 'r':
      top_ = std::clamp(param(0, 1) - 1, 0, h_ - 1);
      bot_ = std::clamp(param(1, h_) - 1, top_, h_ - 1);
      set_cursor(0, top_);
      break;
    case 's': saved_cx_ = cx_; saved_cy_ = cy_; break;
    case 'u': set_cursor(saved_cx_, saved_cy_); break;
    default: break;
  }
}

void Vt::exec_esc(char b) {
  switch (b) {
    case '7': saved_cx_ = cx_; saved_cy_ = cy_; saved_style_ = cur_; break;
    case '8': set_cursor(saved_cx_, saved_cy_); cur_ = saved_style_; break;
    case 'D': index(); break;
    case 'M': reverse_index(); break;
    case 'E': carriage_return(); index(); break;
    case 'c':
      cur_ = Style{};
      for (auto& r : screen_) blank(r);
      set_cursor(0, 0);
      break;
    default: break;
  }
}

void Vt::write(std::string_view bytes) {
  if (screen_.empty()) resize(w_, h_);
  if (!bytes.empty()) gen_ = next_generation();

  for (size_t i = 0; i < bytes.size(); i++) {
    unsigned char b = (unsigned char)bytes[i];

    switch (state_) {
      case State::Ground: {
        if (b == 0x1b) { state_ = State::Esc; params_.clear(); intermediates_.clear(); nums_.clear(); break; }
        if (b == '\n' || b == 0x0b || b == 0x0c) { newline(); break; }
        if (b == '\r') { carriage_return(); break; }
        if (b == '\t') { set_cursor((cx_ / 8 + 1) * 8, cy_); break; }
        if (b == 0x08) { set_cursor(cx_ - 1, cy_); break; }
        if (b == 0x07 || b == 0x0e || b == 0x0f) break;
        if (b < 0x20) break;

        // Fast path: consume the whole run of printable ASCII at once.
        if (utf8_.empty() && b < 0x7F) {
          size_t j = i;
          while (j < bytes.size()) {
            const unsigned char x = (unsigned char)bytes[j];
            if (x < 0x20 || x >= 0x7F) break;
            j++;
          }
          put_ascii_run(bytes.substr(i, j - i));
          i = j - 1;  // the for-loop increment lands on the terminator
          break;
        }

        // Multi-byte characters can straddle a read boundary.
        utf8_.push_back(char(b));
        size_t need = 1;
        unsigned char c0 = (unsigned char)utf8_[0];
        if ((c0 & 0xE0) == 0xC0) need = 2;
        else if ((c0 & 0xF0) == 0xE0) need = 3;
        else if ((c0 & 0xF8) == 0xF0) need = 4;
        if (utf8_.size() < need) break;
        size_t k = 0;
        char32_t cp = text::decode(utf8_, k);
        utf8_.clear();
        if (!extend(cp)) put(cp, std::max(1, text::cp_width(cp)));
        break;
      }

      case State::Esc: {
        if (b == '[') { state_ = State::Csi; break; }
        if (b == ']' || b == 'P' || b == '^' || b == '_') { state_ = b == ']' ? State::Osc : State::StringIgnore; params_.clear(); break; }
        // Charset designators take one more byte. Consuming it by advancing the
        // index breaks the moment the pair straddles a read.
        if (b == '(' || b == ')' || b == '*' || b == '+' || b == '%' || b == '#') {
          state_ = State::EscFinal;
          break;
        }
        exec_esc(char(b));
        state_ = State::Ground;
        break;
      }

      case State::Csi: {
        if ((b >= '0' && b <= '9') || b == ';' || b == ':') { params_.push_back(char(b)); break; }
        if (b == '?' || b == '>' || b == '<' || b == '!' || b == '=') { intermediates_.push_back(char(b)); break; }
        if (b >= 0x20 && b <= 0x2F) { intermediates_.push_back(char(b)); break; }
        if (b >= 0x40 && b <= 0x7E) {
          nums_.clear();
          int acc = -1;
          for (char c : params_) {
            if (c >= '0' && c <= '9') acc = (acc < 0 ? 0 : acc) * 10 + (c - '0');
            else { nums_.push_back(acc); acc = -1; }
          }
          nums_.push_back(acc);
          exec_csi(char(b));
          state_ = State::Ground;
          break;
        }
        state_ = State::Ground;
        break;
      }

      case State::EscFinal:
        state_ = State::Ground;
        break;

      case State::Osc:
        // OSC 8 hyperlinks and title sets carry no cell content; skip to ST.
        if (b == 0x07) { state_ = State::Ground; break; }
        if (b == 0x1b) state_ = State::OscEsc;
        break;

      case State::OscEsc:
        // ESC inside a string terminates it only when followed by a backslash;
        // anything else means the string was abandoned mid-way.
        state_ = (b == '\\') ? State::Ground : State::Osc;
        if (b != '\\' && b == 0x1b) state_ = State::OscEsc;
        break;

      case State::StringIgnore:
        if (b == 0x07) { state_ = State::Ground; break; }
        if (b == 0x1b) state_ = State::StringEsc;
        break;

      case State::StringEsc:
        state_ = (b == '\\') ? State::Ground : State::StringIgnore;
        if (b != '\\' && b == 0x1b) state_ = State::StringEsc;
        break;
    }
  }
}

}  // namespace mico
