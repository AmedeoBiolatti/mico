#pragma once
#include <optional>
#include <string>
#include <string_view>

#include "term/surface.h"

namespace mico {

enum class Key {
  None, Char, Enter, Escape, Tab, BackTab, Backspace, Delete,
  Up, Down, Left, Right, Home, End, PageUp, PageDown,
  F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
};

struct KeyEvent {
  Key key = Key::None;
  char32_t ch = 0;  // valid when key == Key::Char
  bool ctrl = false, alt = false, shift = false;

  bool is(char32_t c) const { return key == Key::Char && ch == c && !ctrl && !alt; }
  bool is_ctrl(char32_t c) const { return key == Key::Char && ch == c && ctrl; }
};

enum class MouseKind { Press, Release, Drag, Move, WheelUp, WheelDown };
enum class MouseButton { None, Left, Middle, Right };

struct MouseEvent {
  MouseKind kind = MouseKind::Move;
  MouseButton button = MouseButton::None;
  Point pos{};
  bool ctrl = false, alt = false, shift = false;
};

struct InputEvent {
  enum class Type { None, Key, Mouse, Resize, Paste } type = Type::None;
  KeyEvent key{};
  MouseEvent mouse{};
  std::string paste;
};

// Turns a byte stream from a terminal into events. Owns only its partial-
// sequence buffer, so the daemon can run one per attached client without a tty
// of its own.
class InputDecoder {
 public:
  void feed(std::string_view bytes) { pending_.append(bytes); }
  bool empty() const { return pending_.empty(); }
  void clear() { pending_.clear(); }

  // Returns the next complete event, or nullopt when more bytes are needed.
  std::optional<InputEvent> next();

  // A bare Escape is indistinguishable from the start of an escape sequence
  // until either more bytes arrive or enough time passes without them. Call
  // this when input has gone quiet to resolve it as a real Escape key.
  std::optional<InputEvent> flush();
  // True while a lone ESC is waiting to be resolved; poll on a short timeout so
  // pressing Escape does not feel laggy.
  bool pending_escape() const { return pending_.size() == 1 && pending_[0] == 0x1b; }

 private:
  std::optional<InputEvent> parse(size_t& consumed);
  std::string pending_;
};

}  // namespace mico
