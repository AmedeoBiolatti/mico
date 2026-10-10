#include "adapters/pi/pi.h"

#include <sys/stat.h>

#include "adapters/cmdline.h"
#include "base/fs.h"
#include "base/json.h"
#include "base/uuid.h"

namespace mico {
namespace {

// A non-empty environment variable, or null.
const char* env(const char* name) {
  const char* v = getenv(name);
  return v && *v ? v : nullptr;
}

// `p` with a leading "~" made the home directory and, when still relative,
// made absolute against `base` (unchanged without one).
std::string absolute(std::string p, const std::string& base) {
  if (p == "~" || p.starts_with("~/")) p = fs::home() + p.substr(1);
  if (!p.empty() && p[0] != '/' && !base.empty()) p = base + "/" + p;
  while (p.size() > 1 && p.back() == '/') p.pop_back();
  return p;
}

// The folder --session-dir names on a command line, or empty.
std::string session_dir_flag(const std::vector<std::string>& argv) {
  std::string dir;
  for (size_t i = 0; i < argv.size(); i++) {
    if (argv[i] == "--session-dir" && i + 1 < argv.size()) dir = argv[i + 1];
    else if (argv[i].starts_with("--session-dir=")) dir = argv[i].substr(14);
  }
  return dir;
}

// The "session" record at the head of a transcript: its id and cwd, and the
// transcript of the session that spawned it, for an omp subagent's. omp
// sometimes writes a "title" record first, so the head is scanned a few lines
// deep rather than just the first one. False when there is none.
struct SessionHead {
  std::string id, cwd, parent;
};
bool read_head(const std::string& path, SessionHead& out) {
  thread_local std::string buf;
  std::string_view head = fs::read_prefix(path, 4 << 10, buf);
  bool found = false;
  fs::for_each_line(head, [&](std::string_view line) {
    if (line.find("\"type\":\"session\"") == std::string_view::npos) return true;
    js::scan_object(line, [&](std::string_view k, const js::Value& v) {
      if (k == "cwd") js::unescape_append(v.body(), out.cwd);
      else if (k == "id") out.id = std::string(v.body());
      else if (k == "parentSession") js::unescape_append(v.body(), out.parent);
      return true;
    });
    found = true;
    return false;
  });
  return found;
}

}  // namespace

std::string PiFamilyAdapter::sessions_dir(const std::vector<std::string>*) const { return agent_dir(fs::home()) + "/sessions"; }

void PiFamilyAdapter::for_each_session(const std::function<void(const std::string&)>& fn,
                                       const std::vector<std::string>* argv,
                                       const std::string& cwd) const {
  const std::string root = sessions_dir(argv);
  fs::list_dir(root, true, [&](const std::string& slug) {
    const std::string dir = root + "/" + slug;
    fs::list_dir(dir, false, [&](const std::string& name) {
      if (fs::has_suffix(name, ".jsonl")) fn(dir + "/" + name);
    });
  });
  // Both agents keep an overridden folder flat: no per-cwd slug inside it.
  std::string flat;
  if (argv) flat = session_dir_flag(*argv);
  if (!flat.empty()) flat = absolute(flat, cwd);
  else if (const char* e = env("PI_CODING_AGENT_SESSION_DIR")) flat = absolute(e, cwd);
  if (flat.empty()) return;
  // Only a direct child of root was already scanned as a cwd slug. The
  // root itself and deeper overrides can contain flat transcripts too.
  if (flat.starts_with(root + "/") && flat.find('/', root.size() + 1) == std::string::npos) return;
  fs::list_dir(flat, false, [&](const std::string& name) {
    if (fs::has_suffix(name, ".jsonl")) fn(flat + "/" + name);
  });
}

void PiFamilyAdapter::add_extras(Launch& l, const LaunchExtras& x) const {
  std::vector<std::string>& argv = l.argv;
  if (cmdline::program(argv) != id()) return;
  const bool tool = !x.tool_extension.empty();
  if (tool) argv.insert(argv.begin() + 1, {"-e", x.tool_extension});
  // Appended to the agent's own system prompt, unless the command line
  // already sets one.
  if (!x.hints.empty() && !cmdline::mentions(argv, "system-prompt")) {
    std::string hints = x.hints;
    if (tool)
      hints += " You also have mico's `mico_plot` tool: it takes the same fields and draws the chart in the "
               "chat; prefer it to writing the block.";
    argv.insert(argv.begin() + 1, {"--append-system-prompt", hints});
  }
}

void PiFamilyAdapter::for_each_subagent(const std::string& path,
                                        const std::function<void(const std::string&)>& fn) {
  if (!fs::has_suffix(path, ".jsonl")) return;
  const std::string dir = path.substr(0, path.size() - 6);
  fs::list_dir(dir, false, [&](const std::string& name) {
    if (fs::has_suffix(name, ".jsonl")) fn(dir + "/" + name);
  });
}

// omp's task call names each subagent it starts ("tasks": [{"name":
// "MarketReview", "agent": "reviewer", …}], the agent given once for all of
// them or per task), and each writes <transcript minus .jsonl>/<name>.jsonl.
void PiFamilyAdapter::call_subagents(const std::string& path, std::string_view line, uint64_t tool_id,
                                     std::vector<SubagentRun>& out) const {
  if (!fs::has_suffix(path, ".jsonl") || line.find("\"task\"") == std::string_view::npos) return;
  const std::string dir = path.substr(0, path.size() - 6) + "/";
  js::Value message{};
  js::scan_object(line, [&](std::string_view k, const js::Value& v) {
    if (k == "message") { message = v; return false; }
    return true;
  });
  js::Value content{};
  if (message.is_object())
    js::scan_object(message.raw, [&](std::string_view k, const js::Value& v) {
      if (k == "content") content = v;
      return true;
    });
  if (!content.is_array()) return;
  js::scan_array(content.raw, [&](const js::Value& item) {
    if (!item.is_object()) return true;
    std::string_view name, id;
    js::Value args{};
    js::scan_object(item.raw, [&](std::string_view k, const js::Value& v) {
      if (k == "name") name = v.body();
      else if (k == "id") id = v.body();
      else if (k == "arguments") args = v;
      return true;
    });
    if (name != "task" || hash_id(id) != tool_id || !args.is_object()) return true;
    std::string agent;
    js::Value tasks{};
    js::scan_object(args.raw, [&](std::string_view k, const js::Value& v) {
      if (k == "agent" && v.is_string()) js::unescape_append(v.body(), agent);
      else if (k == "tasks") tasks = v;
      return true;
    });
    if (tasks.is_array())
      js::scan_array(tasks.raw, [&](const js::Value& t) {
        if (!t.is_object()) return true;
        SubagentRun r;
        js::scan_object(t.raw, [&](std::string_view k, const js::Value& v) {
          if (k == "name" && v.is_string()) js::unescape_append(v.body(), r.name);
          else if (k == "agent" && v.is_string()) js::unescape_append(v.body(), r.kind);
          return true;
        });
        // A name is how its transcript is found; one that would leave the
        // folder is not a name.
        if (r.name.empty() || r.name.find('/') != std::string::npos || r.name.starts_with('.')) return true;
        if (r.kind.empty()) r.kind = agent;
        r.id = r.name;
        r.path = dir + r.name + ".jsonl";
        out.push_back(std::move(r));
        return true;
      });
    return false;
  });
}

// --- pi ----------------------------------------------------------------------

std::string PiAdapter::agent_dir(const std::string& home) const {
  if (const char* d = env("PI_CODING_AGENT_DIR")) return absolute(d, {});
  return home + "/.pi/agent";
}

void PiAdapter::prepare(Launch& l, const LaunchExtras& x) const {
  if (l.argv.empty()) {
    // pi honours the same "use this exact id, creating it if missing" deal as
    // claude.
    if (l.session_id.empty()) l.session_id = make_uuid_v4();
    l.argv = {"pi", "--session-id", l.session_id};
  }
  if (l.session_id.empty()) cmdline::adopt_session_id(l.argv, l.session_id);
  add_extras(l, x);
}

bool PiAdapter::continue_session(Launch& l, std::string_view id, bool fork, std::string*) const {
  if (fork) {
    l.session_id = make_uuid_v4();
    l.argv = {"pi", "--fork", std::string(id), "--session-id", l.session_id};
  } else {
    l.session_id = id;  // resuming keeps the original transcript
    l.argv = {"pi", "--session", std::string(id)};
  }
  return true;
}

// Same deal as claude, but the filename carries a timestamp prefix we don't
// know in advance: "<timestamp>_<session_id>.jsonl". The id suffix still
// identifies it exactly.
bool PiAdapter::find_transcript(const TranscriptQuery& q, FoundTranscript& out) const {
  const std::string suffix = "_" + q.session_id + ".jsonl";
  for_each_session([&](const std::string& path) {
    if (out.path.empty() && fs::has_suffix(path, suffix) && !q.claimed(path)) out.path = path;
  }, q.argv, q.cwd);
  out.session_id = q.session_id;
  return !out.path.empty();
}

// --- omp ---------------------------------------------------------------------

// omp's rules, from its dirs.ts: the config root is ~/.omp ($PI_CONFIG_DIR
// names another folder in the home directory). A profile ($OMP_PROFILE, else
// $PI_PROFILE; "default" is none) keeps everything in
// <root>/profiles/<name>/agent. Without one, $PI_CODING_AGENT_DIR moves the
// agent directory wholesale, and otherwise it is <root>/agent.
namespace {

std::string omp_root(const std::string& home) {
  const char* d = env("PI_CONFIG_DIR");
  return home + "/" + (d ? d : ".omp");
}

std::string omp_profile(const std::vector<std::string>* argv = nullptr) {
  const char* p = getenv("OMP_PROFILE");  // set but empty still wins
  if (!p) p = getenv("PI_PROFILE");
  std::string_view s = p ? p : "";
  if (argv)
    for (size_t i = 1; i < argv->size(); i++) {
      if ((*argv)[i] == "--") break;
      if ((*argv)[i] == "--profile" && i + 1 < argv->size()) s = (*argv)[++i];
      else if ((*argv)[i].starts_with("--profile=")) s = std::string_view((*argv)[i]).substr(10);
    }
  while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
  while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
  if (s == "default" || s.find('/') != std::string_view::npos || s == "." || s == "..") return {};
  return std::string(s);
}

}  // namespace

std::string OmpAdapter::agent_dir(const std::string& home) const {
  const std::string profile = omp_profile();
  if (!profile.empty()) return omp_root(home) + "/profiles/" + profile + "/agent";
  if (const char* d = env("PI_CODING_AGENT_DIR")) return absolute(d, {});
  return omp_root(home) + "/agent";
}

// Sessions are data: unless the agent directory was moved, an existing
// $XDG_DATA_HOME/omp (…/omp/profiles/<name> under a profile) holds them.
std::string OmpAdapter::sessions_dir(const std::vector<std::string>* argv) const {
  const std::string profile = omp_profile(argv);
  if (profile.empty())
    if (const char* d = env("PI_CODING_AGENT_DIR")) return absolute(d, {}) + "/sessions";
  if (const char* x = env("XDG_DATA_HOME")) {
    std::string data = std::string(x) + "/omp";
    if (!profile.empty()) data += "/profiles/" + profile;
    if (fs::exists(data)) return data + "/sessions";
  }
  const std::string root = omp_root(fs::home());
  return (profile.empty() ? root + "/agent" : root + "/profiles/" + profile + "/agent") + "/sessions";
}

void OmpAdapter::prepare(Launch& l, const LaunchExtras& x) const {
  if (l.argv.empty()) l.argv = {"omp"};
  if (cmdline::program(l.argv) == "omp") cmdline::adopt_cwd(l.argv, l.cwd, {"--cwd"});
  add_extras(l, x);
}

bool OmpAdapter::continue_session(Launch& l, std::string_view id, bool fork, std::string* note) const {
  // omp has no fork flag of its own; resuming is all it offers.
  if (fork && note) *note = "omp has no fork — resuming instead";
  l.forked = false;
  l.session_id = id;
  // omp resolves an id among the top-level transcripts only; a subagent's run
  // is resumed by its path.
  std::string target(id);
  for_each_session([&](const std::string& path) {
    for_each_subagent(path, [&](const std::string& sub) {
      SessionHead h;
      if (target == id && read_head(sub, h) && h.id == id) target = sub;
    });
  });
  l.argv = {"omp", "--resume", target};
  return true;
}

void OmpAdapter::snapshot_transcripts(const std::vector<std::string>& argv, const std::string& cwd,
                                      std::vector<std::string>& out) const {
  for_each_session([&](const std::string& path) { out.push_back(path); }, &argv, cwd);
}

// No pre-assignable id: find the newest unclaimed session that appeared after
// we spawned and whose recorded cwd is ours — the same trick as codex. A new
// session's subagents write transcripts of their own, with the same cwd, but
// a level further down where only a resume looks.
bool OmpAdapter::find_transcript(const TranscriptQuery& q, FoundTranscript& out) const {
  std::string best, best_sid;
  int64_t best_mtime = 0;
  const bool resuming = q.resuming();
  auto consider = [&](const std::string& path) {
    if (!resuming && q.existed(path)) return;
    if (q.claimed(path)) return;

    struct stat st{};
    if (stat(path.c_str(), &st) != 0) return;
    if (!resuming && int64_t(st.st_mtime) + 5 < q.started_at) return;
    if (int64_t(st.st_mtime) < best_mtime) return;

    SessionHead h;
    if (!read_head(path, h)) return;
    if (resuming ? h.id != q.session_id : h.cwd != q.cwd) return;

    best = path;
    best_sid = std::move(h.id);
    best_mtime = int64_t(st.st_mtime);
  };
  for_each_session([&](const std::string& path) {
    consider(path);
    if (resuming) for_each_subagent(path, consider);
  }, q.argv, q.cwd);
  if (best.empty()) return false;
  out.path = std::move(best);
  out.session_id = std::move(best_sid);
  return true;
}

}  // namespace mico
