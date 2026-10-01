// Linked against the real app by claude_paste.py; no provider is contacted.
#include <unistd.h>
#include <filesystem>
#include <iostream>
#include "ui/app.h"
#include "views/views.h"
#include "term/text.h"

int main(int argc, char** argv) {
  if (argc != 5) return 2;
  const std::string fixture = argv[1], root = argv[2], image = argv[3], mode = argv[4];
  mico::App app;
  mico::LiveSession session;
  mico::LiveSession::Launch launch;
  launch.agent = "claude";
  launch.cwd = root;
  launch.session_id = "paste-fixture";
  launch.argv = {"python3", fixture, mode == "queue" ? "--queue-fixture" : "--fixture", root};
  session.start(launch);
  auto pane = mico::make_session_pane(&session);
  pane->set_app(&app);
  mico::Surface sf; sf.resize(110, 40);
  mico::Painter painter(sf, {0, 0, 110, 40});
  auto frame = [&] { session.pump(); pane->render(painter, true); };
  auto screen = [&] {
    std::string out;
    for (int y = 0; y < sf.height(); ++y) {
      for (int x = 0; x < sf.width(); ++x)
        if (sf.at(x, y).width) mico::text::encode(sf.at(x, y).cp, out);
      out += '\n';
    }
    return out;
  };
  for (int i = 0; i < 600; ++i) {
    frame();
    if (std::filesystem::exists(root + "/ready") && !session.transcript().empty()) break;
    usleep(5000);
  }
  bool ok = true;
  const auto type = [&](const std::string& s) {
    for (char c : s) pane->on_key({mico::Key::Char, char32_t(c)});
  };
  const auto pump_ms = [&](int ms) {
    for (int i = 0; i < ms / 5; ++i) { frame(); usleep(5000); }
  };
  if (mode == "queue") {
    type("A");
    pane->on_key({mico::Key::Enter});
    for (int i = 0; i < 200 && !session.busy(); ++i) pump_ms(5);
    if (!session.busy()) { std::cerr << "fixture never started working\n"; ok = false; }
    // While it works: two queued (Alt+Enter), one steered (Enter).
    type("B"); pane->on_key({mico::Key::Char, U'\r', false, true});
    type("C"); pane->on_key({mico::Key::Char, U'\r', false, true});
    frame();
    if (screen().find("queued") == std::string::npos) { std::cerr << "queued messages not shown\n" << screen(); ok = false; }
    type("D"); pane->on_key({mico::Key::Enter});
    // ↑ in the empty box takes the newest queued message back to edit.
    type("E"); pane->on_key({mico::Key::Char, U'\r', false, true});
    pane->on_key({mico::Key::Up});
    frame();
    if (session.queued().size() != 2) { std::cerr << "up did not take the queued message back\n"; ok = false; }
    pane->on_key({mico::Key::Escape});  // drop the taken-back draft
    // Let both queued messages go out, one per turn.
    for (int i = 0; i < 1600 && !session.queued().empty(); ++i) pump_ms(5);
    pump_ms(1500);
    session.pty().terminate();
    return ok ? 0 : 1;
  }
  const auto expect = [&](const std::string& what, const char* why) {
    frame();
    if (screen().find(what) == std::string::npos) {
      std::cerr << why << "\n" << screen();
      ok = false;
    }
  };

  // A long paste is held as one token, not poured into the box.
  std::string long_text;
  {
    FILE* f = fopen((root + "/long.txt").c_str(), "r");
    char buf[4096];
    size_t n;
    while (f && (n = fread(buf, 1, sizeof buf, f)) > 0) long_text.append(buf, n);
    if (f) fclose(f);
  }
  pane->on_paste(long_text);
  std::string count = std::to_string(long_text.size());  // ASCII: bytes are characters
  for (int i = int(count.size()) - 3; i > 0; i -= 3) count.insert(size_t(i), ",");
  expect("[Pasted " + count + " characters]", "long paste not collapsed");
  if (screen().find("log line 0000") != std::string::npos) {
    std::cerr << "long paste shown in the box\n";
    ok = false;
  }
  for (char c : std::string("look ")) pane->on_key({mico::Key::Char, char32_t(c)});
  // A pasted image path becomes an image attachment.
  pane->on_paste(image);
  expect("[Image #1]", "image path not attached");
  for (char c : std::string("end")) pane->on_key({mico::Key::Char, char32_t(c)});
  pane->on_key({mico::Key::Enter});

  // Delivery is paced by the session: keep pumping until the fixture has the Enter.
  for (int i = 0; i < 1000 && !std::filesystem::exists(root + "/done"); ++i) {
    frame();
    usleep(5000);
  }
  if (!std::filesystem::exists(root + "/done")) {
    std::cerr << "message never completed\n";
    ok = false;
  }
  session.pty().terminate();
  return ok ? 0 : 1;
}
