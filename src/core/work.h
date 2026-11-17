#pragma once
#include <string>
#include <unordered_map>
#include <vector>

#include "core/conversation.h"
#include "core/store.h"

namespace mico {
class LiveSession;
struct SideTurn;
class Workspace;

// A chat's work, independent of its presentation. Detail is read only for
// the selected item; inactive subagents and output files stay unopened.
struct WorkItem {
  std::string id, kind, title, status, path, info, output_file;
  std::string command;  // a terminal's command line, whole
  std::string text;  // a side question's answer, never written to a transcript
  uint64_t at = 0, tool = 0;
  uint64_t key = 0;
};

class WorkSet {
 public:
  void refresh(const Conversation& conv, const LiveSession* live, const Workspace& ws,
               bool force = false);
  const std::vector<WorkItem>& items() const { return items_; }
  const WorkItem* find(const std::string& id) const;
  std::string detail(const WorkItem& item, const Conversation& conv);
  bool supports_btw() const { return supports_btw_; }
  // The id of the side thread's item.
  static constexpr const char* kThread = "btw:thread";
  // The side thread as markdown: each question in bold, then its answer.
  static std::string thread_text(const std::vector<SideTurn>& turns);

 private:
  std::vector<WorkItem> items_;
  Conversation child_;
  std::string path_;
  std::unordered_map<uint64_t, std::string> described_;  // a terminal's description, by tool id
  int64_t refreshed_ms_ = 0;
  bool supports_btw_ = false;
};
}  // namespace mico
