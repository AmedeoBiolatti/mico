// Linked against the real app by claude_permission.py; no provider is contacted.
#include <unistd.h>
#include <filesystem>
#include <iostream>
#include "ui/app.h"
#include "views/views.h"
#include "base/text.h"

int main(int argc, char** argv) {
  if (argc != 4) return 2;
  const std::string fixture = argv[1], root = argv[2], mode = argv[3];
  mico::App app;
  mico::LiveSession session;
  mico::LiveSession::Launch launch;
  launch.agent = "claude";
  launch.cwd = root;
  launch.session_id = "permission-fixture";
  launch.argv = {"python3", fixture, "--fixture", root, mode};
  session.start(launch);
  auto pane = mico::make_session_pane(&session);
  pane->set_app(&app);
  mico::Surface sf; sf.resize(110, 40);
  mico::Painter painter(sf, {0, 0, 110, 40});
  auto frame = [&] { session.pump(); pane->render(painter, true); };
  auto row = [&](int y) {
    std::string r;
    for (int x = 0; x < sf.width(); ++x) mico::text::encode(sf.at(x, y).cp, r);
    return r;
  };
  auto screen = [&] {
    std::string out;
    for (int y = 0; y < sf.height(); ++y) out += row(y) + '\n';
    return out;
  };
  auto type = [&](std::string_view s) {
    for (char ch : s) pane->on_key({mico::Key::Char, char32_t(ch), false, false, false});
  };
  // The panel: the dialog's question, read off the fixture's screen.
  bool ok = false;
  for (int i = 0; i < 600 && !ok; ++i) {
    frame();
    ok = std::filesystem::exists(root + "/ready") &&
         screen().find("Do you want to proceed?") != std::string::npos &&
         screen().find("\xE2\x96\xB2 Bash command") != std::string::npos;  // ▲
    usleep(5000);
  }
  if (!ok) { std::cerr << "No permission panel\n" << screen(); return 1; }

  if (mode == "yes_note" || mode == "always_note" || mode == "no_note") {
    // n writes a note in the prompt box; Enter pins it to the panel.
    pane->on_key({mico::Key::Char, 'n', false, false, false});
    type(mode == "yes_note" ? "and log it" : mode == "no_note" ? "use gamma" : "fyi");
    pane->on_key({mico::Key::Enter});
    frame();
    if (screen().find("\xE2\x9C\x8E ") == std::string::npos) {  // ✎
      std::cerr << "The note is not shown on the panel\n" << screen(); return 1;
    }
  }
  if (mode == "yes_note") type("1");
  else if (mode == "always_note") type("2");
  else if (mode == "no_note") {
    pane->on_key({mico::Key::Down});
    pane->on_key({mico::Key::Down});
    pane->on_key({mico::Key::Enter});
  } else if (mode == "click_no") {
    int y = -1;
    for (int r = 0; r < sf.height() && y < 0; ++r)
      if (row(r).find("3. No") != std::string::npos) y = r;
    if (y < 0) { std::cerr << "No option row\n" << screen(); return 1; }
    pane->on_mouse({mico::MouseKind::Press, mico::MouseButton::Left}, {10, y});
  } else if (mode == "escape") {
    pane->on_key({mico::Key::Escape});
  }
  // Delivery continues without rendering, as when the chat is not on screen.
  for (int i = 0; i < 2000 && !std::filesystem::exists(root + "/extra-input"); ++i) {
    session.pump();
    usleep(5000);
  }
  frame();
  ok = !session.answer_failed() && std::filesystem::exists(root + "/result.json");
  if (!ok) std::cerr << "Answer not delivered\n" << screen();
  session.pty().terminate();
  return ok ? 0 : 1;
}
