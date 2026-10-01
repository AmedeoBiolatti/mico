#include "adapters/adapters.h"
#include "base/fs.h"
#include "base/log.h"
#include "core/session.h"
#include "core/store.h"
#include "vt/keys.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <set>
#include <map>
#include <cstdlib>
#include <functional>
#include "base/json.h"

namespace mico {

// Told to agents at launch so they can use what mico draws. Short on purpose:
// it is in every request the agent makes.
const char* const kAgentHints =
    "You are running inside mico, a terminal UI that renders your replies. It draws "
    "charts: to show one, write a fenced code block with the language `chart` holding JSON, "
    "e.g. {\"type\": \"line\", \"title\": \"loss\", \"x\": [0, 1, 2], \"series\": "
    "[{\"name\": \"train\", \"y\": [2.1, 1.4, 0.9]}], \"xlabel\": \"step\", \"ylabel\": \"loss\"}. "
    "Types: line, scatter, bar (bar takes \"labels\": [...] and values in \"series\"), hist (\"values\"), "
    "spark (one-line sparklines), heatmap (\"z\": rows, \"labels\", \"ylabels\"). Options: "
    "\"log_y\": true, \"height\": rows, \"marker\": \"braille\"|\"line\"|\"block\" (leave it out unless asked). "
    "Several charts side by side: {\"subplots\": [chart, chart], \"columns\": n}. For data in a CSV (with a header) or JSONL file, give "
    "{\"type\": \"line\", \"file\": \"path\", \"x\": \"step\", \"y\": [\"loss\", \"val_loss\"]} "
    "instead of inline numbers: mico reads it itself and redraws when it changes, so it follows a "
    "running job. Use a chart when a trend or a comparison reads better as one; keep inline data "
    "to a few hundred points. A fenced block with the language `mermaid` (flowchart, stateDiagram, "
    "sequenceDiagram) is drawn as a diagram.";

const char* const kMcpHint =
    " You also have mico's `plot` tool (MCP server \"mico\"): it takes the same fields and draws the "
    "chart in the chat; prefer it to writing the block.";

// The mico binary itself, which agents start as their MCP server.
const std::string& self_exe() {
  static const std::string path = [] {
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    return n > 0 ? std::string(buf, size_t(n)) : std::string();
  }();
  return path;
}

std::string json_quote(std::string_view v) {
  std::string out = "\"";
  for (const char c : v) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out + '"';
}

bool mcp_tools_enabled() {
  std::string buf;
  return fs::read_prefix(config_dir() + "/mcp", 64, buf).starts_with("on");
}

void set_mcp_tools(bool on) {
  mkdir(config_dir().c_str(), 0700);
  if (FILE* f = fopen((config_dir() + "/mcp").c_str(), "w")) {
    fputs(on ? "on\n" : "off\n", f);
    fclose(f);
  }
}

bool agent_hints_enabled() {
  std::string buf;
  const std::string_view v = fs::read_prefix(config_dir() + "/agent-hints", 64, buf);
  return !v.starts_with("off");
}

void set_agent_hints(bool on) {
  mkdir(config_dir().c_str(), 0700);
  if (FILE* f = fopen((config_dir() + "/agent-hints").c_str(), "w")) {
    fputs(on ? "on\n" : "off\n", f);
    fclose(f);
  }
}

namespace {

int64_t now_ms() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// Transcripts already spoken for by another pane. Two codex sessions started in
// the same directory at the same moment can no longer land on the same file.
std::map<std::string, const LiveSession*>& claimed_transcripts() {
  static std::map<std::string, const LiveSession*> s;
  return s;
}

void for_each_rollout(const std::function<void(const std::string&)>& fn) {
  const std::string root = fs::home() + "/.codex/sessions";
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

// pi and omp both lay sessions out flat as sessions/<cwd-slug>/<file>.jsonl —
// one level, unlike codex's year/month/day tree.
void for_each_pi_family_session(const std::string& root,
                                const std::function<void(const std::string&)>& fn) {
  fs::list_dir(root, true, [&](const std::string& slug) {
    const std::string dir = root + "/" + slug;
    fs::list_dir(dir, false, [&](const std::string& fn2) {
      if (fs::has_suffix(fn2, ".jsonl")) fn(dir + "/" + fn2);
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

}  // namespace

LiveSession::~LiveSession() {
  auto it = claimed_transcripts().find(transcript_);
  if (it != claimed_transcripts().end() && it->second == this) claimed_transcripts().erase(it);
}

std::string make_uuid_v4() {
  unsigned char b[16];
  int fd = ::open("/dev/urandom", O_RDONLY);
  if (fd < 0 || read(fd, b, sizeof b) != (ssize_t)sizeof b) {
    for (auto& x : b) x = (unsigned char)(rand() & 0xFF);
  }
  if (fd >= 0) ::close(fd);
  b[6] = (b[6] & 0x0F) | 0x40;  // version 4
  b[8] = (b[8] & 0x3F) | 0x80;  // variant 1
  char out[37];
  snprintf(out, sizeof out,
           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11],
           b[12], b[13], b[14], b[15]);
  return out;
}

bool LiveSession::start(const Launch& l) {
  agent_ = l.agent;
  cwd_ = l.cwd;
  origin_ = l.origin;
  forked_ = l.forked;
  session_id_ = l.session_id;
  started_at_ = int64_t(time(nullptr));
  used_at_ = started_at_;
  turn_open_ = false;
  turns_from_end_ = !session_id_.empty() || forked_;
  turn_file_.clear();
  turn_head_.clear();
  turn_read_ = 0;

  // The adapter follows the agent, never the argv. A resumed or forked session
  // is still a claude session and still gets a chat view.
  adapter_ = adapter_for(agent_);

  std::vector<std::string> argv = l.argv;
  if (argv.empty()) {
    if (agent_ == "claude") {
      // Pre-assigning the id makes correlation exact: no races, no guessing
      // which of several concurrently-started sessions is ours.
      if (session_id_.empty()) session_id_ = make_uuid_v4();
      argv = {"claude", "--session-id", session_id_};
    } else if (agent_ == "pi") {
      // pi honours the same "use this exact id, creating it if missing" deal.
      if (session_id_.empty()) session_id_ = make_uuid_v4();
      argv = {"pi", "--session-id", session_id_};
    } else if (agent_ == "codex" || agent_ == "omp") {
      // Neither offers a pre-assignable id, so the transcript is discovered
      // after the fact by cwd and spawn time.
      argv = {agent_};
    } else {
      argv = {agent_};
    }
  }

  // However it was launched, a claude or pi session gets an id we chose, so
  // the chat view can find its transcript. Without this, "claude --model x"
  // typed into the new-agent prompt would render raw-only.
  if ((agent_ == "claude" || agent_ == "pi") && session_id_.empty()) {
    bool has_flag = false;
    for (size_t i = 0; i < argv.size(); i++) {
      if (argv[i] == "--session-id" && i + 1 < argv.size()) {
        has_flag = true;
        session_id_ = argv[i + 1];
      } else if (argv[i].starts_with("--session-id=")) {
        has_flag = true;
        session_id_ = argv[i].substr(13);
      }
    }
    if (!has_flag) {
      session_id_ = make_uuid_v4();
      argv.push_back("--session-id");
      argv.push_back(session_id_);
    }
  }

  // Claude keeps an AskUserQuestion call in memory while its dialog is up and
  // writes it only with the answer, so the chat saw nothing and the agent
  // looked stopped. It does flush the transcript before running a hook (hooks
  // are handed transcript_path), so a do-nothing PreToolUse hook on that one
  // tool lands the call in the transcript before the dialog opens. The same
  // goes for a call waiting on permission: the PermissionRequest hook fires
  // only when the dialog is about to show, so the chat can show the command
  // or edit being approved, not an agent that seems to have stopped. Flag
  // settings merge with the user's own and are never written to ~/.claude.
  // A command line that already brings --settings is left alone: claude reads
  // only one. So is any program that is not claude itself.
  if (agent_ == "claude" && !argv.empty() &&
      std::string_view(argv[0]).substr(argv[0].rfind('/') + 1) == "claude" &&
      std::none_of(argv.begin(), argv.end(),
                   [](const std::string& a) { return a == "--settings" || a.starts_with("--settings="); })) {
    argv.push_back("--settings");
    argv.push_back(R"({"hooks":{"PreToolUse":[{"matcher":"AskUserQuestion",)"
                   R"("hooks":[{"type":"command","command":"true"}]}],)"
                   R"("PermissionRequest":[{"matcher":"",)"
                   R"("hooks":[{"type":"command","command":"true"}]}]}})");
  }

  // What mico can show, told to the agent so it can use it: charts, today.
  // Appended, never replacing: claude's own system prompt stays, and codex's
  // developer_instructions are set only when the command line does not.
  const std::string prog = argv.empty() ? std::string() : argv[0].substr(argv[0].rfind('/') + 1);
  const auto has = [&](std::string_view needle) {
    return std::any_of(argv.begin(), argv.end(),
                       [&](const std::string& a) { return a.find(needle) != std::string::npos; });
  };
  const auto toml = [](std::string_view v) {
    std::string out = "\"";
    for (const char c : v) {
      if (c == '"' || c == '\\') out += '\\';
      if (c == '\n') { out += "\\n"; continue; }
      out += c;
    }
    return out + '"';
  };
  // mico's own tools, as an MCP server (`mico --mcp`), when the user turned
  // them on with `:mcp on`. Handed over on the command line, never written
  // into the agent's config; claude is also allowed to call plot without a
  // prompt, since drawing a chart changes nothing.
  const bool mcp = mcp_tools_enabled() && !self_exe().empty();
  if (mcp && agent_ == "claude" && prog == "claude") {
    argv.push_back("--mcp-config={\"mcpServers\":{\"mico\":{\"type\":\"stdio\",\"command\":" +
                   json_quote(self_exe()) + ",\"args\":[\"--mcp\"]}}}");
    argv.push_back("--allowedTools=mcp__mico__plot");
  } else if (mcp && agent_ == "codex" && prog == "codex" && !has("mcp_servers.mico")) {
    // codex asks before every MCP call; plot is approved up front, as for claude.
    argv.insert(argv.begin() + 1, {"-c", "mcp_servers.mico.command=" + toml(self_exe()), "-c",
                                   "mcp_servers.mico.args=[\"--mcp\"]", "-c",
                                   "mcp_servers.mico.tools.plot.approval_mode=\"approve\""});
  }

  if (agent_hints_enabled() && !argv.empty()) {
    const std::string hints = std::string(kAgentHints) + (mcp ? kMcpHint : "");
    if (agent_ == "claude" && prog == "claude" && !has("system-prompt")) {
      argv.push_back("--append-system-prompt");
      argv.push_back(hints);
    } else if (agent_ == "codex" && prog == "codex" && !has("developer_instructions")) {
      argv.insert(argv.begin() + 1, {"-c", "developer_instructions=" + toml(hints)});
    }
  }

  if (agent_ == "codex") {
    for_each_rollout([&](const std::string& path) { preexisting_.push_back(path); });
    std::sort(preexisting_.begin(), preexisting_.end());
  } else if (agent_ == "omp") {
    for_each_pi_family_session(fs::home() + "/.omp/agent/sessions",
                               [&](const std::string& path) { preexisting_.push_back(path); });
    std::sort(preexisting_.begin(), preexisting_.end());
  }

  argv_ = std::move(argv);
  {
    std::string j;
    for (auto& a : argv_) { j += a; j += ' '; }
    MLOG("session start: agent=%s cwd=%s id=%s argv=[ %s]", agent_.c_str(), cwd_.c_str(),
         session_id_.c_str(), j.c_str());
  }
  return true;
}

bool LiveSession::restart(const Launch& l) {
  if (!pty_.reset_exited()) return false;
  auto claim = claimed_transcripts().find(transcript_);
  if (claim != claimed_transcripts().end() && claim->second == this)
    claimed_transcripts().erase(claim);
  transcript_.clear();
  preexisting_.clear();
  vt_ = Vt{};
  buf_.clear();
  cancel_answer();
  answer_failed_ = false;
  spawned_ = was_working_ = was_exited_ = unseen_ = false;
  trust_tries_ = 0;
  trust_next_ms_ = 0;
  last_probe_ = last_output_ms_ = 0;
  last_w_ = last_h_ = 0;
  return start(l);
}

bool LiveSession::set_geometry(int w, int h) {
  w = w > 0 ? w : 80;
  h = h > 0 ? h : 24;
  if (spawned_) {
    if (w == last_w_ && h == last_h_) return false;
    last_w_ = w;
    last_h_ = h;
    vt_.resize(w, h);
    pty_.resize(w, h);
    return false;
  }
  spawned_ = true;
  last_w_ = w;
  last_h_ = h;
  vt_.resize(w, h);
  MLOG("session spawn: %s @ %dx%d", agent_.c_str(), w, h);
  pty_.spawn(argv_, cwd_, w, h);
  return true;
}

std::string LiveSession::label() const {
  std::string s = agent_;
  if (forked_ && !origin_.empty()) s += " fork of " + origin_.substr(0, 8);
  else if (!origin_.empty()) s += " resumed";
  if (!pty_.spawn_error().empty()) s += " (can't start)";
  else if (pty_.exited()) s += " (exited)";
  else if (transcript_.empty() && adapter_) s += " (linking…)";
  return s;
}

// Claude: the transcript is exactly <uuid>.jsonl, so search for that name and
// accept no substitutes. Codex resumes match by id; new runs additionally
// require the rollout to be open in this agent's own process session.
void LiveSession::discover_transcript() {
  if (!adapter_ || !transcript_.empty()) return;
  int64_t t = now_ms();
  if (t - last_probe_ < 500) return;
  last_probe_ = t;

  if (agent_ == "claude") {
    const std::string root = fs::home() + "/.claude/projects";
    fs::list_dir(root, true, [&](const std::string& slug) {
      if (!transcript_.empty()) return;
      std::string cand = root + "/" + slug + "/" + session_id_ + ".jsonl";
      if (fs::exists(cand) && !claimed_transcripts().count(cand)) transcript_ = cand;
    });
    if (!transcript_.empty()) {
      claimed_transcripts()[transcript_] = this;
      MLOG("transcript linked: %s -> %s", agent_.c_str(), transcript_.c_str());
    }
    return;
  }

  if (agent_ == "pi") {
    // Same deal as claude, but the filename carries a timestamp prefix we
    // don't know in advance: "<timestamp>_<session_id>.jsonl". The id suffix
    // still identifies it exactly.
    const std::string root = fs::home() + "/.pi/agent/sessions";
    const std::string suffix = "_" + session_id_ + ".jsonl";
    fs::list_dir(root, true, [&](const std::string& slug) {
      if (!transcript_.empty()) return;
      const std::string dir = root + "/" + slug;
      fs::list_dir(dir, false, [&](const std::string& fn2) {
        if (!transcript_.empty() || !fs::has_suffix(fn2, suffix)) return;
        const std::string cand = dir + "/" + fn2;
        if (!claimed_transcripts().count(cand)) transcript_ = cand;
      });
    });
    if (!transcript_.empty()) {
      claimed_transcripts()[transcript_] = this;
      MLOG("transcript linked: pi -> %s", transcript_.c_str());
    }
    return;
  }

  if (agent_ == "omp") {
    // No pre-assignable id: find the newest unclaimed session that appeared
    // after we spawned and whose recorded cwd is ours — the same trick as
    // codex, but omp sometimes writes a "title" record before "session", so
    // the match has to scan a few lines rather than just the first one.
    std::string best, best_sid;
    int64_t best_mtime = 0;
    const std::string root = fs::home() + "/.omp/agent/sessions";
    for_each_pi_family_session(root, [&](const std::string& path) {
      const bool resuming = !session_id_.empty() && !forked_;
      if (!resuming && std::binary_search(preexisting_.begin(), preexisting_.end(), path)) return;
      if (claimed_transcripts().count(path)) return;

      struct stat st{};
      if (stat(path.c_str(), &st) != 0) return;
      if (!resuming && int64_t(st.st_mtime) + 5 < started_at_) return;
      if (int64_t(st.st_mtime) < best_mtime) return;

      thread_local std::string buf;
      std::string_view head = fs::read_prefix(path, 4 << 10, buf);
      bool match = false;
      std::string sid;
      fs::for_each_line(head, [&](std::string_view line) {
        if (line.find("\"type\":\"session\"") == std::string_view::npos) return true;
        std::string cwd;
        js::scan_object(line, [&](std::string_view k, const js::Value& v) {
          if (k == "cwd") js::unescape_append(v.body(), cwd);
          else if (k == "id") sid = std::string(v.body());
          return true;
        });
        match = resuming ? sid == session_id_ : cwd == cwd_;
        return false;
      });
      if (!match) return;

      best = path;
      best_sid = sid;
      best_mtime = int64_t(st.st_mtime);
    });
    if (!best.empty()) {
      transcript_ = best;
      session_id_ = best_sid;
      claimed_transcripts()[transcript_] = this;
      MLOG("transcript linked: omp -> %s", transcript_.c_str());
    }
    return;
  }

  if (agent_ != "codex") return;

  std::string best, best_sid;
  const auto owned = session_id_.empty() ? open_transcripts(pty_.pid()) : std::set<std::string>{};
  int64_t best_mtime = 0;
  for_each_rollout([&](const std::string& path) {
    // Anything that existed before we spawned belongs to someone else, however
    // recently it was written to.
    const bool resuming = !session_id_.empty() && !forked_;
    if (!resuming && !owned.count(path)) return;
    if (!resuming && std::binary_search(preexisting_.begin(), preexisting_.end(), path)) return;
    if (claimed_transcripts().count(path)) return;

    struct stat st{};
    if (stat(path.c_str(), &st) != 0) return;
    if (!resuming && int64_t(st.st_mtime) + 5 < started_at_) return;
    if (int64_t(st.st_mtime) < best_mtime) return;

    thread_local std::string buf;
    std::string_view head = fs::read_first_line(path, buf);
    if (head.empty()) return;

    js::Value payload{};
    js::scan_object(head,
                    [&](std::string_view k, const js::Value& v) {
                      if (k != "payload") return true;
                      payload = v;
                      return false;
                    });
    if (!payload.is_object()) return;

    std::string cwd;
    std::string_view sid, forked_from;
    js::scan_object(payload.raw, [&](std::string_view k, const js::Value& v) {
      if (k == "cwd") js::unescape_append(v.body(), cwd);
      else if (k == "id" || k == "session_id") sid = v.body();
      else if (k == "forked_from_id") forked_from = v.body();
      return true;
    });
    if (sid.empty()) return;
    if (resuming ? sid != session_id_ : cwd != cwd_) return;
    // A fork records what it came from, which identifies ours exactly even if
    // several start together.
    if (forked_ && !origin_.empty() && forked_from != origin_) return;

    best = path;
    best_sid = std::string(sid);
    best_mtime = int64_t(st.st_mtime);
  });

  if (!best.empty()) {
    transcript_ = best;
    session_id_ = best_sid;
    claimed_transcripts()[transcript_] = this;
    MLOG("transcript linked: codex -> %s", transcript_.c_str());
  }
}

bool LiveSession::busy() const {
  if (!spawned_) return false;
  if (pty_.exited()) return false;
  // Claude redraws its prompt, notices and timers even while idle. Output
  // recency is not evidence of work; follow its live spinner instead. This
  // also keeps a quiet model request active until Claude clears the footer.
  if (agent_ == "claude") return screen_shows_claude_activity(vt_);
  // Codex redraws while it is typed into and at startup, so output is no
  // evidence either. Its status line is, and the transcript bridges the
  // moments it is off screen. Not an open turn alone: one whose end never
  // reached the transcript must not read as working forever, and a turn in
  // progress keeps its status line's clock ticking.
  if (agent_ == "codex")
    return screen_shows_codex_activity(vt_) || (turn_open_ && now_ms() - last_output_ms_ < 10000);
  // A turn produces output continuously; the gap only exceeds this when the
  // agent is sitting at its prompt.
  return now_ms() - last_output_ms_ < 1200;
}

bool LiveSession::needs_input() const {
  // Not gated on busy(): a trust dialog with a live countdown keeps redrawing,
  // which would read as "working" and hide the very prompt that needs an
  // answer. The screen-content check is specific enough to stand alone.
  if (!spawned_ || pty_.exited()) return false;
  // Codex's dialogs replace its input box. Words like "Do you want to" in the
  // rows above are as likely its own reply.
  if (agent_ == "codex") return screen_awaits_codex_input(vt_);
  return screen_awaits_input(vt_);
}

void LiveSession::follow_turns() {
  if (agent_ != "codex" || transcript_.empty()) return;
  const int64_t now = now_ms();
  if (transcript_ == turn_file_ && now - turn_poll_ms_ < 150) return;
  turn_poll_ms_ = now;
  struct stat st{};
  if (stat(transcript_.c_str(), &st) != 0) return;
  const uint64_t size = uint64_t(st.st_size);
  if (transcript_ != turn_file_) {
    turn_file_ = transcript_;
    turn_open_ = false;
    turn_head_.clear();
    turn_read_ = turns_from_end_ ? size : 0;
  }
  if (size <= turn_read_) return;
  const int fd = open(transcript_.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return;
  // The record's type comes first: {"timestamp":…,"type":"event_msg",
  // "payload":{"type":"task_started",…. The rest of a line can be megabytes.
  constexpr size_t kHead = 200;
  char buf[65536];
  while (turn_read_ < size) {
    const ssize_t n = pread(fd, buf, std::min<uint64_t>(sizeof buf, size - turn_read_), off_t(turn_read_));
    if (n <= 0) break;
    turn_read_ += uint64_t(n);
    std::string_view chunk(buf, size_t(n));
    while (!chunk.empty()) {
      const size_t nl = chunk.find('\n');
      const std::string_view part = chunk.substr(0, nl);
      if (turn_head_.size() < kHead) turn_head_.append(part.substr(0, kHead - turn_head_.size()));
      if (nl == std::string_view::npos) break;
      chunk.remove_prefix(nl + 1);
      auto has = [&](std::string_view m) { return turn_head_.find(m) != std::string::npos; };
      if (has("\"type\":\"task_started\"") || has("\"type\":\"turn_started\"")) turn_open_ = true;
      else if (has("\"type\":\"task_complete\"") || has("\"type\":\"turn_complete\"") ||
               has("\"type\":\"turn_aborted\""))
        turn_open_ = false;
      turn_head_.clear();
    }
  }
  close(fd);
}

bool LiveSession::send_parts(const std::vector<MessagePart>& parts, std::string_view first) {
  if (!spawned_ || pty_.exited() || message_sending()) return false;
  used_at_ = int64_t(time(nullptr));
  const bool bracketed = vt_.bracketed_paste();
  bool has_image = false;
  for (const auto& p : parts) has_image |= p.image;
  if (!first.empty()) {
    std::vector<Step> steps{{std::string(first), 300}};
    for (const auto& p : parts) steps.push_back({encode_paste(p.text, bracketed), p.image ? 400 : 120});
    steps.push_back({"\r", 0});
    return send_message(std::move(steps));
  }
  if (!has_image) {
    std::string all;
    for (const auto& p : parts) all += p.text;
    pty_.write(encode_paste(all, bracketed));
    pty_.write("\r");
    return true;
  }
  std::vector<Step> steps;
  for (const auto& p : parts) steps.push_back({encode_paste(p.text, bracketed), p.image ? 400 : 120});
  steps.push_back({"\r", 0});
  return send_message(std::move(steps));
}

std::vector<MessagePart> LiveSession::take_last_queued() {
  if (queue_.empty()) return {};
  std::vector<MessagePart> last = std::move(queue_.back());
  queue_.pop_back();
  return last;
}

void LiveSession::flush_queue() {
  if (queue_.empty() || !spawned_ || pty_.exited()) return;
  if (message_sending() || answer_sending() || busy() || needs_input()) return;
  // One per turn. The last one sent has to have started a turn (the work
  // generation moves) before the next goes; a message that starts none, like
  // a slash command handled locally, releases the next after a few seconds.
  if (queue_waiting_ && work_generation_ == queue_gen_ && now_ms() - queue_sent_ms_ < 5000) return;
  if (!send_parts(queue_.front())) return;
  queue_.erase(queue_.begin());
  queue_waiting_ = true;
  queue_gen_ = work_generation_;
  queue_sent_ms_ = now_ms();
}

bool LiveSession::send_message(std::vector<Step> steps) {
  if (!spawned_ || pty_.exited() || message_sending() || steps.empty()) return false;
  msg_steps_ = std::move(steps);
  msg_step_ = 0;
  msg_key_ms_ = 0;  // the first step goes at once
  msg_feedback_ = false;
  return true;
}

bool LiveSession::send_answer(std::vector<std::string> steps) {
  if (!spawned_ || pty_.exited() || answer_sending() || steps.empty()) return false;
  answer_steps_ = std::move(steps);
  answer_step_ = 0;
  answer_key_ms_ = now_ms();
  answer_feedback_ = false;
  answer_failed_ = false;
  return true;
}

void LiveSession::cancel_answer() {
  answer_steps_.clear();
  answer_step_ = 0;
}

void LiveSession::send_after_answer(std::string text) {
  after_answer_ = std::move(text);
  after_answer_ms_ = now_ms() + 400;
}

LiveSession::Status LiveSession::status() const {
  if (pty_.exited()) return Status::Exited;
  if (needs_input()) return Status::Waiting;
  if (busy()) return Status::Working;
  return Status::Idle;
}

bool LiveSession::pump() {
  if (!spawned_) return false;
  pty_.poll_exit();
  pty_.flush_input();
  if (pty_.exited()) {
    auto it = claimed_transcripts().find(transcript_);
    if (it != claimed_transcripts().end() && it->second == this) claimed_transcripts().erase(it);
  }
  buf_.clear();
  if (!pty_.read_available(buf_) && buf_.empty()) {
    // EOF: the child closed its terminal.
  }
  if (!buf_.empty()) {
    vt_.write(buf_);
    last_output_ms_ = now_ms();
    if (answer_step_ > 0) answer_feedback_ = true;
    if (msg_step_ > 0) msg_feedback_ = true;
  }

  bool answer_changed = false;
  if (message_sending()) {
    const int64_t now = now_ms();
    if (pty_.exited()) {
      msg_steps_.clear();
    } else {
      const Step* prev = msg_step_ > 0 ? &msg_steps_[msg_step_ - 1] : nullptr;
      const int64_t since = now - msg_key_ms_;
      // After the agent has visibly taken the last step and gone quiet, or
      // after a second and a half regardless: a step that changed nothing on
      // screen must not strand the rest of the message.
      const bool ready = !prev || (since >= prev->settle_ms &&
                                   ((msg_feedback_ && now - last_output_ms_ >= 40) || since >= 1500));
      if (ready) {
        pty_.write(msg_steps_[msg_step_++].bytes);
        msg_key_ms_ = now;
        msg_feedback_ = false;
        if (msg_step_ == msg_steps_.size()) {
          msg_steps_.clear();
          msg_step_ = 0;
        }
        answer_changed = true;
      }
    }
  }
  if (answer_sending()) {
    const int64_t now = now_ms();
    // Wait for a terminal update and a quiet frame before each subsequent
    // key. In particular, the review screen must mount before its Enter.
    if (pty_.exited() || now - answer_key_ms_ > 3000) {
      cancel_answer();
      answer_failed_ = true;
      answer_changed = true;
    } else if (now - answer_key_ms_ >= 120 && now - last_output_ms_ >= 40 &&
               (answer_step_ == 0 || answer_feedback_)) {
      if (answer_step_ == answer_steps_.size()) {
        cancel_answer();
        // The form is gone; give the agent's prompt a moment to come back.
        after_answer_ms_ = now + 250;
      } else {
        pty_.write(answer_steps_[answer_step_++]);
        answer_key_ms_ = now;
        answer_feedback_ = false;
      }
      answer_changed = true;
    }
  }

  // The folder is on the user's tracked list, which is trust enough: answer
  // Claude's first-run "do you trust this folder" dialog for them, so it does
  // not time out and kill the session while nobody is looking at it. Only
  // before the first transcript line, which is exactly the startup window.
  //
  // The dialog's cursor starts on "No, exit": Enter there quits claude. And
  // a key sent before the dialog listens is dropped, so a scripted
  // "down, enter" can land as a bare Enter. So one step at a time, each
  // judged from the screen: an arrow while the cursor is elsewhere, and Enter
  // only once the screen shows it on "Yes" and has been still for a moment.
  if (agent_ == "claude" && transcript_.empty() && !answer_sending() && trust_tries_ < 20 &&
      now_ms() >= trust_next_ms_ && now_ms() - last_output_ms_ >= 250 &&
      screen_is_trust_prompt(vt_)) {
    const int moves = trust_prompt_moves(vt_);
    if (moves != kNoTrustMove) {
      pty_.write(moves > 0 ? "\x1b[B" : moves < 0 ? "\x1b[A" : "\r");
      trust_tries_++;
      trust_next_ms_ = now_ms() + (moves == 0 ? 3000 : 300);
      if (moves == 0) MLOG("accepted the trust prompt for %s", cwd_.c_str());
    }
  }

  follow_turns();
  // Mark the moment a turn ends, not the state itself: that is what the user
  // missed while their attention was on another pane.
  const bool working = busy();
  if (working && !was_working_) ++work_generation_;
  if (pty_.exited() && !was_exited_) {
    std::string tail;
    for (int y = 0; y < vt_.total_rows(); y++) {
      std::string r;
      for (const Cell& c : vt_.row(y)) {
        if (c.width == 0) continue;
        r.push_back(c.cp >= 0x20 && c.cp < 0x7f ? char(c.cp) : ' ');
      }
      while (!r.empty() && r.back() == ' ') r.pop_back();
      if (!r.empty()) {
        if (!tail.empty()) tail += " | ";
        tail += r;
      }
    }
    if (tail.size() > 400) tail = tail.substr(tail.size() - 400);
    MLOG("session EXITED: %s code=%d  last screen: %s", agent_.c_str(), pty_.exit_status(),
         tail.empty() ? "(blank)" : tail.c_str());
  }
  const bool changed = answer_changed || !buf_.empty() || working != was_working_ || pty_.exited() != was_exited_;
  if (was_working_ && !working) unseen_ = true;
  was_working_ = working;
  was_exited_ = pty_.exited();

  // Notes follow the answer as a message of their own. While the agent works
  // on, that is steering: claude reads it at its next step, right after the
  // answer. A failed answer keeps them for the pane to hand back.
  if (!after_answer_.empty() && !answer_sending() && !answer_failed_ && !message_sending() &&
      now_ms() >= after_answer_ms_ && !pty_.exited()) {
    if (send_parts({MessagePart{false, after_answer_}})) after_answer_.clear();
    answer_changed = true;
  }

  const size_t held = queue_.size();
  flush_queue();

  const std::string before = transcript_;
  discover_transcript();
  return changed || queue_.size() != held || transcript_ != before;
}

}  // namespace mico
