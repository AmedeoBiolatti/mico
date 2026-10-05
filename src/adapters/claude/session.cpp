#include "adapters/claude/claude.h"

#include "adapters/cmdline.h"
#include "base/fs.h"
#include "base/uuid.h"

namespace mico {

std::string claude_home() {
  const char* dir = getenv("CLAUDE_CONFIG_DIR");
  return dir && *dir ? std::string(dir) : fs::home() + "/.claude";
}

std::string claude_state_file() {
  const char* dir = getenv("CLAUDE_CONFIG_DIR");
  return dir && *dir ? std::string(dir) + "/.claude.json" : fs::home() + "/.claude.json";
}

void ClaudeAdapter::prepare(Launch& l, const LaunchExtras& x) const {
  std::vector<std::string>& argv = l.argv;
  if (argv.empty()) {
    // Pre-assigning the id makes correlation exact: no races, no guessing
    // which of several concurrently-started sessions is ours.
    if (l.session_id.empty()) l.session_id = make_uuid_v4();
    argv = {"claude", "--session-id", l.session_id};
  }
  if (l.session_id.empty()) cmdline::adopt_session_id(argv, l.session_id);

  // The rest is claude's own flags, and any other program would choke on them.
  if (cmdline::program(argv) != "claude") return;

  // Claude keeps an AskUserQuestion call in memory while its dialog is up and
  // writes it only with the answer, so the chat saw nothing and the agent
  // looked stopped. It does flush the transcript before running a hook (hooks
  // are handed transcript_path), so a do-nothing PreToolUse hook on that one
  // tool lands the call in the transcript before the dialog opens. The same
  // goes for a call waiting on permission: the PermissionRequest hook fires
  // only when the dialog is about to show, so the chat can show the command
  // or edit being approved, not an agent that seems to have stopped. Flag
  // settings merge with the user's own and are never written to ~/.claude.
  // A command line that already brings --settings is left alone: claude reads
  // only one.
  //
  // The status line is mico too, when it reads claude's usage limits: claude
  // hands its status line command the limits it reads off the API's replies
  // (see limits.cpp), and that command runs the user's own status line, if
  // they set one, on the same input.
  if (!cmdline::has_flag(argv, "--settings")) {
    std::string settings = R"({"hooks":{"PreToolUse":[{"matcher":"AskUserQuestion",)"
                           R"("hooks":[{"type":"command","command":"true"}]}],)"
                           R"("PermissionRequest":[{"matcher":"",)"
                           R"("hooks":[{"type":"command","command":"true"}]}]})";
    if (!x.status_exe.empty()) {
      // Run by a shell: the path single-quoted, any quote in it closed and
      // reopened around an escaped one.
      std::string sh = "'";
      for (char c : x.status_exe) sh += c == '\'' ? std::string("'\\''") : std::string(1, c);
      sh += "' --claude-status";
      settings += R"(,"statusLine":{"type":"command","command":)" + cmdline::json_string(sh) + "}";
    }
    settings += "}";
    argv.push_back("--settings");
    argv.push_back(std::move(settings));
  }

  // mico's own tools, handed over on the command line, never written into
  // claude's config. plot is allowed without a prompt, since drawing a chart
  // changes nothing.
  if (!x.mcp_exe.empty()) {
    argv.push_back("--mcp-config={\"mcpServers\":{\"mico\":{\"type\":\"stdio\",\"command\":" +
                   cmdline::json_string(x.mcp_exe) + ",\"args\":[\"--mcp\"]}}}");
    argv.push_back("--allowedTools=mcp__mico__plot");
  }

  // Appended: claude's own system prompt stays.
  if (!x.hints.empty() && !cmdline::mentions(argv, "system-prompt")) {
    argv.push_back("--append-system-prompt");
    argv.push_back(x.hints + x.mcp_hint);
  }
}

bool ClaudeAdapter::continue_session(Launch& l, std::string_view id, bool fork, std::string*) const {
  l.argv = {"claude", "--resume", std::string(id)};
  if (fork) {
    // Verified: claude honours --session-id alongside --fork-session, so the
    // branch is correlated exactly from the moment it is created.
    l.session_id = make_uuid_v4();
    l.argv.push_back("--fork-session");
    l.argv.push_back("--session-id");
    l.argv.push_back(l.session_id);
  } else {
    l.session_id = id;  // resuming keeps the original transcript
  }
  return true;
}

// The transcript is exactly <uuid>.jsonl, so search for that name and accept
// no substitutes.
bool ClaudeAdapter::find_transcript(const TranscriptQuery& q, FoundTranscript& out) const {
  const std::string root = claude_home() + "/projects";
  fs::list_dir(root, true, [&](const std::string& slug) {
    if (!out.path.empty()) return;
    std::string cand = root + "/" + slug + "/" + q.session_id + ".jsonl";
    if (fs::exists(cand) && !q.claimed(cand)) out.path = cand;
  });
  out.session_id = q.session_id;
  return !out.path.empty();
}

bool ClaudeAdapter::busy(const Liveness& l) const {
  // Claude redraws its prompt, notices and timers even while idle. Output
  // recency is not evidence of work; follow its live spinner instead. This
  // also keeps a quiet model request active until Claude clears the footer.
  return screen_shows_claude_activity(l.vt);
}

// Claude's first-run "do you trust this folder" dialog, answered so it does
// not time out and kill the session while nobody is looking at it.
//
// The dialog's cursor starts on "No, exit": Enter there quits claude. And a
// key sent before the dialog listens is dropped, so a scripted "down, enter"
// can land as a bare Enter. So one step at a time, each judged from the
// screen: an arrow while the cursor is elsewhere, and Enter only once the
// screen shows it on "Yes".
bool ClaudeAdapter::startup_prompt(const Vt& vt) const { return screen_is_trust_prompt(vt); }

std::string ClaudeAdapter::startup_answer(const Vt& vt, bool* confirms) const {
  if (!screen_is_trust_prompt(vt)) return {};
  const int moves = trust_prompt_moves(vt);
  if (moves == kNoTrustMove) return {};
  *confirms = moves == 0;
  return moves > 0 ? "\x1b[B" : moves < 0 ? "\x1b[A" : "\r";
}

}  // namespace mico
