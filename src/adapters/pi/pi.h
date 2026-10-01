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
};

// Sessions are ~/.pi/agent/sessions/<cwd-slug>/<timestamp>_<id>.jsonl, and pi
// takes the id it is to use.
class PiAdapter final : public PiFamilyAdapter {
 public:
  std::string_view id() const override { return "pi"; }
  std::string_view label() const override { return "Pi"; }

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
