#pragma once
#include <functional>
#include <string>

#include "adapters/adapter.h"

namespace mico {

// earendil-works/pi-coding-agent ("pi") and Oh My Pi ("omp"), which is built
// on pi's engine: both write the same {"type":"message","message":{...}}
// records, the same model_change / thinking_level_change events, and the same
// toolCall / toolResult blocks, so they share a transcript reader. They differ
// in how they are started and how a running session's transcript is found.
class PiFamilyAdapter : public Adapter {
 public:
  // transcript.cpp
  void parse(std::string_view raw, Arena& arena, std::vector<Event>& out) const override;
  void observe(std::string_view raw, SessionState& st) const override;
  void seed_state(SessionState& st) const override;

  // tools.cpp
  void read_tools(std::string_view raw, uint64_t offset, ToolSink& sink) const override;

  // usage.cpp
  void read_usage(LineReader& j, UsageEntry& e, UsageResume& r) const override;

  // changes.cpp
  bool may_have_changes(std::string_view raw) const override;
  void read_changes(std::string_view raw, std::string_view cwd, bool text,
                    std::vector<LineChanges>& out) const override;

  // list.cpp
  void list_sessions(const std::function<void(SessionRef&&)>& add) const override;
  // session.cpp: omp's task calls. pi starts no subagents, and finds none.
  void call_subagents(const std::string& path, std::string_view line, uint64_t tool_id,
                      std::vector<SubagentRun>& out) const override;

  // commands.cpp
  void file_commands(const std::string& cwd, const std::string& home,
                     std::vector<SlashCommand>& out) const override;
  ChipControl chip_control(std::string_view key) const override;
  // Both list their models on the command line; the answer is read whole.
  std::vector<std::string> command_probe_argv() const override;
  bool read_command_probe(std::string_view output, bool ended, CommandProbeAnswer& out) const override;

  // screen.cpp
  std::string screen_reply(const Vt& vt) const override;
  // Working while the screen says so: pi's rule "── ⠹ Working ──", omp's "⎋ Working…".
  bool busy(const Liveness& l) const override;

  // The agent's directory in a project, for the project's prompts and skills:
  // ".pi", ".omp".
  virtual std::string_view dot_dir() const = 0;
  // The agent's own directory under `home` — settings, the user's prompts and
  // skills — wherever its environment moves it (PI_CODING_AGENT_DIR, and
  // omp's profiles).
  virtual std::string agent_dir(const std::string& home) const = 0;
  // Where the agent keeps its sessions, one folder per working directory.
  virtual std::string sessions_dir() const;
  // Each top-level transcript: <sessions_dir>/<cwd-slug>/*.jsonl, then those
  // directly in the folder a session-dir override names —
  // $PI_CODING_AGENT_SESSION_DIR, or --session-dir on `argv` (relative to
  // `cwd`) when one is given. omp's subagent runs, a level further down, are
  // not among them.
  void for_each_session(const std::function<void(const std::string&)>& fn,
                        const std::vector<std::string>* argv = nullptr,
                        const std::string& cwd = {}) const;
  // What mico gives the agent at launch, on a command line that runs it:
  // mico's tool extension (-e) and its hints (--append-system-prompt).
  void add_extras(Launch& l, const LaunchExtras& x) const;

  // omp's subagent runs of the session whose transcript is `path`:
  // <path minus .jsonl>/<AgentName>.jsonl. pi has none, and finds none.
  static void for_each_subagent(const std::string& path,
                                const std::function<void(const std::string&)>& fn);

  // pi and omp render a complete chat of their own into the terminal. Splicing
  // its tail shows that rendering verbatim — a streaming reply, thinking, tool
  // output — beside mico's own rendering of the same turn, which is exactly
  // the duplication the chat view exists to avoid. Neither leads a completed
  // message with a bullet and both park their footer below the editor, so no
  // row is reliably "work in flight". Their chat view is transcript-only; the
  // pane title and state chips already say the agent is working.
  void live_rows(const Vt&, std::vector<int>& out, int) const override { out.clear(); }
};

// Sessions are ~/.pi/agent/sessions/<cwd-slug>/<timestamp>_<id>.jsonl
// ($PI_CODING_AGENT_DIR/sessions when set), and pi takes the id it is to use.
class PiAdapter final : public PiFamilyAdapter {
 public:
  std::string_view id() const override { return "pi"; }
  std::string_view label() const override { return "Pi"; }
  std::string_view dot_dir() const override { return ".pi"; }
  std::string agent_dir(const std::string& home) const override;
  std::vector<SlashCommand> builtin_commands() const override;

  // session.cpp
  void prepare(Launch& l, const LaunchExtras& x) const override;
  bool continue_session(Launch& l, std::string_view id, bool fork, std::string* note) const override;
  bool find_transcript(const TranscriptQuery& q, FoundTranscript& out) const override;
};

// Sessions are ~/.omp/agent/sessions/<cwd-slug>/*.jsonl — or under a profile,
// $PI_CODING_AGENT_DIR or $XDG_DATA_HOME/omp; see agent_dir(). omp cannot be
// told an id, so a new session's transcript is found after the fact, as
// codex's is.
class OmpAdapter final : public PiFamilyAdapter {
 public:
  std::string_view id() const override { return "omp"; }
  std::string_view name() const override { return "Oh My Pi"; }
  std::string_view label() const override { return "OMP"; }
  std::string_view dot_dir() const override { return ".omp"; }
  std::string agent_dir(const std::string& home) const override;
  std::string sessions_dir() const override;
  std::vector<SlashCommand> builtin_commands() const override;
  ChipControl chip_control(std::string_view key) const override;
  bool resumes_subagents() const override { return true; }

  // background.cpp
  void read_background(std::string_view raw, uint64_t offset, BackgroundTasks& t) const override;

  // session.cpp
  void prepare(Launch& l, const LaunchExtras& x) const override;
  bool continue_session(Launch& l, std::string_view id, bool fork, std::string* note) const override;
  void snapshot_transcripts(const std::vector<std::string>& argv, const std::string& cwd,
                            std::vector<std::string>& out) const override;
  bool find_transcript(const TranscriptQuery& q, FoundTranscript& out) const override;
};

// The files an edit script touches: omp's "[path#hash]" headers, or
// apply_patch's "*** Update File: path" and its kin, joined by ", ". Empty for
// a script that names none.
std::string edit_script_paths(std::string_view script);

}  // namespace mico
