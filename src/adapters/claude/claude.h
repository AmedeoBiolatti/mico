#pragma once
#include "adapters/adapter.h"
#include "adapters/screen.h"

namespace mico {

// Claude Code. Transcripts are ~/.claude/projects/<cwd-slug>/<session-id>.jsonl,
// and claude takes the id it is to use, so a running session's transcript is
// found by name.
class ClaudeAdapter final : public Adapter {
 public:
  std::string_view id() const override { return "claude"; }
  std::string_view name() const override { return "Claude Code"; }
  std::string_view label() const override { return "Claude"; }

  // transcript.cpp
  void parse(std::string_view raw, Arena& arena, std::vector<Event>& out) const override;
  void observe(std::string_view raw, SessionState& st) const override;
  void seed_state(SessionState& st) const override;

  // tools.cpp
  void read_tools(std::string_view raw, uint64_t offset, ToolSink& sink) const override;

  // usage.cpp
  void read_usage(Jsonl& j, UsageEntry& e, UsageResume& r) const override;
  bool resumes_usage() const override { return true; }
  bool prices_from_samples() const override { return true; }

  // changes.cpp
  bool may_have_changes(std::string_view raw) const override;
  void read_changes(std::string_view raw, std::string_view cwd, bool text,
                    std::vector<LineChanges>& out) const override;

  // list.cpp
  void list_sessions(const std::function<void(SessionRef&&)>& add) const override;
  void trust_folder(const std::string& path) const override;

  // session.cpp
  void prepare(Launch& l, const LaunchExtras& x) const override;
  bool continue_session(Launch& l, std::string_view id, bool fork, std::string* note) const override;
  bool find_transcript(const TranscriptQuery& q, FoundTranscript& out) const override;
  bool busy(const Liveness& l) const override;
  bool awaits_input(const Vt& vt) const override;
  std::string startup_answer(const Vt& vt, bool* confirms) const override;
  bool startup_prompt(const Vt& vt) const override;

  // screen.cpp
  bool permission_prompt(const Vt& vt, PermissionPrompt& out) const override;
  bool side_panel(const Vt& vt, BtwPanel& out) const override;
  bool compacting(const Vt& vt) const override;
  std::string screen_reply(const Vt& vt) const override;
  MenuKeys menu_keys() const override;
};

// Reading its screen (screen.cpp).

// Claude's current working footer, excluding ordinary terminal redraws,
// completion summaries and old status lines in scrollback.
bool screen_shows_claude_activity(const Vt& vt);
// The text of Claude's live spinner row ("✻ Compacting conversation… (12s)"),
// or empty when it shows none. What Claude says it is doing, in its words.
std::string claude_activity_line(const Vt& vt);

// Claude's permission dialog; see PermissionPrompt.
bool parse_permission_prompt(const Vt& vt, PermissionPrompt& out);

// True when the screen is Claude's first-run "do you trust this folder"
// dialog specifically — a prompt mico can answer on the user's behalf, since
// they explicitly added the folder to the tracked list.
bool screen_is_trust_prompt(const Vt& vt);
// Down-arrow presses (negative: up) from the dialog's cursor to its "Yes, I
// trust" choice, or kNoTrustMove when either cannot be found on screen.
constexpr int kNoTrustMove = -1000;
int trust_prompt_moves(const Vt& vt);

// Claude's side-question panel; see BtwPanel.
bool parse_btw_panel(const Vt& vt, BtwPanel& out);

}  // namespace mico
