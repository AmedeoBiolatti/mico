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
  void read_usage(Jsonl& j, UsageEntry& e, UsageResume& r) const override;

  // changes.cpp
  bool may_have_changes(std::string_view raw) const override;
  void read_changes(std::string_view raw, std::string_view cwd, bool text,
                    std::vector<LineChanges>& out) const override;

  // list.cpp
  void list_sessions(const std::function<void(SessionRef&&)>& add) const override;

  // commands.cpp
  void file_commands(const std::string& cwd, const std::string& home,
                     std::vector<SlashCommand>& out) const override;
  ChipControl chip_control(std::string_view key) const override;

  // The agent's directory name, in the home directory (sessions, the user's
  // prompts and skills) and in a project (the project's): ".pi", ".omp".
  virtual std::string_view dot_dir() const = 0;
  // Where the agent keeps its sessions, one folder per working directory.
  std::string sessions_dir() const;

  // pi and omp render a complete chat of their own into the terminal. Splicing
  // its tail shows that rendering verbatim — a streaming reply, thinking, tool
  // output — beside mico's own rendering of the same turn, which is exactly
  // the duplication the chat view exists to avoid. Neither leads a completed
  // message with a bullet and both park their footer below the editor, so no
  // row is reliably "work in flight". Their chat view is transcript-only; the
  // pane title and state chips already say the agent is working.
  void live_rows(const Vt&, std::vector<int>& out, int) const override { out.clear(); }
};

// Sessions are ~/.pi/agent/sessions/<cwd-slug>/<timestamp>_<id>.jsonl, and pi
// takes the id it is to use.
class PiAdapter final : public PiFamilyAdapter {
 public:
  std::string_view id() const override { return "pi"; }
  std::string_view label() const override { return "Pi"; }
  std::string_view dot_dir() const override { return ".pi"; }
  std::vector<SlashCommand> builtin_commands() const override;

  // session.cpp
  void prepare(Launch& l, const LaunchExtras& x) const override;
  bool continue_session(Launch& l, std::string_view id, bool fork, std::string* note) const override;
  bool find_transcript(const TranscriptQuery& q, FoundTranscript& out) const override;
};

// Sessions are ~/.omp/agent/sessions/<cwd-slug>/*.jsonl. omp cannot be told an
// id, so a new session's transcript is found after the fact, as codex's is.
class OmpAdapter final : public PiFamilyAdapter {
 public:
  std::string_view id() const override { return "omp"; }
  std::string_view name() const override { return "Oh My Pi"; }
  std::string_view label() const override { return "OMP"; }
  std::string_view dot_dir() const override { return ".omp"; }
  std::vector<SlashCommand> builtin_commands() const override;

  // session.cpp
  bool continue_session(Launch& l, std::string_view id, bool fork, std::string* note) const override;
  void snapshot_transcripts(std::vector<std::string>& out) const override;
  bool find_transcript(const TranscriptQuery& q, FoundTranscript& out) const override;
};

// pi and omp both lay sessions out flat as <root>/<cwd-slug>/<file>.jsonl —
// one level, unlike codex's year/month/day tree.
void for_each_pi_family_session(const std::string& root,
                                const std::function<void(const std::string&)>& fn);

}  // namespace mico
