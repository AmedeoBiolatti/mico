#pragma once
#include <string>
#include <vector>

#include "core/conversation.h"
#include "core/store.h"

namespace mico {
class LiveSession;
class Workspace;

// A chat's work, independent of its presentation. Detail is read only for
// the selected item; inactive subagents and output files stay unopened.
struct WorkItem {
  std::string id, kind, title, status, path, info, output_file;
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

 private:
  std::vector<WorkItem> items_;
  Conversation child_;
  std::string path_;
  int64_t refreshed_ms_ = 0;
  bool supports_btw_ = false;
};
}  // namespace mico
