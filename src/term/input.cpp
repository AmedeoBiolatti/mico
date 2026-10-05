#include "term/input.h"

#include "base/text.h"

namespace mico {

std::optional<InputEvent> InputDecoder::flush() {
  if (!pending_escape()) return std::nullopt;
  pending_.clear();
  InputEvent e;
  e.type = InputEvent::Type::Key;
  e.key = KeyEvent{Key::Escape};
  return e;
}

std::optional<InputEvent> InputDecoder::next() {
  if (pending_.empty()) return std::nullopt;
  size_t consumed = 0;
  auto ev = parse(consumed);
  if (consumed) pending_.erase(0, consumed);
  return ev;
}

// Returns the event at the head of pending_, setting `consumed` to the number of
// bytes it used. consumed==0 means "incomplete sequence, wait for more input".
std::optional<InputEvent> InputDecoder::parse(size_t& consumed) {
  consumed = 0;
  if (pending_.empty()) return std::nullopt;

  std::string_view p = pending_;
  auto key_ev = [&](Key k, char32_t ch = 0, bool ctrl = false, bool alt = false, bool shift = false) {
    InputEvent e;
    e.type = InputEvent::Type::Key;
    e.key = KeyEvent{k, ch, ctrl, alt, shift};
    return e;
  };

  if (p[0] != 0x1b) {
    auto b = uint8_t(p[0]);
    // Enter is CR (0x0d) in a raw terminal; Ctrl+J is LF (0x0a). They are
    // distinct keys: Enter sends, Ctrl+J inserts a newline. Treating LF as
    // Enter (as this did) made the multi-line prompt impossible to reach.
    if (b == '\r') { consumed = 1; return key_ev(Key::Enter); }
    if (b == '\t') { consumed = 1; return key_ev(Key::Tab); }
    // Backspace is DEL; Ctrl+Backspace sends BS instead (GNOME Terminal,
    // xterm, most others), which is how the two are told apart.
    if (b == 0x7F) { consumed = 1; return key_ev(Key::Backspace); }
    if (b == 0x08) { consumed = 1; return key_ev(Key::Backspace, 0, true); }
    if (b < 0x20) { consumed = 1; return key_ev(Key::Char, char32_t(b + 'a' - 1), true); }
    // A multi-byte character can straddle a read: the daemon and the client
    // both hand over whatever the last read produced. Decoding a partial one
    // yields replacement characters, so wait until all of it has arrived.
    // (The old guard tested decode()'s cursor, which never runs past the end,
    // so it could never fire.)
    size_t need = 1;
    if ((b & 0xE0) == 0xC0) need = 2;
    else if ((b & 0xF0) == 0xE0) need = 3;
    else if ((b & 0xF8) == 0xF0) need = 4;
    if (p.size() < need) return std::nullopt;

    size_t i = 0;
    char32_t cp = text::decode(p, i);
    consumed = i;
    return key_ev(Key::Char, cp);
  }

  if (p.size() == 1) return std::nullopt;  // lone ESC, or the start of a sequence

  // ESC followed by ESC: the first one is a completed Escape key. Without this
  // a buffered Escape swallows the next sequence and turns it into stray text.
  if (p[1] == 0x1b) { consumed = 1; return key_ev(Key::Escape); }

  // ESC O x  — application cursor / F1-F4
  if (p[1] == 'O') {
    if (p.size() < 3) return std::nullopt;
    consumed = 3;
    switch (p[2]) {
      case 'A': return key_ev(Key::Up);
      case 'B': return key_ev(Key::Down);
      case 'C': return key_ev(Key::Right);
      case 'D': return key_ev(Key::Left);
      case 'H': return key_ev(Key::Home);
      case 'F': return key_ev(Key::End);
      case 'P': return key_ev(Key::F1);
      case 'Q': return key_ev(Key::F2);
      case 'R': return key_ev(Key::F3);
      case 'S': return key_ev(Key::F4);
      default: return std::nullopt;
    }
  }

  if (p[1] == 0x7F || p[1] == 0x08) {  // Alt+Backspace
    consumed = 2;
    return key_ev(Key::Backspace, 0, p[1] == 0x08, true);
  }
  if (p[1] != '[') {  // ESC + key == Alt+key
    size_t i = 1;
    char32_t cp = text::decode(p, i);
    consumed = i;
    return key_ev(Key::Char, cp, false, true);
  }

  // CSI: collect parameter bytes then the final byte.
  size_t i = 2;
  while (i < p.size() && ((p[i] >= '0' && p[i] <= '9') || p[i] == ';' || p[i] == '<' || p[i] == '?'))
    i++;
  if (i >= p.size()) return std::nullopt;
  char final = p[i];
  std::string_view params = p.substr(2, i - 2);
  consumed = i + 1;

  auto num = [&](int idx, int fallback) {
    int seen = 0;
    size_t start = 0;
    for (size_t j = 0; j <= params.size(); j++) {
      if (j == params.size() || params[j] == ';') {
        if (seen == idx) {
          std::string_view f = params.substr(start, j - start);
          if (f.empty() || f[0] == '<') f.remove_prefix(f.empty() ? 0 : 1);
          if (f.empty()) return fallback;
          int v = 0;
          for (char c : f) {
            if (c < '0' || c > '9') return fallback;
            v = v * 10 + (c - '0');
          }
          return v;
        }
        seen++;
        start = j + 1;
      }
    }
    return fallback;
  };

  // SGR mouse: CSI < b ; x ; y (M|m)
  if (!params.empty() && params[0] == '<' && (final == 'M' || final == 'm')) {
    int b = num(0, 0), x = num(1, 1), y = num(2, 1);
    InputEvent e;
    e.type = InputEvent::Type::Mouse;
    e.mouse.pos = Point{x - 1, y - 1};
    e.mouse.shift = b & 4;
    e.mouse.alt = b & 8;
    e.mouse.ctrl = b & 16;
    if (b & 64) {
      e.mouse.kind = (b & 1) ? MouseKind::WheelDown : MouseKind::WheelUp;
    } else {
      int btn = b & 3;
      e.mouse.button = btn == 0   ? MouseButton::Left
                       : btn == 1 ? MouseButton::Middle
                       : btn == 2 ? MouseButton::Right
                                  : MouseButton::None;
      if (final == 'm') e.mouse.kind = MouseKind::Release;
      else if (b & 32) e.mouse.kind = (btn == 3) ? MouseKind::Move : MouseKind::Drag;
      else e.mouse.kind = MouseKind::Press;
    }
    return e;
  }

  // Bracketed paste: CSI 200~ ... CSI 201~
  if (final == '~' && num(0, 0) == 200) {
    constexpr std::string_view kEnd = "\x1b[201~";
    size_t end = p.find(kEnd, consumed);
    if (end == std::string_view::npos) { consumed = 0; return std::nullopt; }
    InputEvent e;
    e.type = InputEvent::Type::Paste;
    e.paste = std::string(p.substr(consumed, end - consumed));
    consumed = end + kEnd.size();
    return e;
  }

  int mod = num(1, 1) - 1;
  bool shift = mod & 1, alt = mod & 2, ctrl = mod & 4;
  // A key by its code point, with the modifiers above.
  const auto coded = [&](int code) -> std::optional<InputEvent> {
    if (code == 13) return key_ev(Key::Enter, 0, ctrl, alt, shift);
    if (code == 9) return key_ev(shift ? Key::BackTab : Key::Tab, 0, ctrl, alt, shift);
    if (code == 27) return key_ev(Key::Escape, 0, ctrl, alt, shift);
    if (code == 127 || code == 8) return key_ev(Key::Backspace, 0, ctrl, alt, shift);
    if (code >= 0x20 && code < 0x110000) {
      char32_t ch = char32_t(code);
      if (ctrl && ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
      return key_ev(Key::Char, ch, ctrl, alt, shift);
    }
    return std::nullopt;
  };

  switch (final) {
    case 'A': return key_ev(Key::Up, 0, ctrl, alt, shift);
    case 'B': return key_ev(Key::Down, 0, ctrl, alt, shift);
    case 'C': return key_ev(Key::Right, 0, ctrl, alt, shift);
    case 'D': return key_ev(Key::Left, 0, ctrl, alt, shift);
    case 'H': return key_ev(Key::Home, 0, ctrl, alt, shift);
    case 'F': return key_ev(Key::End, 0, ctrl, alt, shift);
    case 'Z': return key_ev(Key::BackTab);
    case 'I':
    case 'O': {
      // Focus reporting (mode 1004): CSI I as the window gains focus, CSI O
      // as it loses it.
      if (!params.empty()) return std::nullopt;
      InputEvent e;
      e.type = InputEvent::Type::Focus;
      e.focus_in = final == 'I';
      return e;
    }
    case 'u':
      // CSI keycode;mods u: the unambiguous form some terminals send for
      // keys the legacy bytes cannot tell apart (Shift+Enter, Ctrl+Shift+Z,
      // Ctrl+1, which is a plain '1' otherwise).
      return coded(num(0, 0));
    case '~': {
      switch (num(0, 0)) {
        // xterm's modifyOtherKeys: CSI 27 ; mods ; keycode ~, the same thing.
        case 27: return coded(num(2, 0));
        case 1: case 7: return key_ev(Key::Home, 0, ctrl, alt, shift);
        case 2: return key_ev(Key::None);
        case 3: return key_ev(Key::Delete, 0, ctrl, alt, shift);
        case 4: case 8: return key_ev(Key::End, 0, ctrl, alt, shift);
        case 5: return key_ev(Key::PageUp, 0, ctrl, alt, shift);
        case 6: return key_ev(Key::PageDown, 0, ctrl, alt, shift);
        case 15: return key_ev(Key::F5);
        case 17: return key_ev(Key::F6);
        case 18: return key_ev(Key::F7);
        case 19: return key_ev(Key::F8);
        case 20: return key_ev(Key::F9);
        case 21: return key_ev(Key::F10);
        case 23: return key_ev(Key::F11);
        case 24: return key_ev(Key::F12);
        default: return std::nullopt;
      }
    }
    default: return std::nullopt;
  }
}

}  // namespace mico
