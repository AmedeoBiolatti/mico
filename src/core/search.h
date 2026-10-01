#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "model/event.h"
#include "base/parallel.h"
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
// start(); the files are read on worker threads, and step() merges what they
// have found, in newest-first order, waiting at most its budget — so a slow
// disk or a two-gigabyte rollout never blocks a frame.
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
  bool complete() const { return merged_ >= jobs_.size(); }
  bool started() const { return !query_.empty(); }
  const std::string& query() const { return query_; }
  size_t files_done() const { return merged_; }
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
  // One file's search, done on a worker: its first few hits, and how many it
  // had in all.
  struct Work {
    const Job* job = nullptr;
    std::string fold;
    SearchScope scope;
    std::vector<SearchHit> hits;  // at most kSnippetsPerChat
    int count = 0;
  };
  static void search_file(Work& w);
  void merge(Work& w);

  std::string query_, fold_;
  SearchScope scope_;
  std::vector<Job> jobs_;
  Batch<Work> batch_;
  std::vector<Work> done_;   // finished out of order, waiting for their turn
  std::vector<bool> ready_;  // per job: finished and waiting in done_
  size_t merged_ = 0;        // jobs merged, in order
  std::vector<SearchHit> hits_;
  size_t chats_ = 0, total_ = 0;
};

}  // namespace mico
