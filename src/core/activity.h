#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>

#include "base/parallel.h"
#include <vector>

#include "core/edits.h"
#include "core/store.h"

namespace mico {

// What a tool call was for, judged from the tool and its command line.
enum class ToolKind : uint8_t {
  Build,    // cmake --build, make, cargo build, gcc, npm run build…
  Test,     // pytest, ctest, cargo test, a tests/ script…
  Run,      // running a program or script
  Git,      // git, gh
  Install,  // pip/npm/apt/cargo install
  Network,  // curl, wget, fetching or searching the web
  Read,     // reading and searching: cat, grep, rg, ls, Read, Grep…
  Edit,     // changing files: Edit, Write, apply_patch
  Agent,    // a subagent or task
  Wait,     // waiting for the user: a question, a plan to approve
  Other,
};
inline constexpr int kToolKinds = int(ToolKind::Other) + 1;
const char* tool_kind_name(ToolKind k);

// Sorts a call into a kind, and names the group it belongs with in the
// "by command" table: "cmake --build", "git status", "python3 train.py".
// A command line doing several things is named by the heaviest of them:
// `cd x && cp a b && make` is a build.
// `shown_from`, when given, is where in `command` the deciding step starts.
ToolKind classify_tool(std::string_view tool, std::string_view command, std::string* group,
                       size_t* shown_from = nullptr);

// One tool call and how long it took. Durations come from the agent when it
// records them (codex), else from the timestamps of the call and its result
// (claude, pi, omp), which also count any wait for a permission prompt.
struct ToolRun {
  int64_t start_ms = 0;  // unix milliseconds
  int64_t dur_ms = -1;   // -1: no result yet
  uint64_t offset = 0;   // where the call's transcript line starts
  ToolKind kind = ToolKind::Other;
  bool failed = false;
  // Most of its time went on a permission prompt, not on the work: it counts
  // as waiting for the user.
  bool waited = false;
  std::string tool;      // Bash, Edit, exec_command…
  std::string command;   // the command line or the file, on one line
  std::string group;
  std::string dir;       // the folder it says it worked in (command_dir()); empty when none
};

// A change a chat made to a file: how big, and where in the transcript its
// diff is, to be read again when someone looks at it.
struct FileEdit {
  int64_t at_ms = 0;         // unix milliseconds
  uint64_t offset = 0;       // the transcript line holding the diff
  uint64_t call_offset = 0;  // where the call starts, to open the chat there
  std::string file;          // absolute
  std::string moved_to;
  EditOp op = EditOp::Edit;
  int added = 0, removed = 0;
};

// A commit a chat made, as git announced it in what the call printed:
// "[main 777c513] The view is remembered". Read from the agent's record, so
// nothing is guessed; whether the commit is still in its repository is git's
// to say (core/git.h).
struct ChatCommit {
  int64_t at_ms = 0;         // unix milliseconds
  uint64_t call_offset = 0;  // where the call starts, to open the chat there
  std::string hash;          // as printed, usually abbreviated
  std::string branch;        // empty when HEAD was detached
  std::string subject;
};

// True when `command` runs a git command that makes a commit and says so:
// commit, cherry-pick, revert. Not one that only mentions them.
bool makes_commits(std::string_view command);
// The folder a shell command line works in, when it says: where its `cd`s
// (or `pushd`s) lead before the step that does the work, or that step's
// `git -C`, made absolute against `cwd`. Empty when it names none, or names
// one through a variable.
std::string command_dir(std::string_view command, std::string_view cwd);
// The commits a command's output announces, one "[branch hash] subject"
// line each, in order.
std::vector<ChatCommit> commits_announced(std::string_view output);

struct ChatActivity {
  std::string path, agent, id, title, project, cwd;
  std::vector<ToolRun> runs;    // in transcript order
  std::vector<FileEdit> edits;  // the changes that went through, in order
  std::vector<ChatCommit> commits;  // in order, each hash once
};

// Every tool call in every chat mico knows, kept up to date the way the usage
// index is: a pass rebuilds the list from a per-file cache, and a file that
// grew is read only from where the last pass stopped.
class ActivityIndex {
 public:
  void start(const std::vector<Project>& projects, const Store& store);
  // Reads for up to `budget_ms`. True when the pass is complete.
  bool step(int budget_ms);
  bool complete() const { return jobs_.empty() && !batch_.running(); }
  size_t done() const { return chats_.size(); }
  size_t total() const { return chats_.size() + jobs_.size() + batch_.pending(); }
  const std::vector<const ChatActivity*>& chats() const { return chats_; }

  // Reads one file from scratch. For tests.
  static ChatActivity read_file(const std::string& path, const std::string& agent, const std::string& cwd = "/");

  // Where a file was left: calls still waiting for their result, by id.
  struct Pending {
    int64_t start_ms;
    uint64_t offset;
    std::string tool, command;
  };
  struct Resume {
    uint64_t offset = 0;
    std::unordered_map<std::string, Pending> pending;
    // Changes a call proposed, kept until its result says whether they went through.
    std::unordered_map<std::string, std::vector<FileEdit>> pending_edits;
  };

 private:
  struct Cached {
    int64_t mtime = 0;
    uint64_t size = 0;
    ChatActivity data;
    Resume resume;
  };
  struct Job {
    SessionRef s;
    std::string project, title;
    size_t order = 0;  // its place in the listing, kept whatever finishes first
  };
  // Files read on worker threads: a job, and the cache entry it brings up to date.
  struct Work {
    Job job;
    Cached c;
  };
  std::vector<Job> jobs_;
  Batch<Work> batch_;
  std::vector<const ChatActivity*> chats_;
  std::vector<size_t> order_;  // per chat in chats_, its place in the listing
  std::map<std::string, Cached> cache_;
};

}  // namespace mico
