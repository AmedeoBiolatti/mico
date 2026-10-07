#include "core/git.h"

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>

#include "adapters/changes.h"
#include "base/log.h"
#include "base/process.h"

namespace mico {
namespace {

int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Where git is, found once, on the thread that starts the worker.
const std::string& git_path() {
  static const std::string path = proc::find_program("git");
  return path;
}

// Runs `git -C dir <args>` and collects what it prints, up to `cap` bytes.
// False when git is missing, fails, or takes longer than ten seconds (a
// network drive that went away): whatever it was asked about stays unknown.
bool run_git(const std::string& dir, const std::vector<std::string>& args, std::string& out, size_t cap,
             bool any_exit = false) {
  out.clear();
  const std::string& git = git_path();
  if (git.empty()) return false;
  // A repository's own config can name programs for git to run: an fsmonitor
  // hook on every status. mico asks about every tracked folder unbidden, so
  // that one is off; diffs are asked for with --no-ext-diff --no-textconv.
  std::vector<std::string> all = {git, "--no-optional-locks", "-c", "core.quotepath=off",
                                  "-c", "core.fsmonitor=false", "-C", dir};
  all.insert(all.end(), args.begin(), args.end());
  proc::Options opt;
  opt.cap = cap;
  proc::Result r = proc::capture(all, opt);
  out = std::move(r.out);
  if (r.cut) return true;  // too much is still an answer; too slow is none
  if (r.timed_out) return false;
  return any_exit ? r.exit_code >= 0 && r.exit_code != 127 && r.exit_code < 128 : r.exit_code == 0;
}

// The field after the first `n` spaces of `f`: where porcelain v2 puts a
// path, after its fixed fields.
std::string_view after_spaces(std::string_view f, int n) {
  size_t at = 0;
  for (int i = 0; i < n; i++) {
    at = f.find(' ', at);
    if (at == std::string_view::npos) return {};
    at++;
  }
  return f.substr(at);
}

std::string trimmed(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
  return s;
}

int64_t mtime_ns(const std::string& path) {
  struct stat st{};
  if (stat(path.c_str(), &st) != 0) return 0;
  return int64_t(st.st_mtim.tv_sec) * 1000000000 + st.st_mtim.tv_nsec;
}

// What HEAD, the index and HEAD's log were last changed: any commit,
// checkout, reset or staging moves one of them.
void watch_stamps(const std::string& git_dir, int64_t& head, int64_t& index) {
  head = mtime_ns(git_dir + "/HEAD") + mtime_ns(git_dir + "/logs/HEAD");
  index = mtime_ns(git_dir + "/index");
}

bool has_dot_git(const std::string& dir) {
  struct stat st{};
  return stat((dir + "/.git").c_str(), &st) == 0;
}

std::string parent_of(const std::string& p) {
  const size_t slash = p.rfind('/');
  if (slash == std::string::npos) return {};
  return slash == 0 ? "/" : p.substr(0, slash);
}

std::string read_line(const std::string& path) {
  std::string out;
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return out;
  char buf[4096];
  const ssize_t n = read(fd, buf, sizeof buf);
  close(fd);
  if (n > 0) out.assign(buf, size_t(n));
  return trimmed(out.substr(0, out.find('\n')));
}

// The work trees in `dir` and in each folder in it, not inside one found.
void scan_tops(const std::string& dir, int depth, std::vector<std::string>& out) {
  DIR* d = opendir(dir.c_str());
  if (!d) return;
  std::vector<std::string> subdirs;
  while (const dirent* e = readdir(d)) {
    const std::string_view name = e->d_name;
    if (name.empty() || name[0] == '.' || name == "node_modules") continue;
    // A link is not followed: it may lead out of the folder, or round in a loop.
    if (e->d_type == DT_LNK) continue;
    std::string path = dir == "/" ? "/" + std::string(name) : dir + "/" + std::string(name);
    if (e->d_type == DT_UNKNOWN) {
      struct stat st{};
      if (lstat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
    } else if (e->d_type != DT_DIR) {
      continue;
    }
    subdirs.push_back(std::move(path));
  }
  closedir(d);
  std::sort(subdirs.begin(), subdirs.end());
  for (std::string& s : subdirs) {
    if (has_dot_git(s)) out.push_back(std::move(s));
    else if (depth > 1) scan_tops(s, depth - 1, out);
  }
}

}  // namespace

bool parse_status(std::string_view s, GitStatus& out) {
  out = GitStatus{};
  bool any = false;
  size_t i = 0;
  while (i < s.size()) {
    size_t e = s.find('\0', i);
    if (e == std::string_view::npos) e = s.size();
    const std::string_view f = s.substr(i, e - i);
    i = e + 1;
    if (f.empty()) continue;
    any = true;
    if (f.starts_with("# branch.oid ")) {
      const std::string_view oid = f.substr(13);
      if (oid != "(initial)") out.head = std::string(oid.substr(0, 7));
    } else if (f.starts_with("# branch.head ")) {
      const std::string_view h = f.substr(14);
      if (h != "(detached)") out.branch = std::string(h);
    } else if (f.starts_with("# branch.upstream ")) {
      out.upstream = std::string(f.substr(18));
    } else if (f.starts_with("# branch.ab ")) {
      int a = 0, b = 0;
      if (sscanf(std::string(f.substr(12)).c_str(), "+%d -%d", &a, &b) == 2) {
        out.ahead = a;
        out.behind = b;
      }
    } else if (f.size() > 3 && (f[0] == '1' || f[0] == '2') && f[1] == ' ') {
      // "1 XY …": X the index, Y the work tree, '.' unchanged.
      if (f[2] != '.') out.staged++;
      if (f[3] != '.') out.changed++;
      out.files++;
      GitStatus::Entry en{f[2], f[3], std::string(after_spaces(f, f[0] == '1' ? 8 : 9)), {}, {}};
      // "N..." for a file; "S" and three marks for a submodule.
      if (const std::string_view sub = after_spaces(f, 2); sub.size() >= 4 && sub[0] == 'S')
        en.sub = std::string(sub.substr(1, 3));
      // A rename or copy is followed by its original path, a field of its own.
      if (f[0] == '2') {
        const size_t next = s.find('\0', i);
        en.orig = std::string(s.substr(i, (next == std::string_view::npos ? s.size() : next) - i));
        i = next == std::string_view::npos ? s.size() : next + 1;
      }
      out.entries.push_back(std::move(en));
    } else if (f.starts_with("u ")) {
      out.conflicts++;
      out.files++;
      out.entries.push_back(GitStatus::Entry{'U', 'U', std::string(after_spaces(f, 10)), {}, {}});
    } else if (f.starts_with("? ")) {
      out.untracked++;
      out.files++;
      out.entries.push_back(GitStatus::Entry{'?', '?', std::string(f.substr(2)), {}, {}});
    } else if (!f.starts_with("! ") && !f.starts_with("# ")) {
      return false;
    }
  }
  out.repo = any;
  return any;
}

// %H, %an, %at and %s, each ended by a NUL, then the patch.
static const std::vector<std::string> kShowArgs = {
    "show", "--no-color", "--no-ext-diff", "--no-textconv", "--find-renames", "--format=%H%x00%an%x00%at%x00%s%x00"};

void parse_patch(std::string_view diff, const std::string& root, std::vector<FileChange>& out) {
  const auto absolute = [&](std::string_view rel) { return root.empty() ? std::string(rel) : root + "/" + std::string(rel); };
  // One section per file, each opened by its "diff --git a/x b/y" line.
  for (size_t p = diff.find("diff --git "); p != std::string_view::npos;) {
    const size_t next = diff.find("\ndiff --git ", p + 1);
    const std::string_view sec = diff.substr(p, next == std::string_view::npos ? std::string_view::npos : next + 1 - p);
    p = next == std::string_view::npos ? next : next + 1;
    FileChange fc;
    std::string rename_from, rename_to;
    // The header, up to the first hunk: what kind of change it is.
    const size_t hunk = sec.find("\n@@");
    const std::string_view head = sec.substr(0, hunk);
    changes::each_line(head, [&](std::string_view l) {
      if (l.starts_with("new file mode")) fc.op = EditOp::Create;
      else if (l.starts_with("deleted file mode")) fc.op = EditOp::Delete;
      else if (l.starts_with("rename from ")) rename_from = std::string(l.substr(12));
      else if (l.starts_with("rename to ")) rename_to = std::string(l.substr(10));
    });
    std::string header;
    changes::Sink sk{fc, true};
    changes::parse_unified(sec, sk, &header);
    std::string file = !rename_from.empty() ? rename_from : header;
    if (file.empty()) {
      // A file with no hunks (binary, or a mode change): named by the
      // "diff --git a/x b/x" line itself.
      const std::string_view first = sec.substr(0, sec.find('\n'));
      const size_t b = first.rfind(" b/");
      if (b != std::string_view::npos) file = std::string(first.substr(b + 3));
    }
    if (file.empty()) continue;
    fc.file = absolute(file);
    if (!rename_to.empty()) fc.moved_to = absolute(rename_to);
    out.push_back(std::move(fc));
  }
}

std::vector<GitWorktree> parse_worktrees(std::string_view out) {
  std::vector<GitWorktree> v;
  changes::each_line(out, [&](std::string_view l) {
    if (l.starts_with("worktree ")) {
      v.push_back(GitWorktree{});
      v.back().path = std::string(l.substr(9));
      return;
    }
    if (v.empty()) return;
    GitWorktree& w = v.back();
    if (l.starts_with("HEAD ")) w.head = std::string(l.substr(5, 7));
    else if (l.starts_with("branch ")) {
      std::string_view b = l.substr(7);
      if (b.starts_with("refs/heads/")) b.remove_prefix(11);
      w.branch = std::string(b);
    } else if (l == "bare") w.bare = true;
    else if (l.starts_with("locked")) w.locked = true;
    else if (l.starts_with("prunable")) w.prunable = true;
  });
  return v;
}

const char* const kBranchFormat =
    "--format=%(refname:short)%00%(objectname:short)%00%(committerdate:unix)%00%(upstream:short)%00"
    "%(upstream:track)%00%(subject)%00";

// Records of `n` NUL-ended fields, each record after the first opened by
// the newline the format puts between them.
template <class F>
void each_record(std::string_view out, size_t n, F&& fn) {
  std::vector<std::string_view> f(n);
  size_t at = 0;
  for (;;) {
    while (at < out.size() && (out[at] == '\n' || out[at] == '\r')) at++;
    if (at >= out.size()) return;
    for (size_t k = 0; k < n; k++) {
      const size_t e = out.find('\0', at);
      if (e == std::string_view::npos) return;
      f[k] = out.substr(at, e - at);
      at = e + 1;
    }
    fn(f);
  }
}

std::vector<GitBranch> parse_branches(std::string_view out) {
  std::vector<GitBranch> v;
  each_record(out, 6, [&](const std::vector<std::string_view>& f) {
    GitBranch b;
    b.name = std::string(f[0]);
    b.head = std::string(f[1]);
    b.time = std::atoll(std::string(f[2]).c_str());
    b.upstream = std::string(f[3]);
    b.track = std::string(f[4]);
    b.subject = std::string(f[5]);
    v.push_back(std::move(b));
  });
  std::stable_sort(v.begin(), v.end(), [](const GitBranch& a, const GitBranch& b) { return a.time > b.time; });
  return v;
}

const char* const kLogFormat = "--format=%H%x00%an%x00%at%x00%s%x00%D%x00";

std::vector<GitLogEntry> parse_log(std::string_view out) {
  std::vector<GitLogEntry> v;
  each_record(out, 5, [&](const std::vector<std::string_view>& f) {
    GitLogEntry e;
    e.hash = std::string(f[0]);
    e.author = std::string(f[1]);
    e.time = std::atoll(std::string(f[2]).c_str());
    e.subject = std::string(f[3]);
    e.refs = std::string(f[4]);
    v.push_back(std::move(e));
  });
  return v;
}

// The graph, then \x01 and the fields: a line with no \x01 is graph alone.
const char* const kGraphFormat = "--format=%x01%H%x00%an%x00%at%x00%s%x00%D";

void parse_graph_log(std::string_view out, std::vector<GitGraphRow>& rows, std::vector<GitLogEntry>& commits) {
  rows.clear();
  commits.clear();
  changes::each_line(out, [&](std::string_view l) {
    GitGraphRow row;
    const size_t mark = l.find('\x01');
    std::string_view graph = l.substr(0, mark);
    while (!graph.empty() && graph.back() == ' ') graph.remove_suffix(1);
    row.graph = std::string(graph);
    if (mark != std::string_view::npos) {
      std::string_view f[5];
      std::string_view rest = l.substr(mark + 1);
      for (int i = 0; i < 5; i++) {
        const size_t nul = i < 4 ? rest.find('\0') : std::string_view::npos;
        f[i] = rest.substr(0, nul);
        if (nul == std::string_view::npos) {
          if (i < 4) return;  // cut short: not a commit line git wrote whole
          break;
        }
        rest.remove_prefix(nul + 1);
      }
      GitLogEntry e;
      e.hash = std::string(f[0]);
      e.author = std::string(f[1]);
      e.time = std::atoll(std::string(f[2]).c_str());
      e.subject = std::string(f[3]);
      e.refs = std::string(f[4]);
      row.commit = int(commits.size());
      commits.push_back(std::move(e));
    } else if (row.graph.empty()) {
      return;
    }
    rows.push_back(std::move(row));
  });
}

std::vector<std::pair<std::string, GitNumstat>> parse_numstat(std::string_view out) {
  std::vector<std::pair<std::string, GitNumstat>> v;
  size_t at = 0;
  while (at < out.size()) {
    size_t e = out.find('\0', at);
    if (e == std::string_view::npos) break;
    std::string_view rec = out.substr(at, e - at);
    at = e + 1;
    while (!rec.empty() && (rec[0] == '\n')) rec.remove_prefix(1);
    // "added\tremoved\tpath", or for a rename "added\tremoved\t" then the
    // old and new paths as fields of their own.
    const size_t t1 = rec.find('\t'), t2 = t1 == std::string_view::npos ? t1 : rec.find('\t', t1 + 1);
    if (t2 == std::string_view::npos) continue;
    GitNumstat n;
    const std::string_view a = rec.substr(0, t1), r = rec.substr(t1 + 1, t2 - t1 - 1);
    n.added = a == "-" ? -1 : std::atoi(std::string(a).c_str());
    n.removed = r == "-" ? -1 : std::atoi(std::string(r).c_str());
    std::string path(rec.substr(t2 + 1));
    if (path.empty()) {
      const size_t o = out.find('\0', at);
      if (o == std::string_view::npos) break;
      const size_t nw = out.find('\0', o + 1);
      if (nw == std::string_view::npos) break;
      path = std::string(out.substr(o + 1, nw - o - 1));
      at = nw + 1;
    }
    v.emplace_back(std::move(path), n);
  }
  return v;
}

bool parse_show(std::string_view out, const std::string& root, GitCommit& c) {
  const bool repo = c.repo;
  c = GitCommit{};
  c.repo = repo;
  std::string_view fields[4];
  size_t at = 0;
  for (auto& f : fields) {
    const size_t e = out.find('\0', at);
    if (e == std::string_view::npos) return false;
    f = out.substr(at, e - at);
    at = e + 1;
  }
  if (fields[0].size() < 7) return false;
  c.found = true;
  c.hash = std::string(fields[0]);
  c.author = std::string(fields[1]);
  c.time = std::atoll(std::string(fields[2]).c_str());
  c.subject = std::string(fields[3]);
  parse_patch(out.substr(at), root, c.files);
  for (const FileChange& fc : c.files) {
    c.added += fc.added;
    c.removed += fc.removed;
  }
  return true;
}

GitIndex::~GitIndex() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    stop_ = true;
  }
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void GitIndex::enqueue(Job job) {
  git_path();  // found here, on the calling thread, before any fork
  outstanding_++;
  {
    std::lock_guard<std::mutex> lock(mu_);
    jobs_.push_back(std::move(job));
  }
  if (!worker_.joinable()) worker_ = std::thread([this] { work(); });
  cv_.notify_one();
}

void GitIndex::work() {
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [&] { return stop_ || !jobs_.empty(); });
      if (stop_) return;
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }
    Done d;
    {
      logs::Doing doing("running git in", job.dir.c_str());
      run(job, d);
    }
    d.job = std::move(job);
    std::lock_guard<std::mutex> lock(mu_);
    done_.push_back(std::move(d));
  }
}

void GitIndex::run(Job& job, Done& d) {
  std::string out;
  switch (job.kind) {
    case Kind::Status:
      if (run_git(job.dir, {"status", "--porcelain=v2", "--branch", "-z"}, out, 8u << 20))
        parse_status(out, d.status);
      if (d.status.repo && job.arg == "git-dir" &&
          run_git(job.dir, {"rev-parse", "--absolute-git-dir", "--show-toplevel"}, out, 8192)) {
        const size_t nl = out.find('\n');
        d.git_dir = trimmed(out.substr(0, nl));
        if (nl != std::string::npos) d.top = trimmed(out.substr(nl + 1));
      }
      break;
    case Kind::Commit: {
      std::string root;
      if (run_git(job.dir, {"rev-parse", "--show-toplevel"}, out, 4096)) root = trimmed(out);
      d.commit.repo = !root.empty();
      std::vector<std::string> args = kShowArgs;
      args.push_back(job.arg + "^{commit}");
      args.push_back("--");
      // A huge commit is shown in part rather than not at all.
      if (!root.empty() && run_git(job.dir, args, out, 16u << 20)) parse_show(out, root, d.commit);
      break;
    }
    case Kind::Query:
      d.query.ok = run_git(job.dir, job.args, d.query.out, 16u << 20, job.any_exit);
      if (!d.query.ok) d.query.out.clear();
      break;
    case Kind::Scan:
      scan_tops(job.dir, 2, d.scan);
      break;
    case Kind::Blame: {
      d.blame.dir = job.dir;
      d.blame.file = job.arg;
      d.blame.line = job.line;
      // Asked in the file's own folder: it may be in a repository inside
      // the one asked about, or the folder asked about may be in none.
      std::string dir = job.dir, file = job.arg;
      if (const size_t slash = file.rfind('/'); slash != std::string::npos) {
        const std::string folder = file.substr(0, slash);
        dir = file[0] == '/' ? (folder.empty() ? "/" : folder) : dir + "/" + folder;
        file = file.substr(slash + 1);
      }
      if (!run_git(dir,
                   {"blame", "--porcelain", "-L", std::to_string(job.line) + "," + std::to_string(job.line), "--",
                    file},
                   out, 1u << 20)) {
        d.blame.error = "git blame found no line " + std::to_string(job.line) + " in " + job.arg;
      } else {
        const std::string hash = out.substr(0, out.find(' '));
        if (hash.find_first_not_of('0') == std::string::npos) d.blame.error = "that line is not committed yet";
        else d.blame.hash = hash;
      }
      break;
    }
  }
}

const GitStatus* GitIndex::status(const std::string& dir) {
  if (dir.empty()) return nullptr;
  StatusEntry& e = status_[dir];
  const int64_t now = now_ms();
  e.asked_ms = now;
  if (!e.queued && (!e.have || e.again || now - e.read_ms > 30000)) {
    e.queued = true;
    e.again = false;
    enqueue(Job{Kind::Status, dir, e.git_dir.empty() ? "git-dir" : "", 0, {}, false});
  }
  return e.have ? &e.st : nullptr;
}

void GitIndex::refresh(const std::string& dir) {
  auto it = status_.find(dir);
  if (it == status_.end()) return;
  StatusEntry& e = it->second;
  // Read now if it is being shown; else whenever it next is.
  if (e.queued || now_ms() - e.asked_ms > 5000) {
    e.again = true;
    return;
  }
  e.queued = true;
  e.again = false;
  enqueue(Job{Kind::Status, dir, e.git_dir.empty() ? "git-dir" : "", 0, {}, false});
}

const std::string& GitIndex::top_of(const std::string& dir) {
  auto [it, fresh] = top_of_.try_emplace(dir);
  if (fresh)
    for (std::string p = dir; !p.empty(); p = p == "/" ? std::string() : parent_of(p))
      if (has_dot_git(p)) {
        it->second = p;
        break;
      }
  return it->second;
}

const std::string& GitIndex::main_of(const std::string& top) {
  auto [it, fresh] = main_of_.try_emplace(top, top);
  if (!fresh) return it->second;
  // A linked work tree's .git is a file naming its git dir, and that names
  // the repository's own in `commondir`: the main work tree's .git.
  struct stat st{};
  if (stat((top + "/.git").c_str(), &st) != 0 || S_ISDIR(st.st_mode)) return it->second;
  std::string gd = read_line(top + "/.git");
  if (!gd.starts_with("gitdir: ")) return it->second;
  gd = gd.substr(8);
  if (!gd.starts_with('/')) gd = top + "/" + gd;
  std::string common = read_line(gd + "/commondir");
  if (common.empty()) return it->second;  // a submodule's, which has none
  if (!common.starts_with('/')) common = gd + "/" + common;
  char real[PATH_MAX];
  if (!realpath(common.c_str(), real)) return it->second;
  common = real;
  if (common.ends_with("/.git")) it->second = parent_of(common);
  return it->second;
}

const std::vector<std::string>* GitIndex::tops_below(const std::string& dir) {
  ScanEntry& e = scans_[dir];
  if (!e.queued && (!e.have || e.again || now_ms() - e.read_ms > 60000)) {
    e.queued = true;
    e.again = false;
    enqueue(Job{Kind::Scan, dir, {}, 0, {}, false});
  }
  return e.have ? &e.tops : nullptr;
}

GitStatus GitIndex::total(const std::vector<std::string>& tops) {
  GitStatus sum;
  for (const std::string& t : tops)
    if (const GitStatus* st = status(t); st && st->repo) {
      sum.files += st->files;
      sum.conflicts += st->conflicts;
      sum.ahead += st->ahead;
      sum.behind += st->behind;
    }
  return sum;
}

const GitCommit* GitIndex::commit(const std::string& dir, const std::string& hash) {
  const std::string key = dir + "\n" + hash;
  if (auto it = commits_.find(key); it != commits_.end()) return &it->second;
  if (!commit_queued_[key]) {
    commit_queued_[key] = true;
    enqueue(Job{Kind::Commit, dir, hash, 0, {}, false});
  }
  return nullptr;
}

const GitIndex::Query* GitIndex::query(const std::string& dir, const std::vector<std::string>& args, int max_age_ms,
                                       bool any_exit) {
  std::string key = dir + "\n";
  for (const auto& a : args) key += a + "\x1f";
  QueryEntry& q = queries_[key];
  const auto st = status_.find(dir);
  const uint64_t epoch = st == status_.end() ? 0 : st->second.epoch;
  if (!q.queued && (!q.have || q.again || q.epoch != epoch || now_ms() - q.read_ms > max_age_ms)) {
    q.queued = true;
    q.again = false;
    q.epoch = epoch;
    Job job{Kind::Query, dir, {}, 0, args, any_exit};
    enqueue(std::move(job));
  }
  return q.have ? &q.q : nullptr;
}

void GitIndex::refresh_all() {
  for (auto& [dir, e] : status_) e.again = true;
  for (auto& [key, q] : queries_) q.again = true;
  for (auto& [dir, s] : scans_) s.again = true;
  top_of_.clear();
  main_of_.clear();
  version_++;
}

void GitIndex::blame(const std::string& dir, const std::string& file, int line) {
  enqueue(Job{Kind::Blame, dir, file, line, {}, false});
}

bool GitIndex::take_blame(Blame& out) {
  if (blames_.empty()) return false;
  out = std::move(blames_.front());
  blames_.erase(blames_.begin());
  return true;
}

bool GitIndex::pump() {
  bool changed = false;
  std::vector<Done> done;
  {
    std::lock_guard<std::mutex> lock(mu_);
    done.swap(done_);
  }
  outstanding_ -= int(done.size());
  for (Done& d : done) {
    switch (d.job.kind) {
      case Kind::Status: {
        StatusEntry& e = status_[d.job.dir];
        e.queued = false;
        e.read_ms = now_ms();
        if (!d.git_dir.empty()) e.git_dir = d.git_dir;
        if (!d.top.empty()) e.top = d.top;
        if (!e.git_dir.empty()) watch_stamps(e.git_dir, e.head_ns, e.index_ns);
        const GitStatus& s = d.status;
        const GitStatus& o = e.st;
        if (!e.have || s.repo != o.repo || s.branch != o.branch || s.head != o.head || s.upstream != o.upstream ||
            s.ahead != o.ahead || s.behind != o.behind || s.staged != o.staged || s.changed != o.changed ||
            s.untracked != o.untracked || s.conflicts != o.conflicts || s.files != o.files)
          changed = true;
        if (changed || s.entries.size() != o.entries.size()) e.epoch++;
        e.st = std::move(d.status);
        e.st.top = e.top;
        e.have = true;
        break;
      }
      case Kind::Commit: {
        const std::string key = d.job.dir + "\n" + d.job.arg;
        commits_[key] = std::move(d.commit);
        commit_queued_.erase(key);
        changed = true;
        break;
      }
      case Kind::Blame:
        blames_.push_back(std::move(d.blame));
        changed = true;
        break;
      case Kind::Query: {
        std::string key = d.job.dir + "\n";
        for (const auto& a : d.job.args) key += a + "\x1f";
        QueryEntry& q = queries_[key];
        q.queued = false;
        q.read_ms = now_ms();
        if (!q.have || q.q.ok != d.query.ok || q.q.out != d.query.out) changed = true;
        q.q = std::move(d.query);
        q.have = true;
        break;
      }
      case Kind::Scan: {
        ScanEntry& e = scans_[d.job.dir];
        e.queued = false;
        e.read_ms = now_ms();
        if (!e.have || e.tops != d.scan) changed = true;
        e.tops = std::move(d.scan);
        e.have = true;
        break;
      }
    }
  }
  // Once a second, the folders still being looked at: has git moved?
  const int64_t now = now_ms();
  if (now - watched_ms_ >= 1000) {
    watched_ms_ = now;
    for (auto& [dir, e] : status_) {
      if (e.git_dir.empty() || e.queued || now - e.asked_ms > 5000) continue;
      int64_t head = 0, index = 0;
      watch_stamps(e.git_dir, head, index);
      if (head == e.head_ns && index == e.index_ns) continue;
      e.queued = true;
      e.again = false;
      enqueue(Job{Kind::Status, dir, "", 0, {}, false});
    }
  }
  if (changed) version_++;
  return changed;
}

}  // namespace mico
