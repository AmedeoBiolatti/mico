#include "term/term.h"

#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <strings.h>

#include "term/encoder.h"
#include "base/text.h"

namespace mico {
namespace {

termios g_saved{};
volatile sig_atomic_t g_resized = 0;
void on_winch(int) { g_resized = 1; }

}  // namespace

namespace tty {

// Enter: alt screen, hide cursor, SGR mouse (press/release/drag), bracketed paste.
const char* const kInit = "\x1b[?1049h\x1b[?25l\x1b[?1002h\x1b[?1006h\x1b[?2004h\x1b[2J";
// OSC 111 restores the terminal's configured background; terminals that do not
// know it ignore it, as they do OSC 11.
const char* const kFini =
    "\x1b[?2004l\x1b[?1006l\x1b[?1002l\x1b[?25h\x1b[0m\x1b]111\x1b\\\x1b[?1049l";

std::string background_seq(Color bg) {
  if (bg == kDefaultColor) return {};
  char osc[32];
  snprintf(osc, sizeof osc, "\x1b]11;#%06x\x1b\\", unsigned(bg) & 0xFFFFFFu);
  return osc;
}

std::string init_seq(Color bg) { return kInit + background_seq(bg); }

bool enter_raw() {
  if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return false;
  if (tcgetattr(STDIN_FILENO, &g_saved) != 0) return false;
  termios raw = g_saved;
  raw.c_iflag &= ~(unsigned)(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
  raw.c_oflag &= ~(unsigned)(OPOST);
  raw.c_lflag &= ~(unsigned)(ECHO | ICANON | ISIG | IEXTEN);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 0;
  return tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0;
}

void leave_raw() { tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved); }

bool cell_pixels(int* cw, int* ch) {
  winsize ws{};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0 || ws.ws_row == 0 ||
      ws.ws_xpixel == 0 || ws.ws_ypixel == 0)
    return false;
  *cw = ws.ws_xpixel / ws.ws_col;
  *ch = ws.ws_ypixel / ws.ws_row;
  return *cw > 0 && *ch > 0;
}

namespace {

// Runs a tmux command and returns its first line of output, trimmed.
std::string tmux_query(const char* args) {
  std::string cmd = "tmux ";
  cmd += args;
  cmd += " 2>/dev/null";
  std::string out;
  if (FILE* p = popen(cmd.c_str(), "r")) {
    char buf[256];
    if (fgets(buf, sizeof buf, p)) out = buf;
    pclose(p);
  }
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
  return out;
}

bool kitty_name(std::string_view term) {
  return term.find("kitty") != std::string_view::npos || term.find("ghostty") != std::string_view::npos;
}

}  // namespace

ProbeReplies read_probe_replies(std::string_view buf, std::string* rest) {
  ProbeReplies r;
  std::string keep;
  for (size_t i = 0; i < buf.size();) {
    if (buf.compare(i, 3, "\x1b_G") == 0 || buf.compare(i, 2, "\x1bP") == 0) {
      const size_t end = buf.find("\x1b\\", i);
      if (end == std::string_view::npos) break;
      if (buf[i + 1] == '_') {
        const std::string_view body = buf.substr(i + 3, end - i - 3);
        if (body.starts_with("i=31;") && body.find("OK") != std::string_view::npos) r.graphics_ok = true;
      } else if (buf.compare(i, 4, "\x1bP>|") == 0) {
        r.version.assign(buf.substr(i + 4, end - i - 4));
      }
      i = end + 2;
      continue;
    }
    if (buf.compare(i, 2, "\x1b[") == 0) {
      size_t k = i + 2;
      const bool priv = k < buf.size() && buf[k] == '?';
      if (priv) k++;
      const size_t params = k;
      while (k < buf.size() && (std::isdigit(uint8_t(buf[k])) || buf[k] == ';')) k++;
      if (k < buf.size() && (buf[k] == 'c' || buf[k] == 't')) {
        const std::string p(buf.substr(params, k - params));
        if (buf[k] == 't') {
          int kind = 0, h = 0, w = 0;
          if (sscanf(p.c_str(), "%d;%d;%d", &kind, &h, &w) == 3 && kind == 6 && w > 0 && h > 0) {
            r.cell_w = w;
            r.cell_h = h;
          }
        } else if (priv) {
          // Feature 4 in the device attributes is sixel graphics.
          r.sixel = (";" + p + ";").find(";4;") != std::string::npos;
        }
        i = k + 1;
        continue;
      }
    }
    keep.push_back(buf[i++]);
  }
  if (rest) *rest += keep;
  return r;
}

bool kitty_version_ok(std::string_view version, bool kitty_term) {
  // Unicode placeholders arrived in kitty 0.28. An older kitty still says OK
  // to the graphics query, and would draw every placeholder as a stray glyph.
  if (version.starts_with("kitty(")) {
    int major = 0, minor = 0;
    const std::string v(version.substr(6));
    return sscanf(v.c_str(), "%d.%d", &major, &minor) == 2 && (major > 0 || minor >= 28);
  }
  return !(version.empty() && kitty_term);  // a kitty too old even to say its version
}

GfxCaps probe_graphics(std::string* rest) {
  GfxCaps c;
  cell_pixels(&c.cell_w, &c.cell_h);
  const char* env = getenv("MICO_GRAPHICS");
  const std::string_view mode = env ? env : "";
  if (mode == "off" || mode == "0" || mode == "none") return c;
  const bool force_kitty = mode == "kitty", force_sixel = mode == "sixel";

  if (getenv("TMUX")) {
    // tmux answers queries itself, so the terminal outside is asked about
    // through tmux instead. Images get through only with passthrough on, and
    // the placeholders' colour (the image id) only if tmux keeps truecolour.
    const std::string pass = tmux_query("show-options -Apv allow-passthrough");
    const std::string client = tmux_query("display-message -p '#{client_termname}|#{client_termfeatures}'");
    const size_t bar = client.find('|');
    const std::string_view outer = std::string_view(client).substr(0, bar);
    const bool rgb = bar != std::string::npos && client.find("RGB", bar) != std::string::npos;
    if ((pass == "on" || pass == "all") && (force_kitty || (kitty_name(outer) && rgb))) {
      c.kitty = true;
      c.tmux = true;
    }
    return c;
  }

  const char* term = getenv("TERM");
  const char* prog = getenv("TERM_PROGRAM");
  const bool kittyish = force_kitty || getenv("KITTY_WINDOW_ID") || getenv("GHOSTTY_RESOURCES_DIR") ||
                        (term && kitty_name(term)) || (prog && !strcasecmp(prog, "ghostty"));

  // The terminal's name and version, a graphics query (only to terminals
  // that know the protocol: others may print it), the cell size, then primary
  // device attributes. Every terminal answers the last, so its reply says
  // nothing more is coming — and lists sixel support among its features.
  std::string ask = "\x1b[>q";
  if (kittyish) ask += "\x1b_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\x1b\\";
  ask += "\x1b[16t\x1b[c";
  write_all(STDOUT_FILENO, ask);
  std::string buf;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  bool done = false;
  while (!done) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()).count();
    if (left <= 0) break;
    pollfd p{STDIN_FILENO, POLLIN, 0};
    if (::poll(&p, 1, int(left)) <= 0) break;
    char tmp[512];
    const ssize_t n = read(STDIN_FILENO, tmp, sizeof tmp);
    if (n <= 0) continue;
    buf.append(tmp, size_t(n));
    // The DA reply: ESC [ ? … c
    for (size_t at = buf.find("\x1b[?"); at != std::string::npos; at = buf.find("\x1b[?", at + 1)) {
      size_t k = at + 3;
      while (k < buf.size() && (std::isdigit(uint8_t(buf[k])) || buf[k] == ';')) k++;
      if (k < buf.size() && buf[k] == 'c') { done = true; break; }
    }
  }

  const ProbeReplies r = read_probe_replies(buf, rest);
  if (r.cell_w > 0 && r.cell_h > 0 && (c.cell_w == 0 || c.cell_h == 0)) {
    c.cell_w = r.cell_w;
    c.cell_h = r.cell_h;
  }
  const bool graphics_ok = r.graphics_ok, da_sixel = r.sixel;
  const bool new_enough = kitty_version_ok(r.version, term && strstr(term, "kitty"));
  c.kitty = force_kitty || (kittyish && graphics_ok && new_enough);
  // Sixel pictures are drawn to exact pixels, so they need the cell size.
  c.sixel = !c.kitty && (force_sixel || da_sixel) && c.cell_w > 0 && c.cell_h > 0;
  return c;
}

bool query_size(int* w, int* h) {
  winsize ws{};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0) return false;
  *w = ws.ws_col;
  *h = ws.ws_row;
  return true;
}

void install_winch(void (*handler)(int)) {
  struct sigaction sa{};
  sa.sa_handler = handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;
  sigaction(SIGWINCH, &sa, nullptr);
}

void write_all(int fd, std::string_view bytes) {
  size_t off = 0;
  while (off < bytes.size()) {
    ssize_t n = ::write(fd, bytes.data() + off, bytes.size() - off);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      break;
    }
    off += size_t(n);
  }
}

}  // namespace tty

Term::~Term() { stop(); }

bool Term::start(Color bg) {
  if (!tty::enter_raw()) return false;
  tty::install_winch(on_winch);

  active_ = true;
  out_ = tty::init_seq(bg);
  flush();
  std::string typed;
  caps_ = tty::probe_graphics(&typed);
  if (!typed.empty()) decoder_.feed(typed);
  query_size();
  invalidate();
  return true;
}

void Term::stop() {
  if (!active_) return;
  active_ = false;
  out_ += tty::kFini;
  flush();
  tty::leave_raw();
}

void Term::query_size() {
  tty::query_size(&w_, &h_);
  // A font size change resizes the cells, not just the grid.
  int cw = 0, ch = 0;
  if (tty::cell_pixels(&cw, &ch)) {
    caps_.cell_w = cw;
    caps_.cell_h = ch;
  }
}

void Term::invalidate() {
  dirty_all_ = true;
  front_.resize(0, 0);
}

void Term::flush() {
  if (out_.empty()) return;
  size_t off = 0;
  while (off < out_.size()) {
    ssize_t n = write(STDOUT_FILENO, out_.data() + off, out_.size() - off);
    if (n <= 0) break;
    off += size_t(n);
  }
  out_.clear();
}

int Term::input_fd() const { return STDIN_FILENO; }

size_t Term::ingest() {
  char buf[8192];
  size_t total = 0;
  for (;;) {
    ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
    if (n > 0) { decoder_.feed(std::string_view(buf, size_t(n))); total += size_t(n); continue; }
    break;  // VMIN=0 means a short read just signals "drained"
  }
  return total;
}

std::optional<InputEvent> Term::next_event() {
  if (g_resized) {
    g_resized = 0;
    query_size();
    invalidate();
    InputEvent e;
    e.type = InputEvent::Type::Resize;
    return e;
  }
  return decoder_.next();
}

void Term::set_mouse(bool on, bool any_event) {
  if (on == mouse_on_ && any_event == mouse_any_) return;
  mouse_on_ = on;
  mouse_any_ = any_event;
  out_ += mouse_mode_seq(on, any_event);
  invalidate();
}

void Term::present(const Surface& s) {
  if (!active_) return;
  encode_frame(s, front_, out_, dirty_all_, images_);
  if (images_ == ImageMode::Sixel && image_pass_) image_pass_(s, front_, out_);
  dirty_all_ = false;
  flush();
}

}  // namespace mico
