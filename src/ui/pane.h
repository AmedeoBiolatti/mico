#pragma once
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "vt/surface.h"
#include "term/term.h"

namespace mico {

class App;
class LiveSession;

struct MenuItem {
  std::string label;
  std::string action;     // dispatched back to the pane that offered it
  bool enabled = true;
  bool separator = false;
  bool checked = false;
  std::string detail{};   // dim text after the label ("fork  —  fork the selected chat")
  std::string hint{};     // dim, at the right edge: the key that does the same ("Ctrl+G")

  static MenuItem sep() { return MenuItem{"", "", false, true, false}; }
};

// A rectangle of the screen that owns its content, input handling, and context
// menu. Everything the user sees is a Pane: project lists, chats, and later raw
// PTY views and overviews. The App only knows this interface.
class Pane {
 public:
  virtual ~Pane() = default;

  virtual std::string title() const = 0;
  // A short state drawn at the right end of the title bar ("○ Ready"), in the
  // pane's own colour. Empty for none. It costs no row inside the pane.
  struct Badge {
    std::string text;
    Color color = kDefaultColor;
  };
  virtual Badge badge() const { return {}; }
  // `p` is already clipped and translated: (0,0) is the pane's top-left.
  virtual void render(Painter& p, bool focused) = 0;

  virtual bool on_key(const KeyEvent& k) { return false; }
  // Bracketed-paste text. Return true if this pane consumed it; the App only
  // offers it to the focused pane, and a pane that does not care can leave it
  // unhandled (returning false), which drops it rather than replaying it as
  // individual keystrokes.
  virtual bool on_paste(std::string_view text) { return false; }
  // `local` is the cursor position in pane coordinates.
  virtual bool on_mouse(const MouseEvent& m, Point local) { return false; }
  // A link the last mouse event clicked, for the app to open; empty if none.
  virtual std::string take_url() { return {}; }

  // Items to show when the pane is right-clicked at `local`.
  virtual std::vector<MenuItem> context_menu(Point local) { return {}; }
  virtual void on_action(const std::string& action) {}

  // Called when the selection this pane depends on changed elsewhere.
  virtual void on_state_changed() {}

  // True when every keystroke belongs to this pane (a raw pty view forwards
  // them all to the agent). The App then withholds single-letter shortcuts and
  // leaves only the function-key globals, so typing "q" into an agent does not
  // quit mico.
  virtual bool captures_keys() const { return false; }

  // The agent this pane is showing, if it is showing one. Lets the chat list
  // point at panes without the App tracking a parallel index.
  virtual LiveSession* session() const { return nullptr; }

  void set_app(App* a) { app_ = a; }

 protected:
  App* app_ = nullptr;
};

using PanePtr = std::unique_ptr<Pane>;

}  // namespace mico
