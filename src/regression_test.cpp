#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include "adapters/adapters.h"
#include "base/json.h"
#include "base/jsonl.h"
#include "core/session.h"
#include "core/store.h"
#include "adapters/user_text.h"
#include "vt/keys.h"
#include "base/text.h"
#include "ui/app.h"
#include "term/input.h"
#include "views/chat_render.h"
#include "views/views.h"
#include "vt/vt.h"
#include "term/encoder.h"

namespace mico {
namespace {
struct EnvScope {
  std::string key;
  std::optional<std::string> old;
  EnvScope(const char* name, const std::string& value) : key(name) {
    if (const char* v = getenv(name)) old = v;
    setenv(name, value.c_str(), 1);
  }
  ~EnvScope() {
    if (old) setenv(key.c_str(), old->c_str(), 1);
    else unsetenv(key.c_str());
  }
};
void put(const std::string& path, const std::string& text, bool append = false) {
  std::filesystem::create_directories(std::filesystem::path(path).parent_path());
  std::ofstream f(path, append ? std::ios::app : std::ios::trunc);
  f << text;
}
std::string screen(const Surface& sf) {
  std::string out;
  for (int y = 0; y < sf.height(); ++y) {
    for (int x = 0; x < sf.width(); ++x) {
      const auto& c = sf.at(x, y);
      if (c.width) text::encode(c.cp ? c.cp : U' ', out);
    }
    out += '\n';
  }
  return out;
}
const std::string call = R"({"type":"response_item","payload":{"type":"function_call","name":"read","call_id":"old","arguments":"file"}})" "\n";
const std::string result = R"({"type":"response_item","payload":{"type":"function_call_output","call_id":"old","output":"ok"}})" "\n";
}  // namespace

int run_regression_tests() {
  int failures = 0;
  auto check = [&](bool ok, const char* what) {
    if (!ok) { printf("  FAIL  regression: %s\n", what); ++failures; }
  };
  char tmp[] = "/tmp/mico_regression_XXXXXX";
  if (!mkdtemp(tmp)) { check(false, "create isolated fixture directory"); return failures; }
  const std::string base = tmp;
  // Everything, including child processes and App's folder seeding, is isolated
  // from real agent transcripts and user configuration.
  EnvScope home("HOME", base);
  EnvScope config("XDG_CONFIG_HOME", base + "/config");
  const std::string project = base + "/project", other = base + "/other";
  std::filesystem::create_directories(project);
  std::filesystem::create_directories(other);
  put(base + "/config/mico/folders", project + "\n" + other + "\n");

  {
    const std::string path = base + "/message-layout.jsonl";
    const auto message = [](const char* source, const std::string& body) {
      return std::string("{\"type\":\"") + source + "\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"" + body + "\"}]}}\n";
    };
    put(path, message("user", "\\n \\t\\r\\n<pasted_content id=\\\"bdd7\\\">\\nFIRSTUSER\\n\\nSECONDLINE\\n</pasted_content>") +
              message("user", "NEXTUSER") + message("assistant", "FIRSTANSWER") +
              R"({"type":"assistant","message":{"content":[{"type":"tool_use","id":"between","name":"Bash","input":{"command":"true"}}]}})" "\n" +
              message("assistant", "SECONDANSWER") + message("user", "LASTUSER") +
              message("assistant", "LASTANSWER"));
    ChatRenderer chat;
    chat.open(path, &claude_adapter());
    Surface sf; sf.resize(80, 40);
    Painter painter(sf, {0, 0, 80, 40});
    const auto count = [](const std::string& text, const std::string& word) {
      size_t n = 0, at = 0;
      while ((at = text.find(word, at)) != std::string::npos) { ++n; at += word.size(); }
      return n;
    };
    chat.render(painter, Theme{}, Filters{});
    auto rendered = screen(sf);
    const auto row_of = [&](const std::string& word) {
      const size_t at = rendered.find(word);
      return at == std::string::npos ? -1 : int(std::count(rendered.begin(), rendered.begin() + at, '\n'));
    };
    // Turns carry no "You" / agent label: the gutter and tint say who speaks.
    const auto first_text_row = [&] {
      int row = 0;
      for (size_t at = 0; at < rendered.size(); ++row) {
        const size_t nl = rendered.find('\n', at);
        const std::string line = rendered.substr(at, nl - at);
        if (line.find_first_not_of(" \xE2\x96\x8C") != std::string::npos) return row;  // not blank, not only the gutter
        if (nl == std::string::npos) break;
        at = nl + 1;
      }
      return -1;
    };
    check(row_of("FIRSTUSER") >= 0 && row_of("FIRSTUSER") == first_text_row(),
          "user content is the first thing shown, leading blank paste lines dropped");
    check(row_of("SECONDLINE") == row_of("FIRSTUSER") + 2,
          "blank lines inside a user message are preserved");
    check(count(rendered, "You") == 0 && count(rendered, "Claude") == 0,
          "turns carry no speaker labels");
    check(row_of("FIRSTANSWER") >= 0 && row_of("SECONDANSWER") > row_of("FIRSTANSWER") &&
              row_of("LASTANSWER") > row_of("LASTUSER"),
          "every turn is still shown, in order");
    put(path, message("assistant", "APPENDEDANSWER"), true);
    chat.poll_growth();
    chat.render(painter, Theme{}, Filters{});
    check(screen(sf).find("APPENDEDANSWER") != std::string::npos && count(screen(sf), "Claude") == 0,
          "live appended messages arrive without a label");
    sf.resize(60, 40);
    Painter narrow(sf, {0, 0, 60, 40});
    Filters minimal; minimal.density = Density::Minimal;
    chat.render(narrow, Theme{}, minimal);
    check(count(screen(sf), "Claude") == 0 && count(screen(sf), "You") == 0,
          "no labels at other widths and densities either");

    std::string history;
    for (int i = 0; i < 520; ++i) history += message("assistant", "HISTORY" + std::to_string(i));
    const std::string history_path = base + "/grouped-history.jsonl";
    put(history_path, history);
    ChatRenderer grouped;
    grouped.open(history_path, &claude_adapter());
    grouped.render(narrow, Theme{}, Filters{});
    sf.resize(60, 1100);
    Painter tall(sf, {0, 0, 60, 1100});
    grouped.render(tall, Theme{}, Filters{});
    check(count(screen(sf), "Claude") == 0 && screen(sf).find("HISTORY0") != std::string::npos &&
              screen(sf).find("HISTORY519") != std::string::npos,
          "loading older history across a chunk boundary shows every message");
  }

  {
    const std::string envelope = R"(<task-notification>\n<task-id>background-job</task-id>\n<tool-use-id>old</tool-use-id>\n<output-file>/tmp/internal-task.output</output-file>\n<status>completed</status>\n<summary>Background command &quot;Capture HUD&quot; completed (exit code 0)</summary>\n</task-notification>)";
    const auto record = [](const std::string& content) {
      return "{\"type\":\"user\",\"message\":{\"content\":\"" + content + "\"}}\n";
    };
    Arena arena;
    std::vector<Event> events;
    claude_adapter().parse(record(envelope), arena, events);
    check(events.size() == 1 && events[0].kind == EventKind::TaskStatus && events[0].ok &&
              events[0].tool_id == hash_id("old") &&
              arena.view(events[0].text) == "Background command \"Capture HUD\" completed (exit code 0)",
          "Claude background notifications become task summaries with decoded text");
    for (const std::string& content : {"Explain this: " + envelope,
                                      "```xml\\n" + envelope + "\\n```",
                                      envelope + "\\nMy additional message",
                                      std::string("<task-notification>unfinished")}) {
      events.clear();
      claude_adapter().parse(record(content), arena, events);
      check(events.size() == 1 && events[0].kind == EventKind::User,
            "quoted, mixed, and incomplete task notifications stay user text");
    }
    const std::string path = base + "/task-notification.jsonl";
    const std::string task = R"({"type":"assistant","message":{"content":[{"type":"tool_use","id":"old","name":"Bash","input":{"command":"capture HUD"}}]}})" "\n";
    put(path, task + record(envelope));
    ChatRenderer chat;
    chat.open(path, &claude_adapter());
    chat.set_working(true, U'X');
    Surface sf; sf.resize(100, 16);
    Painter painter(sf, {0, 0, 100, 16});
    chat.render(painter, Theme{}, Filters{});
    const auto rendered = screen(sf);
    check(rendered.find("Capture HUD") != std::string::npos &&
              rendered.find("task-notification") == std::string::npos &&
              rendered.find("internal-task.output") == std::string::npos &&
              rendered.find("You") == std::string::npos,
          "task updates render compactly without XML, internal paths, or user attribution");
    check(!chat.in_flight_tool(nullptr, nullptr), "a background completion clears the matching tool's running indicator");
    std::string failure = envelope;
    failure.replace(failure.find("<status>completed"), std::string("<status>completed").size(), "<status>failed");
    failure.replace(failure.find("completed (exit code 0)"), std::string("completed (exit code 0)").size(), "failed (exit code 1)");
    put(path, record(failure), true);
    chat.poll_growth();
    Filters minimal; minimal.density = Density::Minimal;
    chat.render(painter, Theme{}, minimal);
    check(screen(sf).find("failed (exit code 1)") != std::string::npos,
          "failed background tasks remain visible in minimal chat mode");

    // Shapes seen in real transcripts: orphans a previous session left
    // "stopped" (several task ids, no tool id), and Monitor events, which have
    // an <event> and no <status>.
    const std::string orphans = R"(<task-notification>\n<task-id>b1</task-id>\n<task-id>b2</task-id>\n<task-id>__orphan_summary__:shell</task-id>\n<status>stopped</status>\n<summary>2 background shell command tasks didn't finish before the previous session ended.</summary>\n<note>No completion record was found.</note>\n</task-notification>)";
    const std::string monitor = R"(<task-notification>\n<task-id>bw</task-id>\n<summary>Monitor event: &quot;bake&quot;</summary>\n<event>done foundry 24 s</event>\nIf this event is something the user would act on now, send a PushNotification.\n</task-notification>)";
    events.clear();
    claude_adapter().parse(record(orphans), arena, events);
    claude_adapter().parse(record(monitor), arena, events);
    check(events.size() == 2 && events[0].kind == EventKind::TaskStatus && !events[0].ok &&
              arena.view(events[0].text).starts_with("2 background shell") &&
              events[1].kind == EventKind::TaskStatus && events[1].ok &&
              arena.view(events[1].text) == "Monitor event: \"bake\" \xE2\x80\x94 done foundry 24 s",
          "stopped orphans and Monitor events are task activity, not user text");
  }

  {
    const std::string wrapped = "<pasted_content id=\"9253\">\ndid you receive the answer?\n</pasted_content id=\"9253\">";
    auto cleaned = [&](const std::string& input) {
      Arena arena;
      const Str result = unwrap_pasted_content(arena, arena.add(input));
      return std::string(arena.view(result));
    };
    check(cleaned(wrapped) == "did you receive the answer?", "paste envelopes retain only their message text");
    const std::string screenshot_message = "improve the arm and the merger look. now they look pretty generic";
    check(cleaned("<pasted_content id=\"bdd7\">\n" + screenshot_message + "\n</pasted_content id=\"bdd7\">") ==
              screenshot_message, "Claude's hexadecimal paste id from the screenshot is unwrapped");
    check(cleaned("Before\n" + wrapped + "\nAfter\n" + wrapped) ==
              "Before\ndid you receive the answer?\nAfter\ndid you receive the answer?",
          "multiple paste envelopes preserve surrounding text");
    check(cleaned("<pasted_content id=\"1\">\r\n  indented\r\n\r\nsecond\r\n</pasted_content>") ==
              "  indented\r\n\r\nsecond", "paste cleanup preserves content whitespace and accepts plain closing tags");
    check(cleaned("```xml\n" + wrapped + "\n```") == "```xml\n" + wrapped + "\n```",
          "literal paste tags in fenced code remain visible");
    const std::string incomplete = "<pasted_content id=\"9253\">\nunfinished";
    check(cleaned(incomplete) == incomplete, "incomplete paste envelopes do not discard text");
    const std::string mismatch = "<pasted_content id=\"1\">\ntext\n</pasted_content id=\"2\">";
    check(cleaned(mismatch) == mismatch, "mismatched paste ids remain intact");
    const std::string quoted = "Use `<pasted_content id=\"9253\">` to describe it.";
    check(cleaned(quoted) == quoted, "inline examples of paste tags remain intact");

    for (const std::string id : {"9253", "bdd7", "paste_A9-uuid"}) {
      const std::string content = "\"<pasted_content id=\\\"" + id +
          "\\\">\\ndid you receive the answer?\\n</pasted_content id=\\\"" + id + "\\\">\"";
      for (const Adapter* adapter : {&claude_adapter(), &codex_adapter(), &pi_adapter(), &omp_adapter()}) {
        Arena arena;
        std::vector<Event> events;
        const std::string record = adapter->id() == "claude"
            ? "{\"type\":\"user\",\"message\":{\"content\":" + content + "}}"
            : adapter->id() == "codex"
            ? "{\"type\":\"response_item\",\"payload\":{\"type\":\"message\",\"role\":\"user\",\"content\":[{\"text\":" + content + "}]}}"
            : "{\"type\":\"message\",\"message\":{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":" + content + "}]}}";
        adapter->parse(record, arena, events);
        check(events.size() == 1 && events[0].kind == EventKind::User &&
                  arena.view(events[0].text) == "did you receive the answer?",
              "each agent normalizes paste wrappers in user messages");
        const std::string path = base + "/paste-" + std::string(adapter->id()) + ".jsonl";
        put(path, record + "\n");
        ChatRenderer chat;
        chat.open(path, adapter);
        Surface sf; sf.resize(80, 12);
        Painter painter(sf, Rect{0, 0, 80, 12});
        chat.render(painter, Theme{}, Filters{});
        const auto rendered = screen(sf);
        check(rendered.find("did you receive the answer?") != std::string::npos &&
                  rendered.find("pasted_content") == std::string::npos,
              "chat displays the pasted message without its wrapper");
      }
    }
  }

  {
    // Claude writes a pending AskUserQuestion to its transcript only when a
    // hook is about to read it; without one the chat saw no question at all.
    const auto has_settings = [](const LiveSession& s) {
      return std::find(s.argv().begin(), s.argv().end(), "--settings") != s.argv().end();
    };
    LiveSession claude, other_program, own_settings;
    LiveSession::Launch launch;
    launch.agent = "claude"; launch.cwd = project; launch.session_id = "hook-fixture";
    launch.argv = {"/usr/local/bin/claude", "--resume", "hook-fixture"};
    claude.start(launch);
    launch.argv = {"/bin/cat"};
    other_program.start(launch);
    launch.argv = {"claude", "--settings", "mine.json"};
    own_settings.start(launch);
    const auto& a = claude.argv();
    const auto at = std::find(a.begin(), a.end(), "--settings");
    check(at != a.end() && at + 1 != a.end() && at[1].find("\"AskUserQuestion\"") != std::string::npos &&
              at[1].find("PreToolUse") != std::string::npos,
          "claude is launched with a question hook so pending questions reach the transcript");
    check(!has_settings(other_program), "a non-claude program under the claude agent gets no claude flags");
    // Agents are told about charts at launch, unless that is turned off; a
    // command line that already sets the prompt or instructions is left alone.
    {
      const auto count = [](const std::vector<std::string>& a, std::string_view flag) {
        return std::count(a.begin(), a.end(), std::string(flag));
      };
      const auto hinted = [](const std::vector<std::string>& a) {
        return std::any_of(a.begin(), a.end(), [](const std::string& x) { return x.find("```") == std::string::npos && x.find("`chart`") != std::string::npos; });
      };
      check(count(claude.argv(), "--append-system-prompt") == 1 && hinted(claude.argv()),
            "claude is told about charts at launch");
      LiveSession codex, mine;
      LiveSession::Launch cl;
      cl.agent = "codex"; cl.cwd = project;
      cl.argv = {"codex", "resume", "abc"};
      codex.start(cl);
      check(codex.argv().size() > 2 && codex.argv()[1] == "-c" &&
                codex.argv()[2].starts_with("developer_instructions=\"") && codex.argv()[3] == "resume",
            "codex gets the charts note as developer_instructions, before its subcommand");
      cl.agent = "claude"; cl.session_id = "own-prompt";
      cl.argv = {"claude", "--append-system-prompt", "mine"};
      mine.start(cl);
      check(count(mine.argv(), "--append-system-prompt") == 1, "a claude prompt of the user's own is kept");
      // mico's MCP tools: absent until turned on; then claude gets the server
      // and plot allowed, codex the server with plot approved.
      const auto starts = [](const std::vector<std::string>& a, std::string_view p) {
        return std::any_of(a.begin(), a.end(), [&](const std::string& x) { return x.starts_with(p); });
      };
      check(!starts(claude.argv(), "--mcp-config"), "agents get no MCP server unless it is turned on");
      set_mcp_tools(true);
      LiveSession mc, mx;
      cl.agent = "claude"; cl.session_id = "mcp-on"; cl.argv = {"claude"};
      mc.start(cl);
      cl.agent = "codex"; cl.session_id.clear(); cl.argv = {"codex"};
      mx.start(cl);
      check(starts(mc.argv(), "--mcp-config={\"mcpServers\":{\"mico\"") && starts(mc.argv(), "--allowedTools=mcp__mico__plot") &&
                std::any_of(mc.argv().begin(), mc.argv().end(), [](const std::string& x) { return x.find("`plot` tool") != std::string::npos; }),
            ":mcp on gives claude mico's server, plot allowed, and says so in its note");
      check(starts(mx.argv(), "mcp_servers.mico.command=") && starts(mx.argv(), "mcp_servers.mico.tools.plot.approval_mode=\"approve\""),
            ":mcp on gives codex mico's server with plot approved");
      set_mcp_tools(false);
      set_agent_hints(false);
      LiveSession off;
      cl.argv = {"claude"};
      cl.session_id = "hints-off";
      off.start(cl);
      check(count(off.argv(), "--append-system-prompt") == 0, ":charts off stops telling agents");
      set_agent_hints(true);
    }
    check(std::count(own_settings.argv().begin(), own_settings.argv().end(), "--settings") == 1,
          "a launch that brings its own --settings keeps it");
  }

  {
    // The sidebar's marks: a spinner while it works; a reply only once the
    // work has stayed stopped, so a blink of Claude's spinner between steps
    // is no answer; and read as soon as the chat is on screen.
    App app;
    LiveSession session;
    LiveSession::Launch launch;
    launch.agent = "claude"; launch.cwd = project; launch.session_id = "marks-fixture";
    launch.argv = {"/bin/cat"};
    session.start(launch);
    const auto pump_for = [&](int ms) {
      for (int i = 0; i < ms / 10; ++i) { session.pump(); usleep(10000); }
    };
    const auto work = [&](bool on) {
      session.vt().write(on ? "\x1b[2J\x1b[H\xE2\x9C\xBB Thinking\xE2\x80\xA6 (2s)\r\n\xE2\x9D\xAF \r\n"
                            : "\x1b[2J\x1b[H\xE2\x9D\xAF \r\n");
      session.pump();
    };
    // The pty is spawned by the pane's first frame.
    auto pane = make_session_pane(&session);
    pane->set_app(&app);
    Surface sf; sf.resize(80, 20);
    Painter painter(sf, {0, 0, 80, 20});
    pane->render(painter, false);
    pump_for(50);
    work(true);
    const ChatState busy = chat_state(&session, app.theme(), 3);
    check(busy.rank == 2 && busy.glyph == spinner_glyph(3) && busy.glyph != chat_state(&session, app.theme(), 4).glyph,
          "marks: a working chat turns a spinner");
    work(false);
    pump_for(300);
    work(true);
    work(false);
    pump_for(300);
    check(!session.unseen() && chat_state(&session, app.theme()).rank == 3,
          "marks: a pause between steps is not a reply");
    pump_for(1300);
    check(session.unseen() && chat_state(&session, app.theme()).rank == 1,
          "marks: a turn that stayed finished is a new reply");
    pane->render(painter, false);
    check(!session.unseen() && chat_state(&session, app.theme()).rank == 3,
          "marks: a chat on screen is read, focused or not");
  }

  {
    const std::string activity_path = base + "/.claude/projects/fixture/activity-fixture.jsonl";
    put(activity_path, R"({"type":"assistant","message":{"content":[{"type":"text","text":"Activity fixture"}]}})" "\n");
    App app;
    LiveSession session;
    LiveSession::Launch launch;
    launch.agent = "claude"; launch.cwd = project; launch.session_id = "activity-fixture";
    launch.argv = {"/bin/cat"};
    session.start(launch);
    auto pane = make_session_pane(&session);
    pane->set_app(&app);
    Surface sf; sf.resize(80, 20);
    Painter painter(sf, {0, 0, 80, 20});
    const auto right_timer = [&] {
      for (int y = 0; y < sf.height(); ++y) {
        if (sf.at(0, y).st.bg != app.theme().strip_bg || sf.at(78, y).cp != 's') continue;
        std::string value;
        for (int x = 77; x <= 79; ++x) text::encode(sf.at(x, y).cp, value);
        return value;
      }
      return std::string{};
    };
    pane->render(painter, true);
    session.pty().write("Idle redraw\n");
    for (int i = 0; i < 100; ++i) { session.pump(); usleep(1000); }
    pane->render(painter, true);
    check(!session.busy() && screen(sf).find("Thinking") == std::string::npos,
          "Claude idle terminal output does not flash Thinking");
    session.vt().write("\x1b[2J\x1b[H✻ Thinking… (2s)\r\n❯ \r\n");
    session.pump();
    pane->render(painter, true);
    check(session.busy() && screen(sf).find("Thinking") != std::string::npos,
          "Claude's active footer enables the activity row");
    check(right_timer() == "0s ", "activity timer starts at the right edge of the bar");
    usleep(1300000);
    pane->render(painter, true);
    check(session.busy() && screen(sf).find("Thinking") != std::string::npos,
          "quiet Claude work does not blink off after an output timeout");
    check(!right_timer().empty() && right_timer() != "0s ", "thinking timer advances across redraws");
    const auto tool_call = [](const std::string& id) {
      return "{\"type\":\"assistant\",\"message\":{\"content\":[{\"type\":\"tool_use\",\"id\":\"" + id +
          "\",\"name\":\"Bash\",\"input\":{\"command\":\"same-command\"}}]}}\n";
    };
    put(activity_path, tool_call("timed-one"), true);
    pane->render(painter, true);
    check(right_timer() == "0s ", "starting a tool resets the thinking timer");
    const auto command_count = [&] {
      const std::string rendered = screen(sf);
      size_t at = 0, count = 0;
      while ((at = rendered.find("same-command", at)) != std::string::npos) { ++count; ++at; }
      return count;
    };
    check(command_count() == 1, "the running command appears only in the reserved activity row");
    ChatRenderer stored;
    stored.open(activity_path, &claude_adapter());
    Surface history; history.resize(80, 20);
    Painter history_painter(history, {0, 0, 80, 20});
    stored.render(history_painter, Theme{}, Filters{});
    check(screen(history).find("same-command") != std::string::npos,
          "stored chats retain tool rows without an activity bar");
    usleep(1100000);
    pane->on_key(KeyEvent{Key::F2});
    pane->render(painter, true);
    pane->on_key(KeyEvent{Key::F2});
    pane->render(painter, true);
    check(!right_timer().empty() && right_timer() != "0s ", "tool elapsed time survives switching views");
    check(command_count() == 1, "returning from raw view does not duplicate the active command");
    put(activity_path, R"({"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"timed-one","content":"done"}]}})" "\n", true);
    pane->render(painter, true);
    check(command_count() == 1 && screen(sf).find("Thinking") != std::string::npos,
          "completed commands return to history while the activity bar resumes Thinking");
    put(activity_path, tool_call("timed-two"), true);
    pane->render(painter, true);
    check(right_timer() == "0s ", "a new call resets the timer even when its name and arguments match");
    check(command_count() == 2, "a new identical command keeps the completed history entry and one activity entry");
    session.vt().write("\x1b[2J\x1b[H✻ Worked for 5s\r\n❯ \r\n");
    pane->render(painter, true);
    check(!session.busy() && screen(sf).find("Thinking") == std::string::npos,
          "Claude's completed footer clears Thinking immediately");
    check(right_timer().empty(), "idle activity clears the elapsed timer");
    session.pty().terminate();
    std::filesystem::remove(activity_path);
  }

  {
    // An agent gets its terminal and nothing else of mico's: not a descriptor
    // mico left inheritable, nor another agent's terminal. Held, they would
    // keep the daemon's sockets and other panes alive past mico's exit.
    int leak[2];
    check(pipe(leak) == 0, "fds: a descriptor that would be inherited");
    Pty first, second;
    check(first.spawn({"/bin/sleep", "5"}, project, 80, 24) && second.spawn({"/bin/sleep", "5"}, project, 80, 24),
          "fds: two agents start");
    const auto fds_of = [](pid_t pid) {
      std::vector<std::string> out;
      const std::string dir = "/proc/" + std::to_string(pid) + "/fd";
      for (int i = 0; i < 200; i++) {
        out.clear();
        for (const auto& e : std::filesystem::directory_iterator(dir)) out.push_back(e.path().filename());
        std::error_code ec;
        if (std::filesystem::read_symlink("/proc/" + std::to_string(pid) + "/exe", ec).filename() == "sleep") break;
        usleep(5000);
      }
      return out.size();
    };
    check(fds_of(second.pid()) == 3, "fds: an agent holds only its terminal");
    close(leak[0]);
    close(leak[1]);
    first.terminate();
    second.terminate();
  }

  {
    Pty pty;
    check(pty.spawn({"/bin/sh", "-c", "stty raw -echo; printf READY; exec cat"}, project, 80, 24),
          "start the paste backpressure fixture");
    std::string echoed;
    for (int i = 0; i < 500 && echoed.find("READY") == std::string::npos; ++i) {
      pty.read_available(echoed); usleep(1000);
    }
    check(echoed == "READY", "paste fixture enters raw mode before sending");
    echoed.clear();
    const std::string paste(256u << 10, 'p');
    pty.write(paste);
    pty.write("END");
    for (int i = 0; i < 2000 && echoed.size() < paste.size() + 3; ++i) {
      pty.read_available(echoed); pty.flush_input(); usleep(1000);
    }
    check(echoed == paste + "END", "PTY backpressure preserves the full paste and following keystrokes");
    pty.terminate();
  }

  {
    const std::string path = base + "/partial.jsonl";
    put(path, "complete\nhalf");
    Jsonl file;
    check(file.open(path) && file.line_count() == 1, "opening a partial record excludes it");
    put(path, "-done\n", true);
    check(file.refresh() && file.line_count() == 2 && file.line(1) == "half-done",
          "partial record is parsed exactly once after completion");

    const std::string big = base + "/big.jsonl";
    put(big, "first\n" + std::string(6u << 20, 'x') + "\nlast\n");
    Jsonl original;
    original.open(big);
    Jsonl moved(std::move(original));
    check(!moved.complete(), "moving a lazy index preserves its unread prefix");
    while (!moved.complete()) moved.extend_back();
    check(moved.line_count() == 3 && moved.line(0) == "first" &&
              moved.line(1).size() == (6u << 20), "a line larger than the slab does not hide history");

    const std::string empty = base + "/empty.jsonl";
    put(empty, "");
    ChatRenderer chat;
    check(!chat.open(empty, &codex_adapter()), "an empty transcript is initially unavailable");
    put(empty, call);
    check(chat.open(empty, &codex_adapter()), "an initially empty transcript can be reopened");
  }

  {
    const std::string path = base + "/tools.jsonl";
    std::string body = call + R"({"type":"response_item","payload":{"type":"function_call","name":"request_user_input","call_id":"question","arguments":"{\"questions\":[{\"question\":\"Choose?\",\"options\":[{\"label\":\"yes\"}]}]}"}})" "\n";
    for (int i = 0; i < 600; ++i) body += "{\"type\":\"ignored\"}\n";
    body += result + R"({"type":"response_item","payload":{"type":"function_call_output","call_id":"question","output":"yes"}})" "\n";
    put(path, body);
    ChatRenderer chat;
    chat.open(path, &codex_adapter());
    chat.set_questions_interactive(true);
    chat.set_working(true, U'X');
    Surface sf; sf.resize(80, 20);
    Painter p(sf, Rect{0, 0, 80, 20});
    Theme theme; Filters filters;
    chat.render(p, theme, filters);
    chat.set_scroll(2000);
    chat.render(p, theme, filters);
    check(!chat.in_flight_tool(nullptr, nullptr), "backward parsing does not revive completed tools");
    check(!chat.has_pending_question(), "backward parsing does not revive answered questions");

    put(path, call, true);
    chat.poll_growth(); chat.to_bottom(); chat.render(p, theme, filters);
    check(chat.in_flight_tool(nullptr, nullptr), "a new unresolved call becomes active");
    put(path, R"({"type":"event_msg","payload":{"type":"turn_aborted"}})" "\n", true);
    chat.poll_growth();
    check(!chat.in_flight_tool(nullptr, nullptr), "an aborted turn settles unresolved calls");
    put(path, call, true);
    chat.poll_growth();
    chat.set_working(false, U'X');
    check(!chat.in_flight_tool(nullptr, nullptr), "an idle agent does not report an active tool");
  }

  {
    const std::string dir = base + "/.codex/sessions/2026/09/21/";
    const std::string sid = "11111111-1111-4111-8111-111111111111";
    const std::string path = dir + "rollout-old.jsonl";
    put(path, "{\"type\":\"session_meta\",\"payload\":{\"id\":\"" + sid +
                  "\",\"cwd\":\"" + project + "\",\"instructions\":\"" +
                  std::string(100000, 'x') + "\"}}\n");
    const std::string legacy = dir + "rollout-legacy.jsonl";
    put(legacy, "{\"type\":\"session_meta\",\"payload\":{\"session_id\":\"legacy\",\"cwd\":\"" + project + "\"}}\n");
    Store store;
    store.scan();
    bool modern_found = false, legacy_found = false;
    for (const auto& pr : store.projects()) for (const auto& s : pr.sessions) {
      modern_found |= s.id == sid;
      legacy_found |= s.id == "legacy";
    }
    check(modern_found && legacy_found, "discovery accepts both metadata id fields and large headers");
    {
      const std::string question_path = dir + "rollout-question.jsonl";
      put(question_path, "{\"type\":\"session_meta\",\"payload\":{\"id\":\"question-fixture\",\"cwd\":\"" + project + "\"}}\n" +
          R"({"type":"response_item","payload":{"type":"function_call","name":"request_user_input","call_id":"pick","arguments":"{\"questions\":[{\"header\":\"Library\",\"question\":\"Choose a library\",\"options\":[{\"label\":\"Alpha option\"},{\"label\":\"Beta option\"}]}]}"}})" "\n");
      App app;
      LiveSession session;
      LiveSession::Launch launch;
      launch.agent = "codex"; launch.cwd = project; launch.session_id = "question-fixture";
      launch.argv = {"/bin/sh", "-c", "stty raw -echo; printf READY; exec cat"};
      session.start(launch); session.set_geometry(80, 36);
      auto receive = [&](size_t count) {
        std::string bytes;
        for (int i = 0; i < 200 && bytes.size() < count; ++i) {
          session.pty().read_available(bytes); session.pty().flush_input(); usleep(1000);
        }
        return bytes;
      };
      check(receive(5) == "READY", "question fixture is ready for exact input capture");
      session.pump();
      auto pane = make_session_pane(&session);
      pane->set_app(&app);
      Surface sf; sf.resize(80, 36);
      Painter painter(sf, Rect{0, 0, 80, 36});
      pane->render(painter, true);
      int option_y = -1;
      for (int y = 0; y < sf.height(); ++y) {
        std::string row;
        for (int x = 0; x < sf.width(); ++x) text::encode(sf.at(x, y).cp, row);
        if (row.find("Beta option") != std::string::npos) option_y = y;
      }
      check(option_y >= 0, "question option is visible in the live pane");
      if (option_y >= 0) {
        pane->on_mouse(MouseEvent{MouseKind::Press, MouseButton::Left}, Point{8, option_y});
        check(receive(4) == "\x1b[B\r", "clicking a question sends the selected answer to the PTY");
        // A second input can arrive before the next render clears the hit map.
        pane->on_mouse(MouseEvent{MouseKind::Press, MouseButton::Left}, Point{8, option_y});
        check(receive(1).empty(), "a stale question hit cannot submit the same answer twice");
      }
      put(question_path, R"({"type":"response_item","payload":{"type":"function_call_output","call_id":"pick","output":"Beta"}})" "\n", true);
      pane->render(painter, true);
      for (const std::string draft : {"VISIBLE_TEXT", "Aé界Z", "line one\nline two"}) {
        pane->on_paste(draft);
        pane->render(painter, true);
        const Surface original = sf;
        bool retained = true, highlighted = false;
        for (size_t i = 0; i < draft.size(); ++i) {
          pane->on_key(KeyEvent{Key::Left});
          pane->render(painter, true);
          for (int y = 0; y < sf.height(); ++y) for (int x = 0; x < sf.width(); ++x) {
            const Cell& before = original.at(x, y);
            const Cell& after = sf.at(x, y);
            // The end-of-line caret is the only glyph allowed to change.
            if (before.cp != U'▏' && after.cp != U'▏')
              retained &= before.cp == after.cp && before.width == after.width;
            else if (after.cp == U'▏')
              retained &= before.cp == U' ' || before.cp == U'▏';
            highlighted |= after.cp != U' ' && after.width > 0 &&
                           after.st.bg == Theme{}.accent && after.st.fg != after.st.bg;
          }
        }
        check(retained && highlighted,
              "moving the prompt cursor preserves every glyph and column with a contrasting highlight");
        pane->on_key(KeyEvent{Key::Escape});
      }
      pane->on_paste("MESSAGE_TEST");
      pane->on_key(KeyEvent{Key::Enter});
      check(receive(13) == "MESSAGE_TEST\r", "chat sends a message to the same PTY");
      session.vt().write("\r\n\xe2\x9d\xaf MESSAGE_TEST\r\nDo you want to continue?\r\n");
      pane->render(painter, true);
      check(pane->title().find("terminal") == std::string::npos,
            "sending a message does not switch chat to raw on prompt-like output");
      pane->on_key(KeyEvent{Key::F2});
      check(pane->title().find("terminal") != std::string::npos, "F2 still opens raw view explicitly");
      pane->on_key(KeyEvent{Key::F2});
      check(pane->title().find("terminal") == std::string::npos, "F2 returns to chat view");

      // A long transcript exposes viewport jumps that a short, top-aligned
      // conversation would hide. Status changes must not move its final line.
      std::string history;
      for (int i = 0; i < 45; ++i) history += "History line " + std::to_string(i) + "\\n\\n";
      history += "STABLETAILMARKER";
      put(question_path, "{\"type\":\"response_item\",\"payload\":{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"text\":\"" + history + "\"}]}}\n", true);
      const auto tail_row = [&] {
        pane->render(painter, true);
        for (int y = 0; y < sf.height(); ++y) {
          std::string row;
          for (int x = 0; x < sf.width(); ++x) text::encode(sf.at(x, y).cp, row);
          if (row.find("STABLETAILMARKER") != std::string::npos) return y;
        }
        return -1;
      };
      const int idle_y = tail_row();
      // Codex at work: its status line over its input box, the bullet styled
      // (a reply's is plain).
      const auto codex_screen = [&](std::string_view status) {
        session.pty().write("\x1b[2J\x1b[H" + std::string(status) + "\r\n\r\n\xe2\x80\xba Ask Codex to do anything\r\n");
      };
      codex_screen("\x1b[1m\xe2\x80\xa2\x1b[0m Working (0s \xe2\x80\xa2 esc to interrupt)");
      for (int i = 0; i < 200 && !session.busy(); ++i) { session.pump(); usleep(1000); }
      const int busy_y = tail_row();
      check(session.busy() && idle_y >= 0 && busy_y == idle_y,
            "starting activity keeps the transcript at the same screen position");
      codex_screen("\x1b[1m\xe2\x80\xa2\x1b[0m Working (1s \xe2\x80\xa2 esc to interrupt)");
      for (int i = 0; i < 50; ++i) { session.pump(); usleep(1000); }
      check(tail_row() == idle_y, "additional terminal hints stay in the reserved activity row");
      codex_screen("");
      for (int i = 0; i < 1600 && session.busy(); ++i) { session.pump(); usleep(1000); }
      check(!session.busy() && tail_row() == idle_y && screen(sf).find("Thinking") == std::string::npos,
            "ending activity clears its reserved row without moving the transcript");
      // Between status lines (a reply being written, say) the turn its
      // transcript has open keeps it working, and the turn's end ends it.
      put(question_path, R"({"timestamp":"t","type":"event_msg","payload":{"type":"task_started"}})" "\n", true);
      codex_screen("\xe2\x80\xa2 Reading the parser");
      for (int i = 0; i < 600 && !session.busy(); ++i) { session.pump(); usleep(1000); }
      check(session.busy(), "codex: a turn open in its transcript is work while the status line is off");
      put(question_path, R"({"timestamp":"t","type":"event_msg","payload":{"type":"task_complete"}})" "\n", true);
      for (int i = 0; i < 600 && session.busy(); ++i) { session.pump(); usleep(1000); }
      check(!session.busy(), "codex: the turn's end in its transcript ends the work");
      codex_screen("\xe2\x80\xa2 Fixed. Do you want to proceed with the release? (y/n)");
      for (int i = 0; i < 50; ++i) { session.pump(); usleep(1000); }
      check(session.status() == LiveSession::Status::Idle, "codex: a reply that asks something leaves it idle");
      session.pty().write("\x1b[2J\x1b[H  $ touch probe.txt\r\n\r\n\xe2\x80\xba 1. Yes, proceed (y)\r\n  2. No (esc)\r\n");
      for (int i = 0; i < 200 && !session.needs_input(); ++i) { session.pump(); usleep(1000); }
      check(session.status() == LiveSession::Status::Waiting, "codex: its approval dialog needs you");
      session.pty().terminate();
      std::filesystem::remove(question_path);
    }
    {
      // codex's optional question: the call returns at once, the agent works
      // on, and the answer is a message quoting the question.
      const std::string async_path = dir + "rollout-async.jsonl";
      put(async_path, "{\"type\":\"session_meta\",\"payload\":{\"id\":\"async-fixture\",\"cwd\":\"" + project + "\"}}\n" +
          R"({"type":"response_item","payload":{"type":"function_call","name":"request_user_input_async","call_id":"later","arguments":"{\"questions\":[{\"title\":\"Pause the jobs?\",\"options\":[\"Pause them\",\"Continue later\"]}]}"}})" "\n"
          R"({"type":"response_item","payload":{"type":"function_call_output","call_id":"later","output":"{\"accepted\":true}"}})" "\n"
          R"({"type":"event_msg","payload":{"type":"task_complete"}})" "\n");
      App app;
      LiveSession session;
      LiveSession::Launch launch;
      launch.agent = "codex"; launch.cwd = project; launch.session_id = "async-fixture";
      launch.argv = {"/bin/sh", "-c", "stty raw -echo; printf READY; exec cat"};
      session.start(launch); session.set_geometry(80, 36);
      auto receive = [&](size_t count) {
        std::string bytes;
        for (int i = 0; i < 200 && bytes.size() < count; ++i) {
          session.pty().read_available(bytes); session.pty().flush_input(); usleep(1000);
        }
        return bytes;
      };
      check(receive(5) == "READY", "async question fixture is ready for exact input capture");
      session.pump();
      auto pane = make_session_pane(&session);
      pane->set_app(&app);
      Surface sf; sf.resize(80, 36);
      Painter painter(sf, Rect{0, 0, 80, 36});
      const auto row_of = [&](std::string_view needle) {
        pane->render(painter, true);
        for (int y = 0; y < sf.height(); ++y) {
          std::string row;
          for (int x = 0; x < sf.width(); ++x) text::encode(sf.at(x, y).cp, row);
          if (row.find(needle) != std::string::npos) return y;
        }
        return -1;
      };
      check(row_of("accepted") < 0, "an optional question hides codex's receipt");
      check(row_of("optional") >= 0 && row_of("Answer in your own words") >= 0,
            "an optional question outlives the turn that asked it and stays answerable");
      pane->on_key(KeyEvent{Key::Char, '1'});
      check(receive(1).empty(), "typing beside an optional question writes a message, not an answer");
      pane->on_key(KeyEvent{Key::Escape});
      receive(1);
      const int later_y = row_of("Continue later");
      check(later_y >= 0, "optional question options are visible in the live pane");
      // The reply as codex 0.159 writes it, user message and all.
      const auto reply_line = [](const std::string& text) {
        return R"({"type":"response_item","payload":{"type":"message","role":"user","content":[{"type":"input_text","text":)" +
               js::quote(text) + "}]}}\n";
      };
      const std::string later_reply =
          "<send_user_message_question_reply>\n"
          R"([{"answer":"Continue later","question":"Pause the jobs?","questionItemId":"[\"request_user_input_async\",\"later\",0]"}])"
          "\n</send_user_message_question_reply>";
      if (later_y >= 0) {
        pane->on_mouse(MouseEvent{MouseKind::Press, MouseButton::Left}, Point{8, later_y});
        const std::string sent = encode_paste(later_reply, false) + "\r";
        check(receive(sent.size()) == sent, "an optional answer is sent as codex's own reply envelope");
      }
      put(async_path, reply_line(later_reply), true);
      check(row_of("\xE2\x86\xB3 Continue later") >= 0 && row_of("Answer in your own words") < 0 &&
                row_of("questionItemId") < 0,
            "the envelope settles the card and shows the answer on it, not the envelope");

      put(async_path, R"({"type":"response_item","payload":{"type":"function_call","name":"request_user_input_async","call_id":"why","arguments":"{\"questions\":[{\"title\":\"Which benchmark matters?\"}]}"}})" "\n", true);
      const int own_y = row_of("Answer in your own words");
      check(own_y >= 0, "a question without options still offers an answer in the user's words");
      if (own_y >= 0) {
        pane->on_mouse(MouseEvent{MouseKind::Press, MouseButton::Left}, Point{8, own_y});
        check(receive(1).empty() && row_of("Answer to \"Which benchmark matters?\"") >= 0,
              "answering in one's own words opens the box for the answer instead of sending");
        for (char c : std::string("nightly")) pane->on_key(KeyEvent{Key::Char, char32_t(c)});
        pane->on_key(KeyEvent{Key::Enter});
        const std::string why_reply =
            "<send_user_message_question_reply>\n"
            R"([{"answer":"nightly","question":"Which benchmark matters?","questionItemId":"[\"request_user_input_async\",\"why\",0]"}])"
            "\n</send_user_message_question_reply>";
        const std::string sent = encode_paste(why_reply, false) + "\r";
        check(receive(sent.size()) == sent, "the answer in one's own words goes in the envelope");
        put(async_path, reply_line(why_reply), true);
        check(row_of("\xE2\x86\xB3 nightly") >= 0, "the own-words answer lands on its card");
      }

      // An older codex quoted the question instead.
      put(async_path, R"({"type":"response_item","payload":{"type":"function_call","name":"request_user_input_async","call_id":"old","arguments":"{\"questions\":[{\"title\":\"Keep the cache?\",\"options\":[\"Keep it\",\"Drop it\"]}]}"}})" "\n", true);
      put(async_path, reply_line("> Keep the cache?\n\nDrop it"), true);
      check(row_of("\xE2\x86\xB3 Drop it") >= 0, "a quoted reply still settles its card");

      put(async_path, R"({"type":"response_item","payload":{"type":"function_call","name":"request_user_input_async","call_id":"skip","arguments":"{\"questions\":[{\"title\":\"Rename the module?\"}]}"}})" "\n", true);
      check(row_of("Answer in your own words") >= 0, "a new optional question is open");
      put(async_path, R"({"type":"response_item","payload":{"type":"message","role":"user","content":[{"type":"input_text","text":"never mind, ship it"}]}})" "\n", true);
      check(row_of("not answered") >= 0 && row_of("Answer in your own words") < 0,
            "an unrelated message moves past an open optional question");
      session.pty().terminate();
      std::filesystem::remove(async_path);
    }
    {
      LiveSession resumed;
      LiveSession::Launch launch;
      launch.agent = "codex"; launch.cwd = other; launch.session_id = sid;
      launch.argv = {"/bin/cat"};
      resumed.start(launch); resumed.set_geometry(80, 24); resumed.pump();
      check(resumed.transcript() == path, "resume links the original rollout despite a different launch cwd");
      resumed.pty().terminate();
    }
    {
      App app;
      check(app.spawn_continuation("codex", sid, project, false), "prepare a resume");
      auto* selected = app.selected_live();
      check(app.spawn_continuation("codex", sid, project, false) &&
                app.live_sessions().size() == 1 && app.selected_live() == selected,
            "resuming an already running chat focuses its existing session");
      check(selected->session_id() == sid, "a prepared resume keeps its known id");
      // No frame is drawn: this verifies launch routing without invoking Codex.
    }
    {
      App app;
      LiveSession stopped;
      LiveSession::Launch launch;
      launch.agent = "codex"; launch.cwd = project; launch.session_id = sid;
      launch.argv = {base + "/missing-agent"};
      stopped.start(launch); stopped.set_geometry(80, 24);
      auto pane = make_session_pane(&stopped);
      pane->set_app(&app);
      bool resumable = false;
      for (const auto& item : pane->context_menu({}))
        if (item.action == "resume_self") resumable = item.enabled;
      check(resumable, "an exited session exposes an enabled resume action");
      pane->on_action("resume_self");
      check(app.live_sessions().size() == 1 && app.selected_live()->session_id() == sid,
            "an exited session resumes with its original identity");
    }
    {
      LiveSession first, second;
      LiveSession::Launch launch;
      launch.agent = "codex"; launch.cwd = project;
      const std::string a = dir + "rollout-a.jsonl", b = dir + "rollout-b.jsonl";
      launch.argv = {"/bin/sh", "-c", "exec 3< '" + a + "'; exec cat"};
      first.start(launch);
      launch.argv = {"/bin/sh", "-c", "exec 3< '" + b + "'; exec cat"};
      second.start(launch);
      put(a, "{\"type\":\"session_meta\",\"payload\":{\"id\":\"a\",\"cwd\":\"" + project + "\"}}\n");
      put(b, "{\"type\":\"session_meta\",\"payload\":{\"id\":\"b\",\"cwd\":\"" + project + "\"}}\n");
      std::filesystem::last_write_time(b, std::filesystem::file_time_type::clock::now() +
                                           std::chrono::seconds(10));
      first.set_geometry(80, 24); second.set_geometry(80, 24);
      for (int i = 0; i < 150 && (first.transcript().empty() || second.transcript().empty()); ++i) {
        first.pump(); second.pump(); usleep(10000);
      }
      check(first.transcript() == a && second.transcript() == b,
            "simultaneous chats link only to the transcript their own process opened");
      first.pty().terminate(); second.pty().terminate();
    }
    // Keep subsequent UI tests free from auto-selected stored sessions.
    {
      // Open/resume through the actual list and application, with a dummy
      // executable so no provider, credentials or real agent is involved.
      const std::string bin = base + "/bin";
      put(bin + "/codex", "#!/bin/sh\nexec /bin/cat\n");
      std::filesystem::permissions(bin + "/codex", std::filesystem::perms::owner_all);
      const char* path_env = getenv("PATH");
      EnvScope search_path("PATH", bin + ":" + (path_env ? path_env : "/bin"));
      std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() +
                                               std::chrono::seconds(30));
      App app;
      app.select_stored(path);
      app.store().set_custom_name("codex", sid, "Resume fixture");
      Surface sf; sf.resize(120, 32);
      app.draw(sf);
      check(app.live_sessions().empty(), "initial preview does not start an agent");
      auto list = make_chat_list();
      list->set_app(&app);
      Painter painter(sf, Rect{0, 0, 36, 18});
      list->render(painter, true);
      list->on_key(KeyEvent{Key::Down});
      check(app.live_sessions().empty(), "arrow browsing leaves saved chats stopped");
      list->on_key(KeyEvent{Key::Home});
      list->render(painter, true);
      MouseEvent modified_click{MouseKind::Press, MouseButton::Left};
      modified_click.ctrl = true;
      list->on_mouse(modified_click, Point{4, 0});
      check(app.live_sessions().empty(), "multi-select clicks do not start an agent");
      list->on_mouse(MouseEvent{MouseKind::Press, MouseButton::Left}, Point{4, 0});
      LiveSession* resumed = app.selected_live();
      check(resumed && resumed->session_id() == sid && app.live_sessions().size() == 1,
            "one click opens and resumes the chosen saved chat");
      if (resumed) {
        app.draw(sf); app.service(); app.draw(sf);
        check(app.session_title(*resumed) == "Resume fixture", "resuming retains the conversation title");
        const pid_t original_pid = resumed->pty().pid();
        list->render(painter, true);
        // The status word sits at the far right of the row.
        list->on_mouse(MouseEvent{MouseKind::Press, MouseButton::Left}, Point{30, 0});
        app.draw(sf);
        check(resumed->pty().pid() == original_pid && app.live_sessions().size() == 1,
              "clicking a row's status focuses the same chat without a duplicate process");
        app.focus_session(resumed);
        app.feed(InputEvent{InputEvent::Type::Paste, {}, {}, "PRESERVED_RESTART_DRAFT"});
        resumed->pty().terminate();
        for (int i = 0; i < 200 && !resumed->exited(); ++i) { app.service(); usleep(1000); }
        check(resumed->exited(), "the restart fixture has exited");
        check(app.open_selected_chat(), "opening an exited chat prepares its restart");
        app.draw(sf); app.service(); app.draw(sf);
        check(app.selected_live() == resumed && app.live_sessions().size() == 1 &&
                  resumed->pty().pid() != original_pid && !resumed->exited(),
              "an exited chat restarts in place without duplicate rows");
        check(screen(sf).find("PRESERVED_RESTART_DRAFT") != std::string::npos,
              "automatic restart preserves the unsent draft");
        resumed->pty().terminate();
      }
      // Enter works from a saved preview as well as from the list.
      app.select_stored(legacy);
      auto preview = make_chat_view();
      preview->set_app(&app);
      check(preview->on_key(KeyEvent{Key::Enter}) && app.selected_live() &&
                app.selected_live()->session_id() == "legacy",
            "Enter in a saved preview resumes that conversation");

      // ↑ on an empty prompt box recalls what was sent, out of the transcript.
      {
        const std::string hist = base + "/.claude/projects/fixture/hist-fixture.jsonl";
        put(hist,
            R"({"type":"user","message":{"role":"user","content":"first thing"}})" "\n"
            R"({"type":"assistant","message":{"role":"assistant","content":[{"type":"text","text":"ok"}]}})" "\n"
            R"({"type":"user","message":{"role":"user","content":"second\nline two"}})" "\n"
            R"({"type":"assistant","message":{"role":"assistant","content":[{"type":"text","text":"done"}]}})" "\n");
        LiveSession session;
        LiveSession::Launch launch;
        launch.agent = "claude";
        launch.cwd = project;
        launch.session_id = "hist-fixture";
        launch.argv = {"/bin/sh", "-c", "stty raw -echo; exec cat"};
        session.start(launch);
        session.set_geometry(80, 30);
        session.pump();
        auto pane = make_session_pane(&session);
        pane->set_app(&app);
        Surface psf;
        psf.resize(80, 30);
        Painter pp(psf, Rect{0, 0, 80, 30});
        pane->render(pp, true);
        const auto box = [&] {
          pane->render(pp, true);
          return screen(psf);
        };
        const auto tap = [&](Key k, char32_t ch = 0) {
          KeyEvent e;
          e.key = k;
          e.ch = ch;
          pane->on_key(e);
        };
        tap(Key::Up);
        check(box().find("\xE2\x80\xBA second") != std::string::npos, "history: ↑ recalls the newest message");
        check(box().find("line two") != std::string::npos, "history: a multi-line message comes back whole");
        tap(Key::Up);
        std::string sc = box();
        check(sc.find("\xE2\x80\xBA first thing") != std::string::npos &&
                  sc.find("\xE2\x80\xBA second") == std::string::npos,
              "history: ↑ again steps older");
        tap(Key::Up);
        check(box().find("\xE2\x80\xBA first thing") != std::string::npos, "history: the oldest holds");
        tap(Key::Down);
        check(box().find("\xE2\x80\xBA second") != std::string::npos, "history: ↓ steps newer");
        tap(Key::Down);
        check(box().find("\xE2\x80\xBA second") == std::string::npos, "history: ↓ past the newest empties the box");
        // Editing a recalled message turns it into a draft: ↑ stops stepping.
        tap(Key::Up);
        tap(Key::Char, 'X');
        tap(Key::Up);
        sc = box();
        check(sc.find("line twoX") != std::string::npos &&
                  sc.find("\xE2\x80\xBA first thing") == std::string::npos,
              "history: an edited recall is a draft, not a rung");
        session.pty().terminate();
      }

      // Claude's /btw panel: shown at the foot of the chat, its keys passed
      // through, and closed before a message is typed.
      {
        const std::string hist = base + "/.claude/projects/fixture/btw-fixture.jsonl";
        put(hist, R"({"type":"user","message":{"role":"user","content":"say PINEAPPLE"}})" "\n");
        const std::string got = base + "/btw-keys";
        std::string rule;
        for (int i = 0; i < 70; i++) rule += "\xE2\x96\x94";
        const std::string screen_bytes =
            "\xE2\x97\x8F PINEAPPLE\r\n" + rule + "\r\n\r\n    \x1b[1m/btw what fruit\x1b[0m\r\n\r\n"
            "      It was a SIDEANSWER pineapple.\r\n\r\n    \x1b[38;5;246m\xE2\x86\x91/\xE2\x86\x93 to scroll"
            " \xC2\xB7 Esc to close\x1b[39m\r\n";
        put(base + "/btw-screen", screen_bytes);
        LiveSession session;
        LiveSession::Launch launch;
        launch.agent = "claude";
        launch.cwd = project;
        launch.session_id = "btw-fixture";
        launch.argv = {"/bin/sh", "-c", "cat '" + base + "/btw-screen'; stty raw -echo; exec cat > '" + got + "'"};
        session.start(launch);
        session.set_geometry(80, 30);
        auto pane = make_session_pane(&session);
        pane->set_app(&app);
        Surface psf;
        psf.resize(80, 30);
        Painter pp(psf, Rect{0, 0, 80, 30});
        for (int i = 0; i < 200; i++) {
          session.pump();
          pane->render(pp, true);
          if (screen(psf).find("SIDEANSWER") != std::string::npos) break;
          usleep(2000);
        }
        std::string sc = screen(psf);
        check(sc.find("\xE2\x97\x87 btw  what fruit") != std::string::npos &&
                  sc.find("It was a SIDEANSWER pineapple.") != std::string::npos,
              "btw: the side answer shows at the foot of the chat");
        check(sc.find("not saved in the chat") != std::string::npos, "btw: its keys, and that it is not kept");
        const auto keys = [&]() {
          std::string bytes;
          if (FILE* f = fopen(got.c_str(), "rb")) {
            char b[256];
            size_t n;
            while ((n = fread(b, 1, sizeof b, f)) > 0) bytes.append(b, n);
            fclose(f);
          }
          return bytes;
        };
        const auto settle = [&](size_t want) {
          // Each paced step waits for the agent to redraw, or 1.5 s; this
          // one never redraws.
          for (int i = 0; i < 2500 && keys().size() < want; i++) {
            session.pump();
            usleep(2000);
          }
        };
        KeyEvent down;
        down.key = Key::Down;
        pane->on_key(down);
        settle(3);
        check(keys() == "\x1b[B", "btw: ↓ scrolls claude's panel");
        // A message while the panel is up closes it first, then goes.
        pane->on_paste("next question");
        KeyEvent enter;
        enter.key = Key::Enter;
        pane->on_key(enter);
        settle(3 + 1 + 13);
        const std::string k = keys();
        check(k.starts_with("\x1b[B\x1b") && k.find("next question") != std::string::npos &&
                  k.find("next question") > 3,
              "btw: a message closes the panel with Esc before it is typed");
        session.pty().terminate();
      }

      // Sub-projects: in a folder of their own, a chat's folder decides;
      // in the project's own folder, only the chats put there.
      {
        std::filesystem::create_directories(project + "/web/src");
        const std::string dir = base + "/.claude/projects/subfix";
        const auto chat = [&](const std::string& id, const std::string& cwd, const std::string& said) {
          put(dir + "/" + id + ".jsonl", "{\"type\":\"user\",\"cwd\":\"" + cwd +
                                             "\",\"message\":{\"role\":\"user\",\"content\":\"" + said + "\"}}\n");
        };
        chat("sub-web", project + "/web/src", "web work");
        chat("sub-root", project, "root work");
        Store& st = app.store();
        st.scan();
        const auto find = [&](const std::string& id) -> const SessionRef* {
          for (const auto& pr : st.projects())
            if (pr.path == project)
              for (const auto& ss : pr.sessions)
                if (ss.id == id) return &ss;
          return nullptr;
        };
        check(!find("sub-web") && find("sub-root"), "sub: a chat under an untracked folder is not listed");
        check(st.add_subproject(project, "web", project + "/web"), "sub: one in a folder of its own");
        check(find("sub-web") && find("sub-web")->sub == "web", "sub: its folder's chats, however deep, join it");
        check(find("sub-root") && find("sub-root")->sub.empty(), "sub: the root's chats stay the project's");
        check(!st.add_subproject(project, "web", project), "sub: names are unique in a project");
        check(!st.add_subproject(project, "out", base), "sub: only inside the project");
        check(st.add_subproject(project, "notes", project), "sub: one in the project's own folder");
        check(find("sub-root")->sub.empty(), "sub: a same-folder sub-project claims nothing by folder");
        st.assign_sub(project, "claude", "sub-root", "notes");
        check(find("sub-root")->sub == "notes" && st.sub_of_path(find("sub-root")->path) == "notes",
              "sub: a chat put in one belongs to it");
        st.assign_sub(project, "claude", "sub-web", "");
        check(find("sub-web")->sub.empty(), "sub: taken out, even of the one its folder is in");
        st.assign_sub(project, "claude", "sub-web", "web");

        // The filter: the sub-project narrows every tab to its chats.
        int pi = -1;
        for (size_t i = 0; i < st.projects().size(); i++)
          if (st.projects()[i].path == project) pi = int(i);
        app.select_project(pi);
        app.select_sub("web");
        check(app.current_sub() && app.selected_cwd() == project + "/web", "sub: new chats run in its folder");
        const auto only = app.filtered_projects();
        check(only.size() == 1 && only[0].sessions.size() == 1 && only[0].sessions[0].id == "sub-web",
              "sub: the filter holds its chats only");
        check(app.in_filter(only[0].name, find("sub-web")->path) &&
                  !app.in_filter(only[0].name, find("sub-root")->path),
              "sub: in_filter agrees");
        check(app.view_filter().label.find("web") != std::string::npos, "sub: the filter names it");

        // Kept across a restart, and gone with the folder's own removal.
        Store again;
        again.scan();
        bool kept = false;
        for (const auto& pr : again.projects())
          if (pr.path == project)
            for (const auto& ss : pr.sessions) kept |= ss.id == "sub-root" && ss.sub == "notes";
        check(kept, "sub: sub-projects and their chats are kept");
        check(st.remove_subproject(project, "web"), "sub: removed");
        check(!find("sub-web"), "sub: its folder's chats go with it");
        st.remove_subproject(project, "notes");
        app.select_project(pi);
        std::filesystem::remove_all(dir);
        st.scan();
      }

      // Ctrl+K: any chat by name. A running one is focused, not started again.
      {
        const auto type = [&](std::string_view t) {
          for (char c : t) app.feed(InputEvent{InputEvent::Type::Key, KeyEvent{Key::Char, char32_t(c)}, {}, {}});
        };
        const auto press = [&](Key k) { app.feed(InputEvent{InputEvent::Type::Key, KeyEvent{k}, {}, {}}); };
        const size_t running = app.live_sessions().size();
        app.open_switcher();
        app.draw(sf);
        check(screen(sf).find("Go to chat") != std::string::npos && screen(sf).find("Resume fixture") != std::string::npos,
              "the switcher lists chats by title");
        type("resume fix");
        press(Key::Enter);
        app.draw(sf);
        check(app.selected_live() && app.selected_live()->session_id() == sid &&
                  app.live_sessions().size() == running,
              "the switcher opens a running chat without starting another");
        check(screen(sf).find("Go to chat") == std::string::npos, "and closes");

        // F7: an agent, or any command, then where. Backspace on an empty
        // query steps back with the command still typed.
        app.open_new_agent();
        type("/bin/cat");
        app.draw(sf);
        check(screen(sf).find("Run \xE2\x80\x9C/bin/cat\xE2\x80\x9D") != std::string::npos,
              "new agent: what is typed can be run as it is");
        press(Key::Enter);
        app.draw(sf);
        check(screen(sf).find("Run /bin/cat in") != std::string::npos, "new agent: then where");
        press(Key::Backspace);
        app.draw(sf);
        check(screen(sf).find("New agent") != std::string::npos && screen(sf).find("\xE2\x80\xBA /bin/cat") != std::string::npos,
              "new agent: Backspace goes back with the command kept");
        press(Key::Enter);
        press(Key::Enter);  // the selected folder is first
        app.draw(sf);
        check(app.live_sessions().size() == running + 1, "new agent: Enter runs it in the folder");
        std::string recent;
        if (FILE* f = fopen((base + "/config/mico/commands").c_str(), "r")) {
          char line[256];
          if (fgets(line, sizeof line, f)) recent = line;
          fclose(f);
        }
        check(recent == "/bin/cat\n", "new agent: the command is remembered");
        for (LiveSession* s : app.live_sessions()) s->pty().terminate();

        // Tracking a folder: browse into it, Enter adds it.
        std::filesystem::create_directories(base + "/browse/inner");
        app.pick_folder();
        type("browse/");
        app.draw(sf);
        check(screen(sf).find("~/browse/inner/") != std::string::npos, "folder: browsing lists what is inside");
        press(Key::Enter);
        const auto& folders = app.store().folders();
        check(std::find(folders.begin(), folders.end(), base + "/browse/inner") != folders.end(),
              "folder: Enter tracks the one chosen");
      }
    }
    {
      // Archiving acts on the row that was right-clicked, starts nothing, and
      // takes the chat out of the list — also when its agent was running.
      const char* path_env = getenv("PATH");
      EnvScope search_path("PATH", base + "/bin:" + (path_env ? path_env : "/bin"));
      App app;
      app.select_stored(path);  // the selection is some other chat
      Surface sf; sf.resize(120, 32);
      app.draw(sf);
      auto list = make_chat_list();
      list->set_app(&app);
      Painter painter(sf, Rect{0, 0, 36, 18});
      const auto row_showing = [&](const std::string& text) {
        list->render(painter, true);
        for (int y = 0; y < 18; y++) {
          std::string row;
          for (int x = 0; x < 36; x++) text::encode(sf.at(x, y).cp, row);
          if (row.find(text) != std::string::npos) return y;
        }
        return -1;
      };
      const int y = row_showing("legacy");
      check(y >= 0, "the archive fixture chat is listed");
      list->context_menu(Point{4, y});
      list->on_action("archive");
      app.draw(sf);
      check(app.live_sessions().empty(), "archiving a chat from its menu does not start its agent");
      check(app.store().archived("codex", "legacy") && row_showing("legacy") < 0,
            "an archived chat leaves the list");
      app.store().set_archived("codex", "legacy", false);

      check(app.spawn_continuation("codex", "legacy", project, false) &&
                app.live_sessions().size() == 1,
            "the archive fixture chat is running");
      for (int i = 0; i < 20; i++) { app.service(); app.draw(sf); usleep(2000); }
      // A running chat is listed under its session title, marked "ready".
      const int live_y = row_showing("ready");
      list->context_menu(Point{4, live_y});
      list->on_action("archive");
      app.draw(sf);
      check(live_y >= 0 && app.live_sessions().empty() && row_showing("ready") < 0 &&
                row_showing("legacy") < 0,
            "archiving an idle running chat stops its agent and hides it");
      app.store().set_archived("codex", "legacy", false);
    }
    std::filesystem::remove_all(base + "/.codex");
  }

  {
    // Ctrl+C with text highlighted copies it. It must not reach the agent as
    // an interrupt, or detach mico.
    const std::string got = base + "/ctrl-c-received";
    App app;
    Surface sf; sf.resize(120, 32);
    app.spawn_raw({"/bin/sh", "-c", "printf 'SELECTME here'; stty raw -echo; exec cat > '" + got + "'"}, project);
    int sx = -1, sy = -1;
    for (int i = 0; i < 200 && sx < 0; i++) {
      app.service(); app.draw(sf); usleep(5000);
      const std::string all = screen(sf);
      const size_t at = all.find("SELECTME");
      if (at == std::string::npos) continue;
      sy = int(std::count(all.begin(), all.begin() + long(at), '\n'));
      const size_t line = all.rfind('\n', at);
      sx = text::str_width(all.substr(line == std::string::npos ? 0 : line + 1, at - (line == std::string::npos ? 0 : line + 1)));
    }
    check(sx >= 0, "the selection fixture text is on screen");
    const auto mouse = [&](MouseKind kind, int x) {
      MouseEvent m{kind, MouseButton::Left, Point{x, sy}};
      app.feed(InputEvent{InputEvent::Type::Mouse, {}, m, {}});
    };
    mouse(MouseKind::Press, sx);
    mouse(MouseKind::Drag, sx + 4);
    mouse(MouseKind::Drag, sx + 7);
    mouse(MouseKind::Release, sx + 7);
    app.draw(sf);
    app.take_clipboard();  // the copy the release made
    app.feed(InputEvent{InputEvent::Type::Key, KeyEvent{Key::Char, 'c', true}, {}, {}});
    app.draw(sf);
    const std::string clip = app.take_clipboard();
    for (int i = 0; i < 20; i++) { app.service(); usleep(2000); }
    std::string received;
    { std::ifstream f(got); received.assign(std::istreambuf_iterator<char>(f), {}); }
    check(clip.find("SELECTME") != std::string::npos, "Ctrl+C over a selection copies it");
    check(received.find('\x03') == std::string::npos, "Ctrl+C over a selection does not interrupt the agent");
    check(app.running(), "Ctrl+C over a selection does not detach mico");
    for (auto* s : app.live_sessions()) s->pty().terminate();
  }

  {
    // The editing keys, as the terminal's bytes, act on the draft and never
    // reach the agent: Ctrl+Backspace deletes a word, Ctrl+Z brings it back,
    // Ctrl+A then Ctrl+X cuts the whole draft to the clipboard.
    const std::string got = base + "/edit-keys-received";
    App app;
    Surface sf; sf.resize(120, 32);
    app.spawn_raw({"/bin/sh", "-c", "stty raw -echo; exec cat > '" + got + "'"}, project);
    for (int i = 0; i < 20; i++) { app.service(); app.draw(sf); usleep(2000); }
    app.focus_session(app.selected_live());
    const auto keys = [&](const char* bytes) {
      InputDecoder d;
      d.feed(bytes);
      while (auto e = d.next()) app.feed(*e);
      app.draw(sf);
    };
    app.feed(InputEvent{InputEvent::Type::Paste, {}, {}, "DRAFT_ONE DRAFT_TWO"});
    keys("\x08");  // Ctrl+Backspace
    check(screen(sf).find("DRAFT_ONE") != std::string::npos &&
              screen(sf).find("DRAFT_T") == std::string::npos,
          "Ctrl+Backspace deletes the word before the cursor");
    keys("\x1a");  // Ctrl+Z
    check(screen(sf).find("DRAFT_ONE DRAFT_TWO") != std::string::npos, "Ctrl+Z undoes it");
    keys("\x1b\x7f");  // Alt+Backspace
    check(screen(sf).find("DRAFT_T") == std::string::npos, "Alt+Backspace deletes a word too");
    keys("\x1a");  // Ctrl+Z
    check(screen(sf).find("DRAFT_ONE DRAFT_TWO") != std::string::npos, "undo again");
    keys("\x19");  // Ctrl+Y
    check(screen(sf).find("DRAFT_ONE") != std::string::npos &&
              screen(sf).find("DRAFT_T") == std::string::npos,
          "Ctrl+Y redoes");
    keys("\x19");  // nothing left to redo: still not the agent's
    keys("\x1a");
    app.take_clipboard();
    keys("\x01\x18");  // Ctrl+A, Ctrl+X
    const std::string clip = app.take_clipboard();
    check(clip == "DRAFT_ONE DRAFT_TWO" && screen(sf).find("DRAFT_ONE") == std::string::npos,
          "Ctrl+A then Ctrl+X cuts the draft to the clipboard");
    for (int i = 0; i < 20; i++) { app.service(); usleep(2000); }
    std::string received;
    { std::ifstream f(got); received.assign(std::istreambuf_iterator<char>(f), {}); }
    check(received.empty(), "editing keys never reach the agent");
    if (!received.empty()) {
      std::string hex;
      for (unsigned char c : received) { char b[8]; snprintf(b, sizeof b, "%02x ", c); hex += b; }
      fprintf(stderr, "received: %s\n", hex.c_str());
    }
    for (auto* s : app.live_sessions()) s->pty().terminate();
  }

  {
    App app;
    Surface sf; sf.resize(120, 32);
    app.spawn_raw({"/bin/cat"}, project); app.draw(sf);
    LiveSession* a = app.selected_live();
    app.focus_session(a);
    app.feed(InputEvent{InputEvent::Type::Paste, {}, {}, "ALPHA_DRAFT"});
    app.spawn_raw({"/bin/cat"}, other); app.draw(sf);
    LiveSession* b = app.selected_live();
    app.focus_session(b);
    app.feed(InputEvent{InputEvent::Type::Paste, {}, {}, "BETA_DRAFT"});
    app.select_live(a); app.draw(sf);
    check(screen(sf).find("ALPHA_DRAFT") != std::string::npos &&
              screen(sf).find("BETA_DRAFT") == std::string::npos,
          "switching chats preserves each draft independently");
    app.open_tab(1); app.draw(sf); app.open_tab(0); app.draw(sf);
    check(screen(sf).find("ALPHA_DRAFT") != std::string::npos, "switching tabs preserves the chat draft");

    app.focus_session(a);
    app.select_live(b);
    // No draw/service between selection and paste, as with one socket read.
    app.feed(InputEvent{InputEvent::Type::Paste, {}, {}, "_SECOND_ONLY"});
    app.select_live(a); app.draw(sf);
    check(screen(sf).find("SECOND_ONLY") == std::string::npos, "batched paste cannot reach the previously selected chat");
    app.select_live(b); app.draw(sf);
    check(screen(sf).find("BETA_DRAFT_SECOND_ONLY") != std::string::npos,
          "batched paste reaches the newly selected chat");

    int project_index = -1, other_index = -1;
    for (size_t i = 0; i < app.store().projects().size(); ++i) {
      if (app.store().projects()[i].path == project) project_index = int(i);
      if (app.store().projects()[i].path == other) other_index = int(i);
    }
    app.select_project(other_index); app.draw(sf);
    app.select_project(project_index); app.draw(sf);
    app.focus_session(a);
    app.feed(InputEvent{InputEvent::Type::Paste, {}, {}, "_PROJECT_ONLY"});
    app.draw(sf);
    check(screen(sf).find("ALPHA_DRAFT_PROJECT_ONLY") != std::string::npos,
          "switching projects rebuilds the input target");
    app.select_live(b); app.draw(sf);
    check(screen(sf).find("PROJECT_ONLY") == std::string::npos,
          "project switching never pastes into a different project");
    a->pty().terminate(); b->pty().terminate();
  }

  {
    // Search across chats, as a user drives it: / opens the Search tab,
    // typing searches, Enter opens the chat at the match (far back in a long
    // transcript) with the find bar counting, and Esc closes the bar.
    const std::string dir = base + "/.claude/projects/search-fixture";
    std::filesystem::create_directories(dir);
    std::string body = R"({"type":"ai-title","aiTitle":"Search fixture chat"})" "\n";
    const auto line = [&](const char* role, const std::string& text) {
      body += std::string("{\"type\":\"") + role + "\",\"cwd\":\"" + project +
              "\",\"message\":{\"role\":\"" + role + "\",\"content\":[{\"type\":\"text\",\"text\":\"" +
              text + "\"}]}}\n";
    };
    line("user", "first question");
    line("assistant", "the UNIQUEWORD is here");
    for (int i = 0; i < 200; i++) {
      line("user", "filler question " + std::to_string(i));
      line("assistant", "filler answer " + std::to_string(i));
    }
    put(dir + "/11111111-2222-3333-4444-555555555555.jsonl", body);

    App app;
    Surface sf; sf.resize(120, 32);
    app.draw(sf);
    const auto key = [&](KeyEvent k) { app.feed(InputEvent{InputEvent::Type::Key, k, {}, {}}); };
    key(KeyEvent{Key::Char, '/'});
    for (char c : std::string("uniqueword")) key(KeyEvent{Key::Char, char32_t(c)});
    for (int i = 0; i < 200; i++) app.service();
    app.draw(sf);
    std::string shown = screen(sf);
    check(shown.find("Search fixture chat") != std::string::npos &&
              shown.find("UNIQUEWORD is here") != std::string::npos,
          "/ and typing search every chat and list the match");
    key(KeyEvent{Key::Enter});
    app.draw(sf);
    app.draw(sf);
    shown = screen(sf);
    check(shown.find("UNIQUEWORD is here") != std::string::npos &&
              shown.find("filler answer 199") == std::string::npos,
          "Enter opens the chat scrolled back to the match");
    check(shown.find("Find") != std::string::npos && shown.find("1 of 1") != std::string::npos,
          "the opened chat has its find bar counting the match");
    key(KeyEvent{Key::Escape});
    app.draw(sf);
    check(screen(sf).find("1 of 1") == std::string::npos, "Esc closes the find bar");
    std::filesystem::remove_all(dir);
  }

  {
    // The Tools tab, as a user sees it: a chat's slow build is counted as a
    // build, listed among the slowest calls, and Enter opens the chat there.
    const std::string dir = base + "/.claude/projects/tools-fixture";
    std::filesystem::create_directories(dir);
    const auto iso = [](int64_t unix_s) {
      char b[40];
      const time_t t = time_t(unix_s);
      tm g{};
      gmtime_r(&t, &g);
      strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%S.000Z", &g);
      return std::string(b);
    };
    const int64_t t0 = int64_t(time(nullptr)) - 3600;
    std::string body = "{\"type\":\"user\",\"cwd\":\"" + project +
                       "\",\"timestamp\":\"" + iso(t0) + "\",\"message\":{\"role\":\"user\",\"content\":\"build it\"}}\n";
    const auto call = [&](const std::string& id, const std::string& cmd, int64_t at, int64_t took) {
      body += "{\"type\":\"assistant\",\"timestamp\":\"" + iso(at) +
              "\",\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"" + id +
              "\",\"name\":\"Bash\",\"input\":{\"command\":\"" + cmd + "\"}}]}}\n";
      body += "{\"type\":\"user\",\"timestamp\":\"" + iso(at + took) +
              "\",\"message\":{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"" + id +
              "\",\"content\":\"ok\"}]}}\n";
    };
    call("b1", "cd /x && cmake --build build -j8 SLOWBUILD", t0 + 10, 300);
    for (int i = 0; i < 60; i++) call("r" + std::to_string(i), "grep -n needle file" + std::to_string(i), t0 + 400 + i * 5, 1);
    put(dir + "/22222222-3333-4444-5555-666666666666.jsonl", body);

    App app;
    Surface sf; sf.resize(130, 40);
    app.open_tab(3);
    app.draw(sf);
    app.draw(sf);
    std::string shown = screen(sf);
    check(shown.find("build") != std::string::npos && shown.find("cmake --build") != std::string::npos &&
              shown.find("5m 00s") != std::string::npos,
          "the Tools tab counts a chat's build and its time");
    check(shown.find("SLOWBUILD") != std::string::npos, "the slowest call is listed first");
    app.feed(InputEvent{InputEvent::Type::Key, KeyEvent{Key::Enter}, {}, {}});
    app.draw(sf);
    app.draw(sf);
    shown = screen(sf);
    check(shown.find("slowest calls") == std::string::npos && shown.find("SLOWBUILD") != std::string::npos &&
              shown.find("file59") == std::string::npos,
          "Enter opens the chat at the call");
    std::filesystem::remove_all(dir);
  }

  {
    // The chat list is newest first by use, and holds still while in use.
    const std::string dir = base + "/.claude/projects/order-fixture";
    std::filesystem::create_directories(dir);
    const auto chat = [&](const std::string& id, const std::string& title, int ahead) {
      const std::string f = dir + "/" + id + ".jsonl";
      put(f, "{\"type\":\"user\",\"cwd\":\"" + project + "\",\"message\":{\"role\":\"user\",\"content\":\"" + title +
                 "\"}}\n");
      std::filesystem::last_write_time(f, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(ahead));
      return f;
    };
    chat("44444444-0000-0000-0000-00000000000a", "ORDERALPHA", 500);
    const std::string beta = chat("44444444-0000-0000-0000-00000000000b", "ORDERBETA", 600);
    App app;
    Surface sf; sf.resize(120, 40);
    app.draw(sf);
    auto list = make_chat_list();
    list->set_app(&app);
    Painter painter(sf, Rect{0, 0, 40, 30});
    const auto above = [&](const char* a, const char* b) {
      const std::string sc = screen(sf);
      const size_t x = sc.find(a), y = sc.find(b);
      return x != std::string::npos && y != std::string::npos && x < y;
    };
    list->render(painter, false);
    check(above("ORDERBETA", "ORDERALPHA"), "chats are listed newest first");
    // ALPHA is used after BETA; while the list has the focus nothing moves.
    chat("44444444-0000-0000-0000-00000000000a", "ORDERALPHA", 700);
    app.store().scan();
    list->render(painter, true);
    check(above("ORDERBETA", "ORDERALPHA"), "the order holds while the chat list is in use");
    list->on_mouse(MouseEvent{MouseKind::WheelDown, MouseButton::None}, Point{2, 2});
    list->render(painter, false);
    check(above("ORDERBETA", "ORDERALPHA"), "and for a moment after it was touched");
    app.chat_order().held_until_ms = 0;
    list->render(painter, false);
    check(above("ORDERALPHA", "ORDERBETA"), "then it catches up");
    (void)beta;
    std::filesystem::remove_all(dir);
    app.store().scan();
  }

  {
    // The Diff tab: a chat's edit and the file it created are listed by file,
    // with the chat that made them and the diff, and Enter opens the chat at
    // the edit.
    const std::string dir = base + "/.claude/projects/diff-fixture";
    std::filesystem::create_directories(dir);
    const auto iso = [](int64_t unix_s) {
      char b[40];
      const time_t t = time_t(unix_s);
      tm g{};
      gmtime_r(&t, &g);
      strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%S.000Z", &g);
      return std::string(b);
    };
    const int64_t t0 = int64_t(time(nullptr)) - 3600;
    std::string body = "{\"type\":\"user\",\"cwd\":\"" + project + "\",\"timestamp\":\"" + iso(t0) +
                       "\",\"message\":{\"role\":\"user\",\"content\":\"tidy the parser\"}}\n";
    body += "{\"type\":\"assistant\",\"timestamp\":\"" + iso(t0 + 5) +
            "\",\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"w1\",\"name\":\"Write\",\"input\":{\"file_path\":\"" +
            project + "/src/notes.txt\"}}]}}\n";
    body += "{\"type\":\"user\",\"timestamp\":\"" + iso(t0 + 6) +
            "\",\"message\":{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"w1\",\"content\":\"ok\"}]},"
            "\"toolUseResult\":{\"type\":\"create\",\"filePath\":\"" + project +
            "/src/notes.txt\",\"content\":\"first note\\nsecond note\\n\",\"structuredPatch\":[]}}\n";
    body += "{\"type\":\"assistant\",\"timestamp\":\"" + iso(t0 + 10) +
            "\",\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"EDITMARK now the parser\"},{\"type\":\"tool_use\",\"id\":\"e1\",\"name\":\"Edit\",\"input\":{\"file_path\":\"" +
            project + "/src/parse.cpp\"}}]}}\n";
    body += "{\"type\":\"user\",\"timestamp\":\"" + iso(t0 + 11) +
            "\",\"message\":{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"e1\",\"content\":\"ok\"}]},"
            "\"toolUseResult\":{\"filePath\":\"" + project +
            "/src/parse.cpp\",\"structuredPatch\":[{\"oldStart\":40,\"oldLines\":2,\"newStart\":40,\"newLines\":2,"
            "\"lines\":[\" int parse() {\",\"-  return OLDCALL();\",\"+  return NEWCALL();\"]}]}}\n";
    for (int i = 0; i < 80; i++)
      body += "{\"type\":\"assistant\",\"timestamp\":\"" + iso(t0 + 20 + i) +
              "\",\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"filler " +
              std::to_string(i) + "\"}]}}\n";
    put(dir + "/33333333-4444-5555-6666-777777777777.jsonl", body);

    App app;
    Surface sf; sf.resize(130, 40);
    app.open_tab(4);
    app.draw(sf);
    app.draw(sf);
    std::string shown = screen(sf);
    check(shown.find("2 files changed") != std::string::npos && shown.find("src/parse.cpp") != std::string::npos &&
              shown.find("src/notes.txt new") != std::string::npos && shown.find("tidy the parser") != std::string::npos,
          "the Diff tab lists each file a chat changed, and the chat");
    check(shown.find("NEWCALL") != std::string::npos && shown.find("OLDCALL") != std::string::npos &&
              shown.find(" line 40 ") != std::string::npos,
          "the newest file's diff is shown under the list");
    app.feed(InputEvent{InputEvent::Type::Key, KeyEvent{Key::Down}, {}, {}});
    app.draw(sf);
    shown = screen(sf);
    check(shown.find("second note") != std::string::npos && shown.find("created") != std::string::npos,
          "choosing a file shows its changes: a new file, every line added");
    app.feed(InputEvent{InputEvent::Type::Key, KeyEvent{Key::Up}, {}, {}});
    app.feed(InputEvent{InputEvent::Type::Key, KeyEvent{Key::Enter}, {}, {}});
    app.draw(sf);
    app.draw(sf);
    shown = screen(sf);
    check(shown.find("files changed") == std::string::npos && shown.find("EDITMARK") != std::string::npos &&
              shown.find("filler 79") == std::string::npos,
          "Enter opens the chat at the edit");
    std::filesystem::remove_all(dir);
  }

  // Commentary and answers: a finished turn's steps and what the agent said
  // between them fold into one line above its answer, which has a bar of its
  // own; a turn still running shows everything.
  {
    const std::string path = base + "/turns.jsonl";
    const auto asst = [](const std::string& block, const char* stop) {
      return std::string(R"({"type":"assistant","message":{"role":"assistant","stop_reason":")") + stop +
             R"(","content":[)" + block + "]}}\n";
    };
    std::string body =
        R"({"type":"user","message":{"role":"user","content":"why is the build slow"}})" "\n" +
        asst(R"({"type":"text","text":"Let me time it COMMENTMARK."})", "tool_use") +
        asst(R"({"type":"tool_use","id":"t1","name":"Bash","input":{"command":"make TOOLMARK"}})", "tool_use") +
        R"({"type":"user","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"t1","content":"ok"}]}})" "\n" +
        asst(R"({"type":"thinking","thinking":"hmm"})", "end_turn") +
        asst(R"({"type":"text","text":"It spends ANSWERMARK minutes linking."})", "end_turn");
    put(path, body);
    ChatRenderer chat;
    chat.open(path, &claude_adapter());
    Surface sf; sf.resize(80, 24);
    Painter p(sf, Rect{0, 0, 80, 24});
    Theme theme; Filters filters;
    const auto row_of = [&](std::string_view what) {
      const std::string s = screen(sf);
      const size_t at = s.find(what);
      return at == std::string::npos ? -1 : int(std::count(s.begin(), s.begin() + long(at), '\n'));
    };
    const auto line = [&](int y) {
      const std::string s = screen(sf);
      size_t from = 0;
      for (int i = 0; i < y; i++) from = s.find('\n', from) + 1;
      return s.substr(from, s.find('\n', from) - from);
    };
    chat.render(p, theme, filters);
    std::string shown = screen(sf);
    check(shown.find("\xE2\x96\xB8 1 step \xC2\xB7 1 comment") != std::string::npos &&
              shown.find("COMMENTMARK") == std::string::npos && shown.find("TOOLMARK") == std::string::npos,
          "a finished turn's steps fold into one line");
    const int ans = row_of("ANSWERMARK");
    check(ans >= 0 && line(ans).find("\xE2\x96\x8E") != std::string::npos && row_of("1 step") < ans,
          "the answer follows the fold, with a bar of its own");

    // A click opens the fold, where it was.
    const int fold = row_of("1 step");
    chat.on_mouse(MouseEvent{MouseKind::Press, MouseButton::Left, Point{4, fold}}, Point{4, fold});
    chat.on_mouse(MouseEvent{MouseKind::Release, MouseButton::Left, Point{4, fold}}, Point{4, fold});
    chat.render(p, theme, filters);
    shown = screen(sf);
    check(shown.find("\xE2\x96\xBE 1 step") != std::string::npos && shown.find("COMMENTMARK") != std::string::npos &&
              shown.find("TOOLMARK") != std::string::npos && row_of("1 step") == fold,
          "clicking the fold shows the steps");
    chat.on_mouse(MouseEvent{MouseKind::Press, MouseButton::Left, Point{4, fold}}, Point{4, fold});
    chat.on_mouse(MouseEvent{MouseKind::Release, MouseButton::Left, Point{4, fold}}, Point{4, fold});

    // A new turn, still running: everything shows, nothing is an answer yet.
    put(path,
        R"({"type":"user","message":{"role":"user","content":"then fix it"}})" "\n" +
            asst(R"({"type":"text","text":"Trying LIVEMARK first."})", "tool_use") +
            asst(R"({"type":"tool_use","id":"t2","name":"Bash","input":{"command":"make LIVETOOL"}})", "tool_use"),
        true);
    chat.poll_growth();
    chat.render(p, theme, filters);
    shown = screen(sf);
    const int live = row_of("LIVEMARK");
    check(live >= 0 && shown.find("LIVETOOL") != std::string::npos &&
              line(live).find("\xE2\x96\x8E") == std::string::npos && shown.find("COMMENTMARK") == std::string::npos,
          "a running turn is not folded, and its text is not an answer");
    // It ends: its steps fold as well.
    put(path,
        R"({"type":"user","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"t2","content":"ok"}]}})" "\n" +
            asst(R"({"type":"text","text":"Fixed: DONEMARK."})", "end_turn"),
        true);
    chat.poll_growth();
    chat.render(p, theme, filters);
    shown = screen(sf);
    check(shown.find("LIVEMARK") == std::string::npos && shown.find("DONEMARK") != std::string::npos &&
              line(row_of("DONEMARK")).find("\xE2\x96\x8E") != std::string::npos,
          "a turn folds when it ends");

    // Finding text in a folded turn opens it.
    chat.set_find_query("COMMENTMARK");
    chat.find_next(-1);
    chat.render(p, theme, filters);
    check(screen(sf).find("COMMENTMARK") != std::string::npos, "a match inside a fold opens it");

    // Full density folds nothing.
    ChatRenderer full;
    full.open(path, &claude_adapter());
    Filters all;
    all.density = Density::Full;
    Surface sf2; sf2.resize(80, 40);
    Painter p2(sf2, Rect{0, 0, 80, 40});
    full.render(p2, theme, all);
    shown = screen(sf2);
    check(shown.find("COMMENTMARK") != std::string::npos && shown.find("LIVEMARK") != std::string::npos &&
              shown.find("1 step") == std::string::npos,
          "at full density every step shows");
  }

  // pi: a turn that stops with text has its steps folded above that text.
  {
    const std::string path = base + "/turns_pi.jsonl";
    put(path,
        R"({"type":"message","id":"1","message":{"role":"user","content":[{"type":"text","text":"count the files"}]}})" "\n"
        R"({"type":"message","id":"2","message":{"role":"assistant","stopReason":"toolUse","content":[{"type":"text","text":"Listing PISTEP."},{"type":"toolCall","id":"k1","name":"bash","arguments":{"command":"ls | wc -l"}}]}})" "\n"
        R"({"type":"message","id":"3","message":{"role":"toolResult","toolCallId":"k1","toolName":"bash","isError":false,"content":[{"type":"text","text":"12"}]}})" "\n"
        R"({"type":"message","id":"4","message":{"role":"assistant","stopReason":"stop","content":[{"type":"text","text":"There are PIANSWER files."}]}})" "\n");
    ChatRenderer chat;
    chat.open(path, &pi_adapter());
    Surface sf; sf.resize(80, 20);
    Painter p(sf, Rect{0, 0, 80, 20});
    Theme theme; Filters filters;
    chat.render(p, theme, filters);
    const std::string shown = screen(sf);
    check(shown.find("PISTEP") == std::string::npos && shown.find("PIANSWER") != std::string::npos &&
              shown.find("\xE2\x96\xB8 1 step \xC2\xB7 1 comment") != std::string::npos,
          "pi: a finished turn folds above its answer");
  }

  // A cancelled codex turn has no answer, so nothing folds.
  {
    const std::string path = base + "/turns_codex.jsonl";
    put(path,
        R"({"type":"response_item","payload":{"type":"message","role":"user","content":[{"type":"input_text","text":"go"}]}})" "\n"
        R"({"type":"response_item","payload":{"type":"message","role":"assistant","content":[{"type":"output_text","text":"Looking CXMARK."}]}})" "\n"
        R"({"type":"response_item","payload":{"type":"function_call","name":"shell","call_id":"c1","arguments":"{\"command\":\"ls\"}"}})" "\n"
        R"({"type":"response_item","payload":{"type":"message","role":"assistant","content":[{"type":"output_text","text":"Half a thought CXLAST"}]}})" "\n"
        R"({"type":"event_msg","payload":{"type":"turn_aborted"}})" "\n");
    ChatRenderer chat;
    chat.open(path, &codex_adapter());
    Surface sf; sf.resize(80, 20);
    Painter p(sf, Rect{0, 0, 80, 20});
    Theme theme; Filters filters;
    chat.render(p, theme, filters);
    const std::string shown = screen(sf);
    check(shown.find("CXMARK") != std::string::npos && shown.find("CXLAST") != std::string::npos &&
              shown.find("\xE2\x96\x8E") == std::string::npos,
          "a cancelled turn keeps its steps, and has no answer");
  }

  // A reply read off the screen is drawn under the chat while the agent writes
  // it, and gives way to the transcript's message once that lands: the text
  // is on screen once, never twice.
  {
    const std::string path = base + "/draft.jsonl";
    const std::string user =
        R"({"type":"user","message":{"role":"user","content":"explain slow start"}})" "\n";
    put(path, user);
    ChatRenderer chat;
    chat.open(path, &claude_adapter());
    Surface sf; sf.resize(80, 20);
    Painter p(sf, Rect{0, 0, 80, 20});
    Theme theme; Filters filters;
    auto count = [&](std::string_view what) {
      const std::string s = screen(sf);
      size_t n = 0;
      for (size_t at = 0; (at = s.find(what, at)) != std::string::npos; at += what.size()) n++;
      return n;
    };
    chat.set_draft("**TCP Slow Start**\nIt begins with a small DRAFTWINDOW and grows");
    chat.render(p, theme, filters);
    check(count("DRAFTWINDOW") == 1 && chat.draft_shown(), "a streaming reply is drawn under the chat");
    check(count("**") == 0, "the draft is rendered as markdown");
    chat.set_draft("**TCP Slow Start**\nIt begins with a small DRAFTWINDOW and grows each round trip.");
    chat.render(p, theme, filters);
    check(count("round trip") == 1, "the draft grows as the screen does");

    put(path, R"({"type":"assistant","message":{"id":"m1","content":[{"type":"text","text":"## TCP Slow Start\n\nIt begins with a small `DRAFTWINDOW` and grows each round trip."}]}})" "\n", true);
    chat.poll_growth();
    chat.render(p, theme, filters);
    check(!chat.draft_shown() && count("DRAFTWINDOW") == 1,
          "the transcript's message replaces the draft, not doubles it");

    chat.set_draft("## TCP Slow Start\n\nIt begins with a small DRAFTWINDOW and grows each round trip.\n\n"
                   "Reading /tmp/shot.png 41s\n/tmp/shot.png (40s)");
    chat.render(p, theme, filters);
    check(!chat.draft_shown() && count("DRAFTWINDOW") == 1,
          "a draft holding the message and then more is the message");

    chat.set_draft("Now something the transcript has NOTYET");
    chat.render(p, theme, filters);
    check(chat.draft_shown() && count("NOTYET") == 1 && count("DRAFTWINDOW") == 1,
          "a new block after a committed one is drawn as a draft");

    put(path, user, true);
    chat.poll_growth();
    chat.set_draft("It begins with a small DRAFTWINDOW and grows each round trip.");
    chat.render(p, theme, filters);
    check(chat.draft_shown(), "a message from an earlier turn does not hold a new draft");
    chat.set_draft("");
    chat.render(p, theme, filters);
    check(!chat.draft_shown(), "an empty draft clears it");
  }

  {
    // Codex sets its approval policy from /permissions; /approvals is gone.
    SessionState cx;
    codex_adapter().seed_state(cx);
    const auto label = [&](std::string_view key) {
      for (const auto& f : cx.fields)
        if (f.key == key) return f.label;
      return std::string();
    };
    const ChipControl c = codex_adapter().chip_control("approval");
    check(label("approval") == "permissions" && c.picker == "/permissions" && c.values.empty(),
          "codex: its approval policy is the permissions chip, opening /permissions");
  }

  {
    // Emoji: as wide as terminals draw them, and kept whole however many
    // code points they are made of.
    check(text::str_width("✅") == 2 && text::str_width("⚡") == 2 && text::str_width("🟢") == 2,
          "emoji: emoji drawn as pictures are two columns");
    check(text::str_width("⚠") == 1 && text::str_width("⚠️") == 2 && text::str_width("🌡") == 1,
          "emoji: a symbol is one column until the selector asks for its picture");
    check(text::str_width("🇮🇹") == 2 && text::str_width("👍🏽") == 2 && text::str_width("👩‍💻") == 2 &&
              text::str_width("a⚠️b") == 4,
          "emoji: flags, skin tones and joined emoji are one glyph of two columns");
    std::vector<text::Span> spans;
    text::wrap_spans("👩‍💻👩‍💻👩‍💻", 4, spans);
    check(spans.size() == 2 && spans[0].off == 0 && spans[1].off == spans[0].len,
          "emoji: wrapping never splits a joined emoji");

    Surface sf;
    sf.resize(12, 1);
    Painter p(sf, Rect{0, 0, 12, 1});
    const int drawn = p.text(0, 0, "a⚠️b👩‍💻c", Style{});
    std::string glyph;
    text::encode(sf.at(1, 0).cp, glyph);
    check(drawn == 7 && sf.at(1, 0).width == 2 && glyph == "⚠️" && screen(sf).starts_with("a⚠️b👩‍💻c "),
          "emoji: a cell holds the whole sequence and draws it back whole");

    Surface front;
    std::string out;
    encode_frame(sf, front, out, true);
    check(out.find("⚠️\x1b[1;4H") != std::string::npos && out.find("👩‍💻\x1b[1;7H") != std::string::npos,
          "emoji: the cursor is placed again after a glyph a terminal may measure differently");

    Vt vt;
    vt.resize(20, 2);
    vt.write("x👩‍💻y⚠");
    vt.write("️z🇮");
    vt.write("🇹!́");
    std::string row;
    for (const Cell& c : vt.row(0))
      if (c.width) text::encode(c.cp, row);
    check(row.starts_with("x👩‍💻y⚠️z🇮🇹! ") && vt.cursor().x == 10,
          "emoji: an agent's emoji take the columns it gave them, across reads");
  }

  std::filesystem::remove_all(base);
  return failures;
}
}  // namespace mico
