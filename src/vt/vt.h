#pragma once
#include <deque>
#include <string>
#include <string_view>
#include <vector>

#include "vt/surface.h"

namespace mico {

using VtRow = std::vector<Cell>;

// A terminal emulator: bytes in, cell grid out. Both agents render inline
// rather than on the alternate screen, so scrollback is ours to keep and is
// part of the model, not an afterthought.
class Vt {
 public:
  void resize(int w, int h);
  void write(std::string_view bytes);

  int width() const { return w_; }
  int height() const { return h_; }

  // Rows addressed across scrollback + screen: 0 is the oldest retained line.
  int total_rows() const { return int(scrollback_.size()) + h_; }
  const VtRow& row(int i) const;

  Point cursor() const { return {cx_, cy_}; }
  bool cursor_visible() const { return cursor_visible_; }
  bool alt_screen() const { return alt_; }

  // Which input protocols the application asked for; the raw pane forwards
  // mouse events only when the agent actually wants them.
  bool wants_mouse() const { return mouse_mode_ != 0; }
  bool sgr_mouse() const { return sgr_mouse_; }
  bool app_cursor_keys() const { return app_cursor_; }
  bool bracketed_paste() const { return bracketed_paste_; }

 private:
  // Sequences arrive split across reads, so every multi-byte form needs a
  // state rather than a look-ahead into the current buffer.
  enum class State { Ground, Esc, EscFinal, Csi, Osc, OscEsc, StringIgnore, StringEsc };

  VtRow& line(int y);
  void put(char32_t cp, int w);
  // Bulk path for a run of printable ASCII, which is nearly all agent output.
  void put_ascii_run(std::string_view s);
  void newline();
  void index();          // move down, scrolling within the margins
  void reverse_index();
  void carriage_return() { cx_ = 0; wrap_pending_ = false; }
  void scroll_up(int n);
  void scroll_down(int n);
  void erase_in_display(int mode);
  void erase_in_line(int mode);
  void insert_lines(int n);
  void delete_lines(int n);
  void insert_chars(int n);
  void delete_chars(int n);
  void erase_chars(int n);
  void set_cursor(int x, int y);
  void blank(VtRow& r) const;

  void exec_csi(char final);
  void exec_sgr();
  void set_mode(bool on);
  void exec_esc(char b);
  int param(size_t i, int fallback) const;

  int w_ = 80, h_ = 24;
  int cx_ = 0, cy_ = 0;
  int saved_cx_ = 0, saved_cy_ = 0;
  int top_ = 0, bot_ = 23;  // scrolling region, inclusive
  bool wrap_pending_ = false;
  bool autowrap_ = true;
  bool cursor_visible_ = true;
  bool alt_ = false;
  bool app_cursor_ = false;
  bool sgr_mouse_ = false;
  bool bracketed_paste_ = false;
  int mouse_mode_ = 0;

  Style cur_{};
  Style saved_style_{};

  std::vector<VtRow> screen_;
  std::vector<VtRow> alt_buf_;
  std::deque<VtRow> scrollback_;

  State state_ = State::Ground;
  std::string params_;
  std::string intermediates_;
  std::vector<int> nums_;
  std::string utf8_;  // partial multi-byte character across write() calls
};

}  // namespace mico
