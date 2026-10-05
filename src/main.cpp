#include <poll.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>

#include "term/term.h"
#include "term/input.h"
#include "adapters/adapters.h"
#include "vt/vt.h"
#include "base/text.h"
#include "base/log.h"
#include "ui/app.h"
#include "math/math.h"
#include "ui/theme.h"
#include "views/chart.h"
#include "views/diagram.h"

#include <string>
#include <fstream>
#include <sstream>

namespace mico {
int run_bench();
int run_selftest();
int run_api_test();
int run_daemon();
int run_client(bool allow_spawn);
int kill_daemon();
int run_mcp_server();
}  // namespace mico

namespace {

// `mico --keys`: what this terminal sends for each key, and what mico makes
// of it. For a terminal (a phone's) whose Ctrl, Alt or function keys may not
// arrive as anything mico can tell apart.
int show_keys() {
  if (!mico::tty::enter_raw()) {
    fprintf(stderr, "mico --keys needs a terminal\n");
    return 1;
  }
  static const char* const kNames[] = {"none", "char", "Enter", "Esc", "Tab", "Shift+Tab", "Backspace", "Delete",
                                       "Up", "Down", "Left", "Right", "Home", "End", "PageUp", "PageDown",
                                       "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"};
  printf("Press keys to see what this terminal sends. q quits.\r\n");
  fflush(stdout);
  mico::InputDecoder dec;
  bool quit = false;
  while (!quit) {
    pollfd p{STDIN_FILENO, POLLIN, 0};
    const int r = ::poll(&p, 1, dec.pending_escape() ? 50 : -1);
    std::string bytes;
    if (r > 0) {
      char buf[256];
      const ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
      if (n <= 0) break;
      bytes.assign(buf, size_t(n));
      dec.feed(bytes);
      std::string shown;
      for (unsigned char c : bytes) {
        char b[8];
        if (c == 0x1b) shown += "ESC ";
        else if (c < 0x20 || c == 0x7f) { snprintf(b, sizeof b, "0x%02x ", c); shown += b; }
        else { shown += char(c); shown += ' '; }
      }
      printf("bytes: %s\r\n", shown.c_str());
    }
    auto ev = r > 0 ? dec.next() : dec.flush();
    for (; ev; ev = dec.next()) {
      if (ev->type != mico::InputEvent::Type::Key) {
        printf("   -> %s\r\n", ev->type == mico::InputEvent::Type::Mouse ? "mouse" : ev->type == mico::InputEvent::Type::Paste ? "paste" : "other");
        continue;
      }
      const mico::KeyEvent& k = ev->key;
      std::string name = std::string(k.ctrl ? "Ctrl+" : "") + (k.alt ? "Alt+" : "") + (k.shift ? "Shift+" : "");
      if (k.key == mico::Key::Char) {
        std::string ch;
        mico::text::encode(k.ch, ch);
        name += k.ch == ' ' ? std::string("Space") : ch;
      } else {
        name += kNames[int(k.key)];
      }
      printf("   -> %s\r\n", name.c_str());
      if (k.is('q')) quit = true;
    }
    fflush(stdout);
  }
  mico::tty::leave_raw();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  // The MCP server agents start: nothing else of mico, and only JSON on stdout.
  if (argc > 1 && !strcmp(argv[1], "--mcp")) return mico::run_mcp_server();
  // Claude runs it for every status line update: quick, quiet, no log.
  if (argc > 1 && !strcmp(argv[1], "--claude-status")) return mico::claude_status_line();
  int dump_w = 0, dump_h = 0, pick_p = 0, pick_s = 0, density = -1, scroll = 0;
  const char* vt_file = nullptr;
  const char* spawn_cmd = nullptr;
  const char* spawn_agent = nullptr;
  bool local = false, daemon = false, attach_only = false, kill = false, usage = false;
  // Draws one equation to a PNG: how the renderer is looked at and tuned.
  //   mico --math 'TEX' out.png [--inline] [--cell W H]
  if (argc > 3 && !strcmp(argv[1], "--math")) {
    bool display = true;
    int cw = 10, ch = 20;
    for (int i = 4; i < argc; i++) {
      if (!strcmp(argv[i], "--inline")) display = false;
      else if (!strcmp(argv[i], "--cell") && i + 2 < argc) {
        cw = atoi(argv[++i]);
        ch = atoi(argv[++i]);
      }
    }
    const mico::math::Image im = mico::math::draw(argv[2], display, cw, ch);
    const std::string png = mico::math::png(im, 0xDCE4EE);
    FILE* f = fopen(argv[3], "wb");
    if (!f) return 1;
    fwrite(png.data(), 1, png.size(), f);
    fclose(f);
    printf("%d x %d cells (%d x %d px), baseline row %d\n", im.cols, im.rows, im.w, im.h, im.base_row);
    return 0;
  }
  // Draws a ```mermaid block's text as the chat would, in cells, to stdout.
  //   mico --mermaid FILE [COLS]
  if (argc > 2 && !strcmp(argv[1], "--mermaid")) {
    FILE* f = fopen(argv[2], "rb");
    if (!f) return 1;
    std::string src;
    char buf[4096];
    for (size_t k; (k = fread(buf, 1, sizeof buf, f)) > 0;) src.append(buf, k);
    fclose(f);
    mico::chart::Figure fig;
    if (!mico::diagram::draw(src, argc > 3 ? atoi(argv[3]) : 100, fig)) {
      fprintf(stderr, "mico: not drawn\n");
      return 1;
    }
    for (const auto& row : fig.rows) {
      for (const auto& p : row) fputs(p.text.c_str(), stdout);
      fputc('\n', stdout);
    }
    return 0;
  }
  // Draws one chart spec (a ```chart block's JSON) to a PNG, as a terminal
  // that shows images would get it.   mico --chart 'JSON' out.png [--cell W H] [--cols N]
  if (argc > 3 && !strcmp(argv[1], "--chart")) {
    int cw = 10, ch = 20, cols = 90;
    for (int i = 4; i < argc; i++) {
      if (!strcmp(argv[i], "--cell") && i + 2 < argc) {
        cw = atoi(argv[++i]);
        ch = atoi(argv[++i]);
      } else if (!strcmp(argv[i], "--cols") && i + 1 < argc) {
        cols = atoi(argv[++i]);
      }
    }
    mico::chart::Spec spec;
    std::string why;
    if (!mico::chart::parse(argv[2], spec, &why) ||
        (!spec.file.empty() && !mico::chart::load_file(spec, ".", &why, nullptr, nullptr))) {
      fprintf(stderr, "mico: %s\n", why.c_str());
      return 1;
    }
    mico::math::configure(mico::math::Config{true, cw, ch, mico::active_theme().math});
    const mico::math::Image* im = mico::chart::image(spec, argv[2], cols);
    if (!im) return 1;
    const std::string png = mico::math::png(*im, 0);
    FILE* f = fopen(argv[3], "wb");
    if (!f) return 1;
    fwrite(png.data(), 1, png.size(), f);
    fclose(f);
    printf("%d x %d cells (%d x %d px)\n", im->cols, im->rows, im->w, im->h);
    return 0;
  }
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--dump")) {
      dump_w = 120;
      dump_h = 40;
      if (i + 2 < argc && argv[i + 1][0] != '-') {
        dump_w = atoi(argv[++i]);
        dump_h = atoi(argv[++i]);
      }
    } else if (!strcmp(argv[i], "--spawn") && i + 1 < argc) {
      spawn_cmd = argv[++i];
    } else if (!strcmp(argv[i], "--usage")) {
      usage = true;
    } else if (!strcmp(argv[i], "--local")) {
      local = true;
    } else if (!strcmp(argv[i], "--daemon")) {
      daemon = true;
    } else if (!strcmp(argv[i], "--attach")) {
      attach_only = true;
    } else if (!strcmp(argv[i], "kill") || !strcmp(argv[i], "--kill")) {
      kill = true;
    } else if (!strcmp(argv[i], "--bench")) {
      return mico::run_bench();
    } else if (!strcmp(argv[i], "--selftest")) {
      return mico::run_selftest();
    } else if (!strcmp(argv[i], "--api-test")) {
      return mico::run_api_test();
    } else if (!strcmp(argv[i], "--spawn-agent") && i + 1 < argc) {
      spawn_agent = argv[++i];
    } else if (!strcmp(argv[i], "--keys")) {
      return show_keys();
    } else if (!strcmp(argv[i], "--background") && i + 1 < argc) {
      // Replays a Claude transcript's background work: every start, event
      // and end, and what was still running at its last line.
      const char* file = argv[++i];
      std::ifstream f(file, std::ios::binary);
      std::string line;
      mico::BackgroundTasks t;
      uint64_t at = 0;
      std::vector<std::string> seen;
      while (std::getline(f, line)) {
        const auto before = t.running;
        mico::claude_adapter().read_background(line, at, t);
        for (const auto& r : t.running)
          if (std::none_of(before.begin(), before.end(), [&](const auto& b) { return b.id == r.id; }))
            printf("byte %llu: started %s %s: %s\n", (unsigned long long)at, r.kind.c_str(), r.id.c_str(), r.what.c_str());
        for (const auto& b : before)
          if (std::none_of(t.running.begin(), t.running.end(), [&](const auto& r) { return r.id == b.id; }))
            printf("byte %llu: ended %s %s\n", (unsigned long long)at, b.kind.c_str(), b.id.c_str());
        at += line.size() + 1;
      }
      printf("running at the end: %zu\n", t.running.size());
      for (const auto& r : t.running)
        printf("  %s %s: %s (%d events)\n", r.kind.c_str(), r.id.c_str(), r.what.c_str(), r.events);
      return 0;
    } else if (!strcmp(argv[i], "--vt") && i + 1 < argc) {
      vt_file = argv[++i];
    } else if (!strcmp(argv[i], "--project") && i + 1 < argc) {
      pick_p = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--session") && i + 1 < argc) {
      pick_s = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--scroll") && i + 1 < argc) {
      scroll = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--density") && i + 1 < argc) {
      density = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
      printf(
          "mico — a view over your coding agents' sessions\n\n"
          "  mico                 interactive\n"
          "  mico --dump [w h]    render one frame as text (for testing)\n"
          "  mico --dump --usage  render the usage tab instead\n"
          "  --project N --session N --density 0|1|2\n\n"
          "keys: tab focus · arrows/jk move · d density · right-click menu · q quit\n");
      return 0;
    }
  }

  if (vt_file) {
    // Replays captured PTY bytes through the emulator and prints the grid.
    std::ifstream f(vt_file, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    mico::Vt vt;
    vt.resize(dump_w ? dump_w : 80, dump_h ? dump_h : 24);
    vt.write(ss.str());
    std::string out;
    for (int y = 0; y < vt.total_rows(); y++) {
      const auto& r = vt.row(y);
      std::string linebuf;
      for (const auto& c : r) {
        if (c.width == 0) continue;
        mico::text::encode(c.cp ? c.cp : U' ', linebuf);
      }
      while (!linebuf.empty() && linebuf.back() == ' ') linebuf.pop_back();
      out += linebuf;
      out += '\n';
    }
    fwrite(out.data(), 1, out.size(), stdout);
    return 0;
  }

  mico::logs::init(daemon ? "daemon" : local ? "local" : attach_only ? "attach" : "client");
  mico::logs::install_crash_handler();
  if (kill) return mico::kill_daemon();
  if (daemon) return mico::run_daemon();

  // Attaching is the default: agents belong to the daemon, so closing this
  // terminal — or losing the ssh connection — leaves them running.
  const bool headless = dump_w || vt_file || spawn_cmd || spawn_agent;
  if (!local && !headless) return mico::run_client(!attach_only);

  mico::App app;
  app.start_scroll = scroll;
  if (usage) app.open_tab(1);
  if (density >= 0) app.filters().density = mico::Density(density);
  if (pick_p || pick_s) app.force_select(pick_p, pick_s);
  if (spawn_cmd) {
    char cwd[4096];
    app.spawn_raw({"/bin/sh", "-c", spawn_cmd}, getcwd(cwd, sizeof cwd) ? cwd : ".");
  }
  if (spawn_agent) {
    char cwd[4096];
    app.spawn_agent(spawn_agent, getcwd(cwd, sizeof cwd) ? cwd : ".");
  }
  return dump_w ? app.dump(dump_w, dump_h) : app.run();
}
