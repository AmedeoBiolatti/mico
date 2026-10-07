#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "model/changes.h"

// What git says about the folders mico tracks and the commits agents made in
// them. Only ever read: status, show, blame, with --no-optional-locks so a
// look never takes the index lock from an agent that is committing. Git runs
// on a thread of its own, so a slow repository never holds up a frame.
namespace mico {

// A work tree as `git status` sees it.
struct GitStatus {
  bool repo = false;     // false: not in a work tree, or no git
  std::string branch;    // empty when HEAD is detached
  std::string head;      // the commit checked out, abbreviated; empty before the first
  std::string upstream;  // "origin/main"; empty when the branch tracks nothing
  int ahead = 0, behind = 0;
  int staged = 0;     // files with changes in the index
  int changed = 0;    // files with changes in the work tree, not yet staged
  int untracked = 0;
  int conflicts = 0;
  int files = 0;      // files that differ in any of those ways, each once
  std::string top;    // the work tree's top, absolute; empty until known
  // Each file that differs: git's two letters for it, the index's then the
  // work tree's ('.' unchanged, '?' untracked, 'U' in conflict), and its
  // path from the top. An untracked folder's path ends in '/'.
  struct Entry {
    char x = '.', y = '.';
    std::string path;
    std::string orig;  // a rename's old path
    // A submodule's three marks: its commit moved ('C'), its files changed
    // ('M'), it has untracked files ('U'), each '.' when not. Empty for a file.
    std::string sub;
  };
  std::vector<Entry> entries;
};
// `git status --porcelain=v2 --branch -z`. False when it is not that.
bool parse_status(std::string_view porcelain, GitStatus& out);

// The repositories a folder has to do with: the one holding it, if any, then
// every one found inside it, each after the one it is inside of.
struct GitRepos {
  std::vector<std::string> tops;  // their work trees' tops, absolute
  bool held = false;     // tops[0] holds the folder: it is the folder's own, or one above it
  bool looking = false;  // the folder is still being looked through for more
};

// A work tree of a repository: the main one, or one `git worktree add` made.
struct GitWorktree {
  std::string path;
  std::string head;    // abbreviated
  std::string branch;  // empty when detached
  bool bare = false, locked = false, prunable = false;
};
// `git worktree list --porcelain`.
std::vector<GitWorktree> parse_worktrees(std::string_view out);

// A local branch.
struct GitBranch {
  std::string name, head, upstream;
  std::string track;  // "[ahead 2, behind 1]", "[gone]", or empty
  std::string subject;
  int64_t time = 0;   // its last commit's, unix seconds
};
// for-each-ref with kBranchFormat, newest first.
extern const char* const kBranchFormat;
std::vector<GitBranch> parse_branches(std::string_view out);

// A commit as the log lists it.
struct GitLogEntry {
  std::string hash;  // in full
  std::string author, subject;
  std::string refs;  // "HEAD -> main, origin/main"
  int64_t time = 0;
};
// git log with kLogFormat.
extern const char* const kLogFormat;
std::vector<GitLogEntry> parse_log(std::string_view out);
// `git log --graph` with kGraphFormat: each line's graph, and the commit on
// it, if any (an index into `commits`); a line of graph alone joins or
// splits branches between two commits.
struct GitGraphRow {
  std::string graph;  // git's own: '*', '|', '/', '\\', '_' and spaces
  int commit = -1;
};
extern const char* const kGraphFormat;
void parse_graph_log(std::string_view out, std::vector<GitGraphRow>& rows, std::vector<GitLogEntry>& commits);

// The lines each file gained and lost, from `git diff --numstat -z`, by
// path from the top; -1 for a binary file.
struct GitNumstat {
  int added = 0, removed = 0;
};
std::vector<std::pair<std::string, GitNumstat>> parse_numstat(std::string_view out);

// A patch of several files, one "diff --git" section each, as `git diff` and
// `git show` print them. `root` makes the paths absolute.
void parse_patch(std::string_view diff, const std::string& root, std::vector<FileChange>& out);

// A commit as git has it, with its diff against its first parent.
struct GitCommit {
  bool found = false;  // false: not in this repository (rewritten, or made elsewhere)
  bool repo = true;    // false: the folder is no git work tree now (or is gone)
  std::string hash;    // in full
  std::string author, subject;
  int64_t time = 0;    // unix seconds
  int added = 0, removed = 0;
  std::vector<FileChange> files;  // each with its lines; paths absolute
};
// The output of show_args() below. `root` is the work tree's top, to make
// the paths absolute.
bool parse_show(std::string_view out, const std::string& root, GitCommit& c);

// Asks git, off the calling thread, and keeps the answers.
class GitIndex {
 public:
  GitIndex() = default;
  ~GitIndex();
  GitIndex(const GitIndex&) = delete;
  GitIndex& operator=(const GitIndex&) = delete;

  // The status of the work tree holding `dir`, as last read; null until the
  // first read is in. Asking is what keeps it fresh: read again when git's
  // HEAD or index move, when refresh() says so, and every half minute while
  // something keeps asking.
  const GitStatus* status(const std::string& dir);
  // Read `dir` again soon: an agent there has just finished a turn.
  void refresh(const std::string& dir);

  // The top of the work tree holding `dir`: the nearest folder, `dir` or one
  // above it, with a .git in it. Empty when there is none. Found by looking,
  // not by asking git, and remembered until refresh_all().
  const std::string& top_of(const std::string& dir);
  // The main work tree of the repository work tree `top` belongs to: `top`
  // itself unless `git worktree add` made it. Remembered until refresh_all().
  const std::string& main_of(const std::string& top);
  // The work trees in `dir` and one level further down, not looking inside
  // one once found, nor in hidden folders or node_modules. Read off this
  // thread: null until it has been; read again a minute on, while asked.
  const std::vector<std::string>* tops_below(const std::string& dir);
  // The states of the work trees at `tops` added up: files, conflicts,
  // ahead, behind. One not read yet counts for nothing.
  GitStatus total(const std::vector<std::string>& tops);

  // Commit `hash` (in full or abbreviated) of the repository holding `dir`.
  // Null while it is being read.
  const GitCommit* commit(const std::string& dir, const std::string& hash);

  // Which commit last changed line `line` of `file`. The answer comes back
  // through take_blame(), once.
  struct Blame {
    std::string dir, file;
    int line = 0;
    std::string hash;   // in full; empty when the line is not committed
    std::string error;  // why there is no answer
  };
  void blame(const std::string& dir, const std::string& file, int line);
  bool take_blame(Blame& out);

  // Takes in what git has answered, and looks for HEAD or the index having
  // moved. True when anything shown may have changed.
  bool pump();
  // What `git <args>` printed in `dir`, as last read; null until the first
  // answer. Read again whenever the status of `dir` changes, and once it is
  // older than `max_age_ms`. `ok` false when git failed; with `any_exit`,
  // a non-zero exit is an answer too (`diff --no-index` exits 1 on a
  // difference).
  struct Query {
    std::string out;
    bool ok = false;
  };
  const Query* query(const std::string& dir, const std::vector<std::string>& args, int max_age_ms = 30000,
                     bool any_exit = false);
  // Everything is read again, as it is next asked for: `r`.
  void refresh_all();

  // Moves whenever anything here does.
  uint64_t version() const { return version_; }
  // True while git is still to answer something asked: the loop comes back
  // for it soon rather than at its idle pace.
  bool waiting() const { return outstanding_ > 0; }

 private:
  enum class Kind { Status, Commit, Blame, Query, Scan };
  struct Job {
    Kind kind;
    std::string dir, arg;
    int line = 0;
    std::vector<std::string> args;  // a query's
    bool any_exit = false;
  };
  struct Done {
    Job job;
    GitStatus status;
    std::string git_dir, top;
    GitCommit commit;
    Blame blame;
    Query query;
    std::vector<std::string> scan;
  };
  struct StatusEntry {
    GitStatus st;
    bool have = false, queued = false, again = false;
    int64_t read_ms = 0, asked_ms = 0;
    std::string git_dir;  // where HEAD and the index are, to watch them
    std::string top;
    int64_t head_ns = 0, index_ns = 0;
    uint64_t epoch = 0;   // moves when the status read differs from the last
  };
  struct QueryEntry {
    Query q;
    bool have = false, queued = false, again = false;
    int64_t read_ms = 0;
    uint64_t epoch = 0;  // its folder's status epoch when it was asked
  };
  std::map<std::string, QueryEntry> queries_;  // dir \n args
  void enqueue(Job job);
  void work();
  static void run(Job& job, Done& d);

  struct ScanEntry {
    std::vector<std::string> tops;
    bool have = false, queued = false, again = false;
    int64_t read_ms = 0;
  };
  std::map<std::string, ScanEntry> scans_;
  std::map<std::string, std::string> top_of_, main_of_;
  std::map<std::string, StatusEntry> status_;
  std::map<std::string, GitCommit> commits_;   // dir + "\n" + hash
  std::map<std::string, bool> commit_queued_;  // the same keys, while being read
  std::vector<Blame> blames_;
  int64_t watched_ms_ = 0;
  int outstanding_ = 0;  // jobs asked for and not yet taken in
  uint64_t version_ = 1;

  std::thread worker_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Job> jobs_;
  std::vector<Done> done_;
  bool stop_ = false;
};

}  // namespace mico
