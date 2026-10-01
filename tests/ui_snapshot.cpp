// Render the real application surface with synthetic data. Linked by render_preview.py.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include "ui/app.h"

int main(int argc, char** argv) {
  const int w = argc > 1 ? std::atoi(argv[1]) : 120;
  const int h = argc > 2 ? std::atoi(argv[2]) : 36;
  const bool working = argc > 3 && std::string(argv[3]) == "working";
  const bool prompt = argc > 3 && std::string(argv[3]) == "prompt";
  const bool live = working || prompt || (argc > 3 && std::string(argv[3]) == "live");
  mico::App app;
  app.force_select(0, 0);
  mico::Surface surface;
  surface.resize(w, h);
  if (live) {
    const auto* s = app.current_session();
    if (!s || !app.spawn_continuation(s->agent, s->id, s->cwd, false)) return 2;
    app.draw(surface);
    app.service();
    if (prompt) {
      app.feed(mico::InputEvent{mico::InputEvent::Type::Paste, {}, {}, "Edit this café 界 text"});
      for (int i = 0; i < 6; ++i)
        app.feed(mico::InputEvent{mico::InputEvent::Type::Key, mico::KeyEvent{mico::Key::Left}});
    }
    if (working) {
      auto* session = app.selected_live();
      session->pty().write("Inspecting the project\n");
      for (int i = 0; i < 200 && !session->busy(); ++i) { app.service(); usleep(1000); }
    }
  }
  app.draw(surface);
  std::printf("%d %d\n", w, h);
  for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
    const auto& c = surface.at(x, y);
    std::printf("%u %d %d %u %u\n", unsigned(c.cp), c.st.fg, c.st.bg, c.st.a, c.width);
  }
  for (auto* s : app.live_sessions()) s->pty().terminate();
}
