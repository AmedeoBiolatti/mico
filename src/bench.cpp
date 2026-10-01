#include <sys/resource.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <string>

#include "core/json.h"
#include "core/adapter.h"
#include "term/encoder.h"
#include "term/vt.h"
#include "term/input.h"
#include "core/store.h"
#include "term/text.h"
#include "core/state.h"
#include "views/markdown.h"
#include "views/views.h"
#include "ui/app.h"
#include "views/chat_render.h"
#include "views/chart.h"
#include "math/deflate.h"
#include "math/math.h"
#include "math/sixel.h"

namespace mico {
namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

long peak_rss_kb() {
  rusage ru{};
  getrusage(RUSAGE_SELF, &ru);
  return ru.ru_maxrss;
}

// Peak RSS is a high-water mark and never falls, so it cannot show memory
// being handed back. Current RSS can.
long rss_kb() {
  FILE* f = fopen("/proc/self/statm", "r");
  if (!f) return 0;
  long total = 0, resident = 0;
  if (fscanf(f, "%ld %ld", &total, &resident) != 2) resident = 0;
  fclose(f);
  return resident * (sysconf(_SC_PAGESIZE) / 1024);
}

void line(const char* label, double ms, const char* note = "") {
  printf("  %-38s %8.1f ms   %s\n", label, ms, note);
}

}  // namespace

int run_bench() {
  Theme th;
  Filters f;
  printf("mico bench\n\n");

  // Pictures: what the first sight of an equation or a chart costs, at a
  // HiDPI cell size (the expensive case). Each is drawn once and cached.
  {
    printf("  pictures (18x38 px cells)\n\n");
    char note[96];
    const auto avg = [](auto&& fn, int n) {
      fn();
      const auto t0 = Clock::now();
      for (int i = 0; i < n; i++) fn();
      return ms_since(t0) / n;
    };
    const char* eq = R"(\begin{aligned} \mathbf{x}_{k+1} &= \mathbf{x}_k - \eta \nabla f(\mathbf{x}_k) \\ )"
                     R"(&= \begin{pmatrix} 1 - \eta a & -\eta b \\ -\eta c & 1 - \eta d \end{pmatrix} \mathbf{x}_k \end{aligned})";
    math::Image im;
    line("equation: lay out + draw", avg([&] { im = math::draw(eq, true, 18, 38); }, 50));
    std::vector<uint8_t> rgba(im.alpha.size() * 4);
    for (size_t i = 0; i < im.alpha.size(); i++) rgba[i * 4 + 3] = im.alpha[i];
    std::string z;
    const double zm = avg([&] { z = math::zlib_compress(rgba.data(), rgba.size()); }, 50);
    snprintf(note, sizeof note, "%zu KB -> %zu KB", rgba.size() >> 10, z.size() >> 10);
    line("equation: compress for kitty", zm, note);
    line("equation: encode as sixel", avg([&] { (void)math::sixel(im.alpha.data(), im.w, im.w, im.h, 0xFFFFFF, 0); }, 50));

    const math::Config saved = math::config();
    math::configure(math::Config{true, 18, 38, th.math});
    chart::Spec spec;
    std::string why, js = R"({"type":"line","title":"loss","series":[{"name":"train","y":[)";
    for (int i = 0; i < 2000; i++) js += std::to_string(2.0 / (1 + i * 0.01)) + (i < 1999 ? "," : "");
    js += R"(]},{"name":"val","y":[)";
    for (int i = 0; i < 2000; i++) js += std::to_string(2.2 / (1 + i * 0.008)) + (i < 1999 ? "," : "");
    js += "]}]}";
    chart::parse(js, spec, &why);
    int n = 0;
    const math::Image* ci = nullptr;
    line("chart: draw (2 x 2000 points)", avg([&] {
      spec.series[0].y[0] = 2.0 + 1e-9 * ++n;  // a new frame, not a cache hit
      ci = chart::image(spec, "bench", 100);
    }, 20));
    if (ci) {
      const double cz = avg([&] { z = math::zlib_compress(ci->rgba.data(), ci->rgba.size()); }, 20);
      snprintf(note, sizeof note, "%d x %d px, %zu KB -> %zu KB", ci->w, ci->h, ci->rgba.size() >> 10, z.size() >> 10);
      line("chart: compress for kitty", cz, note);
      line("chart: encode as sixel", avg([&] { (void)math::sixel_rgba(ci->rgba.data(), ci->w, ci->w, ci->h, th.panel); }, 20));
    }
    math::configure(saved);
    printf("\n");
  }

  auto t = Clock::now();
  Store store;
  store.add_folder("/home/bamedeo/Desktop/kaggle/kaggriculture", false);
  store.scan();
  double scan_ms = ms_since(t);
  char note[128];
  snprintf(note, sizeof note, "%zu projects, %zu sessions", store.projects().size(),
           store.session_count());
  line("Store::scan (cold listing)", scan_ms, note);

  // Pick the largest transcript available; that is the case that has to stay fast.
  const SessionRef* big = nullptr;
  for (const auto& p : store.projects())
    for (const auto& s : p.sessions)
      if (!big || s.bytes > big->bytes) big = &s;
  if (!big) { printf("  no sessions found\n"); return 0; }
  snprintf(note, sizeof note, "%.1f MB, %s", big->bytes / 1048576.0, big->agent.c_str());
  printf("\n  target: %s\n\n", note);

  t = Clock::now();
  ChatRenderer chat;
  chat.open(big->path, Store::adapter_for(*big));
  line("ChatRenderer::open (index)", ms_since(t));

  Surface s;
  s.resize(120, 40);
  Rect r{0, 0, 120, 40};

  t = Clock::now();
  { Painter p(s, r); chat.render(p, th, f); }
  line("first frame (tail)", ms_since(t));

  constexpr int kFrames = 60;
  t = Clock::now();
  for (int i = 0; i < kFrames; i++) { Painter p(s, r); chat.render(p, th, f); }
  line("60 steady frames at tail", ms_since(t), "should be ~0: nothing changed");

  t = Clock::now();
  for (int i = 0; i < kFrames; i++) {
    chat.set_scroll(i * 4);
    Painter p(s, r);
    chat.render(p, th, f);
  }
  line("60 frames scrolling back 240 rows", ms_since(t));

  t = Clock::now();
  chat.set_scroll(5000);
  { Painter p(s, r); chat.render(p, th, f); }
  line("jump to scroll 5000", ms_since(t));

  t = Clock::now();
  for (int i = 0; i < kFrames; i++) { Painter p(s, r); chat.render(p, th, f); }
  line("60 steady frames at scroll 5000", ms_since(t), "should be ~0: nothing changed");

  // A user scrolling back through the whole transcript. The window has to
  // slide, not accumulate: this is the case that used to retain the file.
  chat.to_bottom();
  { Painter p(s, r); chat.render(p, th, f); }
  const long before_rss = rss_kb();
  t = Clock::now();
  KeyEvent pgup{Key::PageUp};
  for (int i = 0; i < 3000; i++) {
    chat.on_key(pgup);
    Painter p(s, r);
    chat.render(p, th, f);
  }
  snprintf(note, sizeof note, "RSS %ld -> %ld MB, retains %zu MB, %zu window resets",
           before_rss / 1024, rss_kb() / 1024, chat.retained_bytes() >> 20,
           chat.window_resets());
  line("3000 page-ups back through the file", ms_since(t), note);

  printf("\n  current RSS %ld MB, peak %ld MB\n", rss_kb() / 1024, peak_rss_kb() / 1024);

  // ---- the daemon path: what a frame costs to build and to send ----
  printf("\n  daemon path (what an attached client actually pays)\n\n");
  {
    Surface back, front;
    const int W = 120, H = 40;
    back.resize(W, H);
    App app;

    t = Clock::now();
    for (int i = 0; i < 100; i++) app.draw(back);
    line("100 full app renders", ms_since(t), "layout + every pane");

    // First frame: everything is new.
    std::string frame;
    encode_frame(back, front, frame, true);
    snprintf(note, sizeof note, "%zu KB for %d cells", frame.size() >> 10, W * H);
    line("full repaint, encoded", 0.0, note);

    // Focus the chat pane: at startup the keyboard drives the project list,
    // which stops changing once it hits the top.
    for (int i = 0; i < 2; i++)
      app.feed(InputEvent{InputEvent::Type::Key, KeyEvent{Key::Tab}, {}, {}});

    // Steady state: scroll one line and see what a typical update costs.
    size_t total = 0;
    int frames = 0;
    t = Clock::now();
    for (int i = 0; i < 200; i++) {
      app.feed(InputEvent{InputEvent::Type::Key, KeyEvent{Key::Up}, {}, {}});
      app.draw(back);
      frame.clear();
      if (encode_frame(back, front, frame, false)) {
        total += frame.size();
        frames++;
      }
    }
    double ms = ms_since(t);
    snprintf(note, sizeof note, "%zu bytes/frame avg over %d frames", frames ? total / size_t(frames) : 0, frames);
    line("200 scroll frames: render + diff + encode", ms, note);
  }

  // ---- terminal emulation throughput ----
  {
    // A stand-in for agent output: mostly text, with the colour changes and
    // cursor moves a TUI actually emits. Built here so the measurement does
    // not depend on a captured file that can disappear.
    std::string blob;
    blob.reserve(8u << 20);
    const char* words =
        "the agent wrote a line of output here with some detail about what it did ";
    while (blob.size() < (8u << 20)) {
      blob += "\x1b[0;38;2;200;206;218;48;2;22;25;34m";
      blob += words;
      blob += "\r\n\x1b[1m";
      blob += words;
      blob += "\x1b[0m\r\n";
      if ((blob.size() & 0xFFFF) < 128) blob += "\x1b[3A\x1b[10C\xE2\x94\x80\xE2\x94\x80\r\n";
    }
    Vt vt;
    vt.resize(120, 40);
    t = Clock::now();
    vt.write(blob);
    const double ms = ms_since(t);
    snprintf(note, sizeof note, "%.0f MB/s", double(blob.size()) / (ms / 1000.0) / 1048576.0);
    line("Vt::write", ms, note);
  }

  return 0;
}

}  // namespace mico
