#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>

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
int run_daemon();
int run_client(bool allow_spawn);
int kill_daemon();
int run_mcp_server();
}  // namespace mico

int main(int argc, char** argv) {
  // The MCP server agents start: nothing else of mico, and only JSON on stdout.
  if (argc > 1 && !strcmp(argv[1], "--mcp")) return mico::run_mcp_server();
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
    mico::math::configure(mico::math::Config{true, cw, ch, mico::Theme{}.math});
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
    } else if (!strcmp(argv[i], "--spawn-agent") && i + 1 < argc) {
      spawn_agent = argv[++i];
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
