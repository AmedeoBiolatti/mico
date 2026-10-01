#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core/event.h"
#include "core/store.h"

namespace mico {

// What text a search counts: what the chat shows at its density. Thinking,
// hidden at the usual density, is found only when it is shown; so a match is
// always something the chat can scroll to and light up. One rule for both
// searches, within a chat and across chats.
struct SearchScope {
  bool thinking = false;
  bool tools = true;
  bool meta = false;
};
bool in_scope(EventKind k, const SearchScope& s);

// One place a search found: a message in a chat, with a line of context.
struct SearchHit {
  std::string path, agent, id, title, project;
  int64_t mtime = 0;
  uint64_t offset = 0;   // byte where the matching transcript line starts
  std::string snippet;   // one line around the match, whitespace collapsed
  size_t match_at = 0;   // the match within `snippet`, in bytes
  size_t match_len = 0;
  bool first_in_chat = false;  // the chat's first hit: draw its heading above
  int chat_hits = 0;           // on the first hit: how many the chat has
};

// Searches every chat mico knows about, newest first. A pass is started with
// start(); step() works through the files for at most a few milliseconds and
// returns, so a slow disk or a two-gigabyte rollout never blocks a frame.
//
// Each file is mapped and scanned raw, ASCII case folded; a raw hit is then
// parsed and kept only if the query is in text a chat shows (a message, a
// tool call or its output), never in JSON keys, ids or signatures.
class ChatSearch {
 public:
  ~ChatSearch();
  void start(const std::vector<Project>& projects, const Store& store, std::string query,
             SearchScope scope = {});
  bool step(int budget_ms);
  void cancel();
  bool complete() const { return next_job_ >= jobs_.size() && !map_; }
  bool started() const { return !query_.empty(); }
  const std::string& query() const { return query_; }
  size_t files_done() const { return next_job_ - (map_ ? 1 : 0); }
  size_t files_total() const { return jobs_.size(); }
  size_t chats_with_hits() const { return chats_; }
  size_t total_hits() const { return total_; }
  const std::vector<SearchHit>& hits() const { return hits_; }

  static constexpr int kSnippetsPerChat = 3;
  static constexpr size_t kMaxHits = 600;

 private:
  struct Job {
    SessionRef s;
    std::string project;
    std::string title;
  };
  void open_next();
  void close_map();
  // Confirms a raw hit on the line [a, b) and records it.
  bool confirm(const Job& j, size_t a, size_t b);

  std::string query_, fold_;
  SearchScope scope_;
  std::vector<Job> jobs_;
  size_t next_job_ = 0;
  const char* map_ = nullptr;
  size_t size_ = 0;
  size_t pos_ = 0;
  int file_hits_ = 0;
  size_t file_first_ = 0;  // index in hits_ of this chat's first hit
  bool file_first_kept_ = false;
  std::vector<SearchHit> hits_;
  size_t chats_ = 0, total_ = 0;
  Arena tmp_;                // reused to parse a candidate line
  std::vector<Event> evs_;
};

}  // namespace mico
