#pragma once
#include <optional>
#include <string>

#include "term/caps.h"
#include "term/encoder.h"
#include "term/input.h"
#include "term/surface.h"

namespace mico {

// Terminal setup, shared by the local renderer and the thin client. The client
// owns its tty and does nothing else; keeping these here means one definition
// of what "mico has taken over the screen" means.
namespace tty {
extern const char* const kInit;  // alt screen, hide cursor, SGR mouse, paste
extern const char* const kFini;  // undoes kInit, and any background init_seq set
// kInit, then the terminal's default background set to `bg` (OSC 11). A window
// whose height is not a whole number of rows leaves a strip below the last one
// that the terminal fills with its default background; no cell reaches it, so
// without this it shows as a line in the terminal's own colour.
std::string init_seq(Color bg);
bool enter_raw();   // false if stdin/stdout is not a tty
void leave_raw();
bool query_size(int* w, int* h);
// Pixels per cell, from the window size the kernel knows; false if unknown.
bool cell_pixels(int* cw, int* ch);
// Asks the terminal what images it can draw, and how big its cells are:
// kitty's protocol with Unicode placeholders (kitty 0.28+, Ghostty; also
// through tmux with passthrough on), else sixel if its device attributes list
// it. MICO_GRAPHICS=off disables this, =kitty or =sixel forces one. Bytes the
// user typed while waiting are handed back in `rest`.
GfxCaps probe_graphics(std::string* rest);

// What a terminal said back to the probe. Split out so it can be tested.
struct ProbeReplies {
  bool graphics_ok = false;  // the kitty graphics query was answered OK
  bool sixel = false;        // device attributes list sixel
  std::string version;       // XTVERSION: "kitty(0.32.2)", "ghostty 1.1.0", …
  int cell_w = 0, cell_h = 0;
};
ProbeReplies read_probe_replies(std::string_view buf, std::string* rest);
// False for a kitty too old for Unicode placeholders.
bool kitty_version_ok(std::string_view version, bool kitty_term);
void install_winch(void (*handler)(int));
void write_all(int fd, std::string_view bytes);
}  // namespace tty


// Owns the tty: raw mode, the alternate screen, mouse reporting, and the
// front buffer that present() diffs against.
class Term {
 public:
  ~Term();

  bool start(Color bg = kDefaultColor);  // false if stdin/stdout is not a tty
  void stop();

  int width() const { return w_; }
  int height() const { return h_; }

  // Input is decoded once, here, whether mico runs locally or behind a socket.
  int input_fd() const;
  size_t ingest();                        // drain stdin; returns bytes read
  std::optional<InputEvent> next_event(); // pop one decoded event, if any
  std::optional<InputEvent> flush_escape() { return decoder_.flush(); }
  bool pending_escape() const { return decoder_.pending_escape(); }

  // Diffs `s` against the last presented frame and writes only what changed.
  void present(const Surface& s);
  // Extra bytes to emit with the next frame (mode changes, clipboard).
  void queue(std::string_view bytes) { out_ += bytes; }
  void set_mouse(bool on, bool any_event = false);
  const GfxCaps& caps() const { return caps_; }
  // How present() shows image cells. For sixel, `pass` draws them after the
  // text (it lives with the images; the terminal layer does not know them).
  using ImagePass = void (*)(const Surface& back, Surface& front, std::string& out);
  void set_images(ImageMode mode, ImagePass pass = nullptr) {
    images_ = mode;
    image_pass_ = pass;
  }
  bool mouse_on() const { return mouse_on_; }
  // Forces the next present() to repaint every cell (after a resize/suspend).
  void invalidate();

 private:
  void query_size();
  void flush();

  bool active_ = false;
  int w_ = 80, h_ = 24;
  InputDecoder decoder_;
  std::string out_;       // batched output
  Surface front_;
  bool dirty_all_ = true;
  bool mouse_on_ = true;
  bool mouse_any_ = false;
  GfxCaps caps_{};
  ImageMode images_ = ImageMode::None;
  ImagePass image_pass_ = nullptr;
};

}  // namespace mico
