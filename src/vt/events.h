#pragma once
#include <string>

#include "vt/geom.h"

namespace mico {

// Input as mico understands it, whichever front end it came from. The host
// terminal's decoder (term/input.h) produces these; vt/keys.h re-encodes them
// for an agent's pty.
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
  // Focus: the terminal window gained or lost focus (focus_in says which),
  // reported once mico has asked for it with mode 1004.
  enum class Type { None, Key, Mouse, Resize, Paste, Focus } type = Type::None;
  KeyEvent key{};
  MouseEvent mouse{};
  std::string paste;
  bool focus_in = false;
};

}  // namespace mico
