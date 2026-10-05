#include "adapters/pi/pi.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <map>

#include "adapters/listing.h"
#include "base/fs.h"
#include "base/json.h"

namespace mico {
namespace {

// Reads the head of the transcript at `path` into `s`: its cwd, its id, and a
// title — the one omp chose, else the first real prompt.
void read_ref(SessionRef& s, std::string& buf) {
  const std::string fname = s.path.substr(s.path.rfind('/') + 1);
  s.mtime = fs::mtime(s.path, &s.bytes);

  std::string first_user;
  auto scan = [&](std::string_view blob) {
  fs::for_each_line(blob, [&](std::string_view line) {
    std::string_view type;
    js::Value message{};
    std::string_view title;
    js::scan_object(line, [&](std::string_view k, const js::Value& v) {
      if (k == "type") {
        type = v.body();
        // omp's "custom"/"custom_message" telemetry outnumbers everything
        // else in a session by an order of magnitude; reject it after one
        // member rather than walk its (often large) payload.
        if (type != "session" && type != "title" && type != "title_change" &&
            type != "message")
          return false;
        return true;
      }
      if (k == "cwd" && s.cwd.empty()) s.cwd = std::string(v.body());
      else if (k == "id" && s.id.empty() && type == "session") s.id = std::string(v.body());
      else if (k == "title") title = v.body();
      else if (k == "message") message = v;
      return true;
    });

    // omp names sessions itself, and a name it chose beats anything
    // mined from the transcript.
    if ((type == "title" || type == "title_change") && !title.empty()) {
      s.title.clear();
      js::unescape_append(title, s.title);
      return s.cwd.empty();  // keep reading only if cwd is still unknown
    }

    if (first_user.empty() && type == "message" && message.is_object()) {
      std::string_view role;
      js::Value content{};
      js::scan_object(message.raw, [&](std::string_view mk, const js::Value& mv) {
        if (mk == "role") { role = mv.body(); return true; }
        if (mk == "content") { content = mv; return false; }
        return true;
      });
      if (role == "user" && content.is_array()) {
        js::scan_array(content.raw, [&](const js::Value& b) {
          if (!b.is_object()) return true;
          bool done = false;
          js::scan_object(b.raw, [&](std::string_view bk, const js::Value& bv) {
            if (bk != "text" || !bv.is_string()) return true;
            std::string text;
            js::unescape_append(bv.body(), text);
            if (!text.empty() && !is_noise_prompt(text)) { first_user = std::move(text); done = true; }
            return false;
          });
          return !done;
        });
      }
    }
    return s.cwd.empty() || (s.title.empty() && first_user.empty());
  });
  };

  constexpr size_t kPrefix = 128u << 10;
  scan(fs::read_prefix(s.path, kPrefix, buf));
  if (s.title.empty() && first_user.empty() && s.bytes > kPrefix)
    scan(fs::read_prefix(s.path, 4u << 20, buf));

  if (s.title.empty()) s.title = first_user;
  if (s.id.empty()) {
    // Fall back to the uuid half of "<timestamp>_<uuid>.jsonl".
    const std::string stem = fname.substr(0, fname.size() - 6);
    const size_t us = stem.find('_');
    s.id = us == std::string::npos ? stem : stem.substr(us + 1);
  }
  if (s.cwd.empty()) s.cwd = "(unknown)";
}

}  // namespace

void PiFamilyAdapter::list_sessions(const std::function<void(SessionRef&&)>& add) const {
  std::string buf;
  for_each_session([&](const std::string& path) {
    SessionRef s;
    s.agent = id();
    s.path = path;
    read_ref(s, buf);
    // omp's subagent runs, each a chat of its own: named for the agent and
    // the chat that spawned it, since their first prompt is the assignment.
    const std::string parent = s.title;
    for_each_subagent(path, [&](const std::string& sub) {
      SessionRef r;
      r.agent = s.agent;
      r.path = sub;
      read_ref(r, buf);
      const size_t slash = sub.rfind('/');
      r.title = "↳ " + sub.substr(slash + 1, sub.size() - slash - 7);
      if (!parent.empty()) r.title += " · " + parent;
      add(std::move(r));
    });
    add(std::move(s));
  });
}

}  // namespace mico
