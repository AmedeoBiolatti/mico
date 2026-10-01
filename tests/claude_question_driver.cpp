// Linked against the real app by claude_questions.py; no provider is contacted.
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include "ui/app.h"
#include "views/views.h"
#include "term/text.h"

int main(int argc, char** argv) {
  if (argc != 4) return 2;
  const std::string fixture = argv[1], root = argv[2], mode = argv[3];
  mico::App app;
  mico::LiveSession session;
  mico::LiveSession::Launch launch;
  launch.agent = "claude";
  launch.cwd = root;
  launch.session_id = "question-fixture";
  launch.argv = {"python3", fixture, "--fixture", root, mode};
  session.start(launch);
  auto pane = mico::make_session_pane(&session);
  pane->set_app(&app);
  mico::Surface sf; sf.resize(110, 45);
  mico::Painter painter(sf, {0, 0, 110, 45});
  auto frame = [&] { session.pump(); pane->render(painter, true); };
  auto screen = [&] {
    std::string out;
    for (int y = 0; y < sf.height(); ++y) {
      for (int x = 0; x < sf.width(); ++x) mico::text::encode(sf.at(x, y).cp, out);
      out += '\n';
    }
    return out;
  };
  auto click = [&](std::string_view label) {
    frame();
    for (int y = 0; y < sf.height(); ++y) {
      std::string row;
      for (int x = 0; x < sf.width(); ++x) mico::text::encode(sf.at(x, y).cp, row);
      if (row.find(label) == std::string::npos) continue;
      pane->on_mouse({mico::MouseKind::Press, mico::MouseButton::Left}, {8, y});
      return true;
    }
    std::cerr << "Missing option: " << label << '\n';
    return false;
  };
  // Wait for the fixture's terminal to enter raw mode and display its form.
  for (int i = 0; i < 600; ++i) {
    frame();
    if (std::filesystem::exists(root + "/ready") && !session.transcript().empty()) break;
    usleep(5000);
  }
  bool ok = true;
  // A stale working footer must not label a question awaiting the user as
  // model thinking. This also covers answer forms opened mid-turn.
  session.vt().write("\x1b[2J\x1b[H✻ Thinking… (2s)\r\n");
  frame();
  if (screen().find("Thinking") != std::string::npos) {
    std::cerr << "Thinking shown while a question awaits the user\n"; ok = false;
  }
  session.vt().write("\x1b[2J\x1b[HFORM READY\r\n");
  if (mode == "single") ok = click("Bravo") && ok;
  else if (mode == "notes") {
    // n opens a note for the question in the prompt box; Enter pins it to
    // the card. It must not choose anything or reach the agent yet.
    pane->on_key({mico::Key::Char, 'n', false, false, false});
    for (char ch : std::string("cheaper to run")) pane->on_key({mico::Key::Char, char32_t(ch), false, false, false});
    pane->on_key({mico::Key::Enter});
    frame();
    if (screen().find("\xE2\x9C\x8E cheaper to run") == std::string::npos) {  // ✎
      std::cerr << "The note is not shown on the card\n"; ok = false;
    }
    ok = click("Bravo") && ok;
  }
  else if (mode == "multi_keyboard") {
    // Keyboard only. Enter toggles the option under the cursor and stays on
    // the question; it must not skip ahead to Submit.
    frame();
    pane->on_key({mico::Key::Enter});  // Alpha, where the cursor starts
    frame();
    if (screen().find("[ Submit ]") == std::string::npos ||
        screen().find("\xE2\x9D\xAF \xE2\x98\x91") == std::string::npos) {  // ❯ ☑
      std::cerr << "Enter did not toggle the option under the cursor\n"; ok = false;
    }
    pane->on_key({mico::Key::Down});
    pane->on_key({mico::Key::Down});
    pane->on_key({mico::Key::Enter});  // Charlie
    pane->on_key({mico::Key::Tab});    // onto Submit
    pane->on_key({mico::Key::Enter});
  } else {
    ok = click("Alpha") && click("Charlie") && ok;
    if (mode == "mixed") ok = ok && click("Echo");
    if (mode == "two_multi") ok = ok && click("Delta") && click("Echo");
    ok = ok && click("[ Submit ]");
  }
  frame();
  if (mode != "single" && mode != "notes" && screen().find("☑") == std::string::npos) {
    std::cerr << "Submitted selections disappeared\n"; ok = false;
  }
  if (mode == "cancel") {
    for (int i = 0; i < 400 && !std::filesystem::exists(root + "/keys"); ++i) {
      session.pump(); usleep(5000);
    }
    pane->on_key({mico::Key::F2});
  }
  // Deliberately do not render during transport: delivery must continue when
  // this pane is hidden or another chat is selected.
  for (int i = 0; i < 2000; ++i) {
    session.pump();
    if (!session.answer_sending() &&
        (mode == "cancel" || mode == "stall" || std::filesystem::exists(root + "/result.json"))) break;
    usleep(5000);
  }
  if (mode == "cancel") {
    for (int i = 0; i < 50; ++i) { session.pump(); usleep(5000); }
    ok = ok && !std::filesystem::exists(root + "/result.json");
  }
  else if (mode == "stall") ok = ok && session.answer_failed();
  else {
    frame();
    ok = ok && !session.answer_failed() && screen().find("MODEL_RESUMED") != std::string::npos;
    if (mode == "notes") {
      // The note follows the answer as a message of its own.
      for (int i = 0; i < 600 && !std::filesystem::exists(root + "/extra-input"); ++i) {
        session.pump(); usleep(5000);
      }
      std::ifstream in(root + "/extra-input", std::ios::binary);
      const std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      if (got.find("Note on my answer to \"Choose letters\": cheaper to run") == std::string::npos ||
          got.empty() || got.back() != '\r') {
        std::cerr << "The note did not follow the answer: " << got << '\n'; ok = false;
      }
    } else {
      // An extra Enter after a single answer must not spill into the composer.
      for (int i = 0; i < 50; ++i) { session.pump(); usleep(5000); }
      ok = ok && !std::filesystem::exists(root + "/extra-input");
    }
  }
  session.pty().terminate();
  if (!ok) std::cerr << screen();
  return ok ? 0 : 1;
}
