#include "adapters/pi/pi.h"

#include <sys/stat.h>

#include "adapters/cmdline.h"
#include "base/fs.h"
#include "base/json.h"
#include "base/uuid.h"

namespace mico {

void for_each_pi_family_session(const std::string& root,
                                const std::function<void(const std::string&)>& fn) {
  fs::list_dir(root, true, [&](const std::string& slug) {
    const std::string dir = root + "/" + slug;
    fs::list_dir(dir, false, [&](const std::string& fn2) {
      if (fs::has_suffix(fn2, ".jsonl")) fn(dir + "/" + fn2);
    });
  });
}

// --- pi ----------------------------------------------------------------------

std::string PiAdapter::sessions_dir() const { return fs::home() + "/.pi/agent/sessions"; }

void PiAdapter::prepare(Launch& l, const LaunchExtras&) const {
  if (l.argv.empty()) {
    // pi honours the same "use this exact id, creating it if missing" deal as
    // claude.
    if (l.session_id.empty()) l.session_id = make_uuid_v4();
    l.argv = {"pi", "--session-id", l.session_id};
  }
  if (l.session_id.empty()) cmdline::adopt_session_id(l.argv, l.session_id);
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
  const std::string root = sessions_dir();
  const std::string suffix = "_" + q.session_id + ".jsonl";
  fs::list_dir(root, true, [&](const std::string& slug) {
    if (!out.path.empty()) return;
    const std::string dir = root + "/" + slug;
    fs::list_dir(dir, false, [&](const std::string& fn2) {
      if (!out.path.empty() || !fs::has_suffix(fn2, suffix)) return;
      const std::string cand = dir + "/" + fn2;
      if (!q.claimed(cand)) out.path = cand;
    });
  });
  out.session_id = q.session_id;
  return !out.path.empty();
}

// --- omp ---------------------------------------------------------------------

std::string OmpAdapter::sessions_dir() const { return fs::home() + "/.omp/agent/sessions"; }

bool OmpAdapter::continue_session(Launch& l, std::string_view id, bool fork, std::string* note) const {
  // omp has no fork flag of its own; resuming is all it offers.
  if (fork && note) *note = "omp has no fork — resuming instead";
  l.forked = false;
  l.session_id = id;
  l.argv = {"omp", "--resume", std::string(id)};
  return true;
}

void OmpAdapter::snapshot_transcripts(std::vector<std::string>& out) const {
  for_each_pi_family_session(sessions_dir(), [&](const std::string& path) { out.push_back(path); });
}

// No pre-assignable id: find the newest unclaimed session that appeared after
// we spawned and whose recorded cwd is ours — the same trick as codex, but omp
// sometimes writes a "title" record before "session", so the match has to scan
// a few lines rather than just the first one.
bool OmpAdapter::find_transcript(const TranscriptQuery& q, FoundTranscript& out) const {
  std::string best, best_sid;
  int64_t best_mtime = 0;
  const std::string root = sessions_dir();
  for_each_pi_family_session(root, [&](const std::string& path) {
    const bool resuming = q.resuming();
    if (!resuming && q.existed(path)) return;
    if (q.claimed(path)) return;

    struct stat st{};
    if (stat(path.c_str(), &st) != 0) return;
    if (!resuming && int64_t(st.st_mtime) + 5 < q.started_at) return;
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
      match = resuming ? sid == q.session_id : cwd == q.cwd;
      return false;
    });
    if (!match) return;

    best = path;
    best_sid = sid;
    best_mtime = int64_t(st.st_mtime);
  });
  if (best.empty()) return false;
  out.path = std::move(best);
  out.session_id = std::move(best_sid);
  return true;
}

}  // namespace mico
