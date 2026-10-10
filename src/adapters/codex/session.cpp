#include "adapters/codex/codex.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <functional>
#include <map>
#include <set>

#include "adapters/cmdline.h"
#include "base/fs.h"
#include "base/json.h"
#include "base/process.h"

namespace mico {
namespace {

void for_each_rollout(const std::function<void(const std::string&)>& fn) {
  const std::string root = codex_home() + "/sessions";
  fs::list_dir(root, true, [&](const std::string& y) {
    fs::list_dir(root + "/" + y, true, [&](const std::string& m) {
      const std::string md = root + "/" + y + "/" + m;
      fs::list_dir(md, true, [&](const std::string& dd) {
        const std::string dir = md + "/" + dd;
        fs::list_dir(dir, false, [&](const std::string& fn2) {
          if (fs::has_suffix(fn2, ".jsonl"))
            fn(dir + "/" + fn2);
        });
      });
    });
  });
}

// A cwd and timestamp cannot distinguish two Codex instances launched at
// once. The PTY child is a session leader; its process family owns the rollout
// writer. Read only its open file links, without touching transcript contents.
std::set<std::string> open_transcripts(pid_t leader) {
  std::set<std::string> paths;
  if (leader <= 0) return paths;
  fs::list_dir("/proc", true, [&](const std::string& name) {
    if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos) return;
    const pid_t pid = pid_t(std::strtol(name.c_str(), nullptr, 10));
    if (getsid(pid) != leader) return;
    const std::string dir = "/proc/" + name + "/fd";
    fs::list_dir(dir, false, [&](const std::string& fd) {
      char target[4096];
      const ssize_t n = readlink((dir + "/" + fd).c_str(), target, sizeof target);
      if (n <= 0 || size_t(n) == sizeof target) return;
      std::string path(target, size_t(n));
      if (fs::has_suffix(path, ".jsonl")) paths.insert(std::move(path));
    });
  });
  return paths;
}

// Whether the user's own config.toml sets developer_instructions: a -c
// override would replace theirs, not add to it.
bool config_has_instructions() {
  const std::string path = codex_home() + "/config.toml";
  std::string buf;
  const std::string_view s = fs::read_prefix(path, 1u << 20, buf);
  for (size_t at = s.find("developer_instructions"); at != std::string_view::npos;
       at = s.find("developer_instructions", at + 1)) {
    size_t b = at;
    while (b > 0 && (s[b - 1] == ' ' || s[b - 1] == '\t')) b--;
    if (b == 0 || s[b - 1] == '\n') return true;  // a key, not a mention in a value
  }
  return false;
}

// Older Codex releases have no daemon and reject --no-daemon. Ask the
// executable once, without starting an agent or contacting a model.
bool supports_no_daemon(const std::string& program) {
  const std::string bin = proc::find_program(program);
  struct stat st{};
  if (bin.empty() || stat(bin.c_str(), &st) != 0) return false;
  struct Capability { ino_t inode; time_t modified; bool supported; };
  static std::map<std::string, Capability> cache;
  auto it = cache.find(bin);
  if (it != cache.end() && it->second.inode == st.st_ino && it->second.modified == st.st_mtime)
    return it->second.supported;
  proc::Options options;
  options.cap = 128u << 10;
  options.timeout_ms = 2000;
  const auto help = proc::capture({bin, "--help"}, options);
  const bool supported = help.ok() && help.out.find("--no-daemon") != std::string::npos;
  cache[bin] = {st.st_ino, st.st_mtime, supported};
  return supported;
}

}  // namespace

std::string codex_home() {
  const char* home = getenv("CODEX_HOME");
  return home && *home ? std::string(home) : fs::home() + "/.codex";
}

void CodexAdapter::prepare(Launch& l, const LaunchExtras& x) const {
  // No pre-assignable id: the transcript is discovered after the fact.
  if (l.argv.empty()) l.argv = {"codex"};
  std::vector<std::string>& argv = l.argv;
  if (cmdline::program(argv) != "codex") return;
  cmdline::adopt_cwd(argv, l.cwd, {"--cd", "-C"});

  // Transcript discovery ties the writer to this PTY's process session.
  // The shared daemon owns writers outside it; use a local server so two
  // agents in the same folder can still be linked without guessing.
  if (!cmdline::has_flag(argv, "--no-daemon") && !cmdline::has_flag(argv, "--remote") &&
      supports_no_daemon(argv[0])) argv.insert(argv.begin() + 1, "--no-daemon");

  // mico's own tools, as -c overrides rather than written into codex's config.
  // codex asks before every MCP call; plot is approved up front, as for claude.
  if (!x.mcp_exe.empty() && !cmdline::mentions(argv, "mcp_servers.mico")) {
    argv.insert(argv.begin() + 1, {"-c", "mcp_servers.mico.command=" + cmdline::toml_string(x.mcp_exe), "-c",
                                   "mcp_servers.mico.args=[\"--mcp\"]", "-c",
                                   "mcp_servers.mico.tools.plot.approval_mode=\"approve\""});
  }
  // Set only when neither the command line nor the user's config sets
  // developer_instructions: theirs is kept, and mico's hints go without.
  if (!x.hints.empty() && !cmdline::mentions(argv, "developer_instructions") && !config_has_instructions())
    argv.insert(argv.begin() + 1, {"-c", "developer_instructions=" + cmdline::toml_string(x.hints + x.mcp_hint)});
}

bool CodexAdapter::continue_session(Launch& l, std::string_view id, bool fork, std::string*) const {
  l.argv = {"codex", fork ? "fork" : "resume", std::string(id)};
  if (!fork) l.session_id = id;
  return true;
}

void CodexAdapter::snapshot_transcripts(const std::vector<std::string>&, const std::string&,
                                        std::vector<std::string>& out) const {
  // An *already running* session in the same directory has a fresh mtime and
  // a matching cwd, so "newest matching file" would happily attach to someone
  // else's conversation. Whatever exists now is not ours.
  for_each_rollout([&](const std::string& path) { out.push_back(path); });
}

// Resumes match by id; new runs additionally require the rollout to be open in
// this agent's own process session.
bool CodexAdapter::find_transcript(const TranscriptQuery& q, FoundTranscript& out) const {
  std::string best, best_sid;
  const auto owned = q.session_id.empty() ? open_transcripts(q.pid) : std::set<std::string>{};
  int64_t best_mtime = 0;
  auto consider = [&](const std::string& path) {
    // Anything that existed before we spawned belongs to someone else, however
    // recently it was written to.
    const bool resuming = q.resuming();
    if (!resuming && !owned.count(path)) return;
    if (!resuming && q.existed(path)) return;
    if (q.claimed(path)) return;

    struct stat st{};
    if (stat(path.c_str(), &st) != 0) return;
    if (!resuming && int64_t(st.st_mtime) + 5 < q.started_at) return;
    if (int64_t(st.st_mtime) < best_mtime) return;

    thread_local std::string buf;
    std::string_view head = fs::read_first_line(path, buf);
    if (head.empty()) return;

    js::Value payload{};
    std::string_view type;
    js::scan_object(head,
                    [&](std::string_view k, const js::Value& v) {
                      if (k == "type") type = v.body();
                      if (k != "payload") return true;
                      payload = v;
                      return false;
                    });
    if (type != "session_meta" || !payload.is_object()) return;

    std::string cwd;
    std::string_view sid, forked_from;
    bool subagent = false;
    js::scan_object(payload.raw, [&](std::string_view k, const js::Value& v) {
      if (k == "cwd") js::unescape_append(v.body(), cwd);
      else if (k == "id" || k == "session_id") sid = v.body();
      else if (k == "forked_from_id") forked_from = v.body();
      else if (k == "source" && v.is_object())
        js::scan_object(v.raw, [&](std::string_view sk, const js::Value&) {
          if (sk == "subagent") subagent = true;
          return true;
        });
      return true;
    });
    if (sid.empty()) return;
    if (!resuming && subagent) return;
    if (resuming ? sid != q.session_id : cwd != q.cwd) return;
    // A fork records what it came from, which identifies ours exactly even if
    // several start together.
    if (q.forked && !q.origin.empty() && forked_from != q.origin) return;

    best = path;
    best_sid = std::string(sid);
    best_mtime = int64_t(st.st_mtime);
  };
  if (q.resuming()) for_each_rollout(consider);
  else for (const auto& path : owned) consider(path);
  if (best.empty()) return false;
  out.path = std::move(best);
  out.session_id = std::move(best_sid);
  return true;
}

bool CodexAdapter::find_replacement_transcript(const TranscriptQuery& q, FoundTranscript& out) const {
  TranscriptQuery next = q;
  next.session_id.clear();
  next.origin.clear();
  next.forked = false;
  return find_transcript(next, out);
}

bool CodexAdapter::busy(const Liveness& l) const {
  // Codex redraws while it is typed into and at startup, so output is no
  // evidence either. Its status line is, and the transcript bridges the
  // moments it is off screen. Not an open turn alone: one whose end never
  // reached the transcript must not read as working forever, and a turn in
  // progress keeps its status line's clock ticking.
  return screen_shows_codex_activity(l.vt) || (l.turn_open && l.quiet_ms < 10000);
}

bool CodexAdapter::awaits_input(const Vt& vt) const {
  // Codex's dialogs replace its input box. Words like "Do you want to" in the
  // rows above are as likely its own reply.
  return screen_awaits_codex_input(vt);
}

// Codex's turns as its transcript records them: task_started, then
// task_complete or turn_aborted. The record's type comes first:
// {"timestamp":…,"type":"event_msg","payload":{"type":"task_started",…
int CodexAdapter::turn_marker(std::string_view head) const {
  auto has = [&](std::string_view m) { return head.find(m) != std::string_view::npos; };
  if (has("\"type\":\"task_started\"") || has("\"type\":\"turn_started\"")) return 1;
  if (has("\"type\":\"task_complete\"") || has("\"type\":\"turn_complete\"") ||
      has("\"type\":\"turn_aborted\""))
    return -1;
  return 0;
}

}  // namespace mico
