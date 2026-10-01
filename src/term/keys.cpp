#include "term/keys.h"

#include <cstdio>

#include "term/text.h"

namespace mico {

std::string encode_key(const KeyEvent& k, bool app_cursor) {
  const char* ss3 = app_cursor ? "\x1bO" : "\x1b[";
  switch (k.key) {
    case Key::Enter: return "\r";
    case Key::Tab: return "\t";
    case Key::BackTab: return "\x1b[Z";
    case Key::Backspace:
      if (k.alt) return k.ctrl ? "\x1b\x08" : "\x1b\x7f";
      return k.ctrl ? "\x08" : "\x7f";
    case Key::Escape: return "\x1b";
    case Key::Delete: return "\x1b[3~";
    case Key::Up: return std::string(ss3) + "A";
    case Key::Down: return std::string(ss3) + "B";
    case Key::Right: return std::string(ss3) + "C";
    case Key::Left: return std::string(ss3) + "D";
    case Key::Home: return std::string(ss3) + "H";
    case Key::End: return std::string(ss3) + "F";
    case Key::PageUp: return "\x1b[5~";
    case Key::PageDown: return "\x1b[6~";
    case Key::Char: {
      std::string out;
      if (k.ctrl && k.ch >= 'a' && k.ch <= 'z') {
        out.push_back(char(k.ch - 'a' + 1));
        return out;
      }
      if (k.alt) out.push_back('\x1b');
      text::encode(k.ch, out);
      return out;
    }
    default: return {};
  }
}

std::string encode_mouse(const MouseEvent& m, bool sgr) {
  if (!sgr) return {};  // only SGR 1006 is worth emitting; agents all request it
  int b = 0;
  switch (m.kind) {
    case MouseKind::WheelUp: b = 64; break;
    case MouseKind::WheelDown: b = 65; break;
    default:
      b = m.button == MouseButton::Left     ? 0
          : m.button == MouseButton::Middle ? 1
          : m.button == MouseButton::Right  ? 2
                                            : 3;
      if (m.kind == MouseKind::Drag || m.kind == MouseKind::Move) b += 32;
      break;
  }
  if (m.shift) b |= 4;
  if (m.alt) b |= 8;
  if (m.ctrl) b |= 16;
  char buf[48];
  snprintf(buf, sizeof buf, "\x1b[<%d;%d;%d%c", b, m.pos.x + 1, m.pos.y + 1,
           m.kind == MouseKind::Release ? 'm' : 'M');
  return buf;
}

std::string encode_paste(const std::string& text, bool bracketed) {
  if (!bracketed) return text;
  return "\x1b[200~" + text + "\x1b[201~";
}

}  // namespace mico
