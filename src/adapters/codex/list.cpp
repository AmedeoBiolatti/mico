#include "adapters/codex/codex.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <map>

#include "adapters/listing.h"
#include "base/fs.h"
#include "base/json.h"

namespace mico {
namespace {

// Codex keeps its own names for sessions in one small index, appended to as a
// name is refined. Later entries win. Reading this once beats scraping 200
// rollouts, and it is the name codex itself shows.
std::map<std::string, std::string> load_codex_names() {
  std::map<std::string, std::string> names;
  FILE* index = fopen((codex_home() + "/session_index.jsonl").c_str(), "rb");
  if (!index) return names;
  char* buf = nullptr;
  size_t capacity = 0;
  ssize_t n;
  // This index is append-only. Stream all complete records so the newest
  // names remain available even after years of sessions and renames.
  while ((n = getline(&buf, &capacity, index)) > 0) {
    if (buf[n - 1] != '\n') break;
    const std::string_view line(buf, size_t(n));
    std::string_view id, name;
    js::scan_object(line, [&](std::string_view k, const js::Value& v) {
      if (k == "id") id = v.body();
      else if (k == "thread_name") name = v.body();
      return true;
    });
    if (id.empty() || name.empty()) continue;
    std::string decoded;
    js::unescape_append(name, decoded);
    if (!decoded.empty()) names[std::string(id)] = std::move(decoded);
  }
  free(buf);
  fclose(index);
  return names;
}

}  // namespace

void CodexAdapter::list_sessions(const std::function<void(SessionRef&&)>& add) const {
  std::string buf;
  const std::map<std::string, std::string> names = load_codex_names();
  const std::string root = codex_home() + "/sessions";
  // Layout is sessions/YYYY/MM/DD/rollout-*.jsonl.
  fs::list_dir(root, true, [&](const std::string& y) {
    fs::list_dir(root + "/" + y, true, [&](const std::string& m) {
      const std::string md = root + "/" + y + "/" + m;
      fs::list_dir(md, true, [&](const std::string& dd) {
        const std::string dir = md + "/" + dd;
        fs::list_dir(dir, false, [&](const std::string& fname) {
          if (!fs::has_suffix(fname, ".jsonl")) return;

          SessionRef s;
          s.agent = id();
          s.path = dir + "/" + fname;
          s.mtime = fs::mtime(s.path, &s.bytes);

          // Only the head: session_meta is line 0, and measured across this
          // machine's rollouts the first real user turn lands by 14 KB
          // (median 8 KB). 64 KB is a wide margin and avoids reading ~100 MB
          // across a few hundred sessions just to build a list.
          // session_meta is line 0, so a small read identifies the rollout.
          // Codex's own name usually covers the title, and only a session the
          // index has never heard of needs the deeper scan for a first turn.
          auto scan = [&](std::string_view blob) {
          fs::for_each_line(blob, [&](std::string_view line) {
            const bool maybe_meta = line.find("session_meta") != std::string_view::npos;
            const bool maybe_user = line.find("\"role\":\"user\"") != std::string_view::npos;
            if (!maybe_meta && !maybe_user) return true;

            std::string_view type;
            js::Value payload{};
            js::scan_object(line, [&](std::string_view k, const js::Value& v) {
              if (k == "type") type = v.body();
              else if (k == "payload") { payload = v; return false; }
              return true;
            });
            if (!payload.is_object()) return true;

            if (type == "session_meta") {
              js::scan_object(payload.raw, [&](std::string_view k, const js::Value& v) {
                if (k == "id" || k == "session_id") s.id = std::string(v.body());
                else if (k == "cwd") {
                  s.cwd.clear();
                  js::unescape_append(v.body(), s.cwd);
                }
                return true;
              });
              return true;
            }
            if (type != "response_item") return true;

            std::string_view ptype, role;
            js::Value content{};
            js::scan_object(payload.raw, [&](std::string_view k, const js::Value& v) {
              if (k == "type") ptype = v.body();
              else if (k == "role") role = v.body();
              else if (k == "content") content = v;
              return true;
            });
            if (ptype != "message" || role != "user" || !content.is_array()) return true;

            bool done = false;
            js::scan_array(content.raw, [&](const js::Value& b) {
              if (!b.is_object()) return true;
              js::scan_object(b.raw, [&](std::string_view k, const js::Value& v) {
                if (k != "text" || !v.is_string()) return true;
                std::string text;
                js::unescape_append(v.body(), text);
                if (!text.empty() && !is_noise_prompt(text)) { s.title = std::move(text); done = true; }
                return false;
              });
              return !done;
            });
            return !done;
          });
          };

          // session_meta carries the whole system preamble: measured across
          // this machine's rollouts it reaches 21 KB, and two thirds of them
          // exceed 8 KB. Read past it, or the very line that identifies the
          // session is the one that gets cut off.
          constexpr size_t kMetaPrefix = 32u << 10;
          // Parse the complete metadata even when its preamble exceeds the
          // listing prefix. scan() expects newline-terminated records.
          const std::string meta(fs::read_first_line(s.path, buf));
          scan(meta + "\n");
          scan(fs::read_prefix(s.path, kMetaPrefix, buf));

          // Prefer codex's own name for the thread; the first user turn is
          // only a fallback, and a bare uuid is the last resort. The lookup has
          // to come after the id is known, so a deeper read cannot skip it.
          if (auto it = names.find(s.id); it != names.end() && !it->second.empty())
            s.title = it->second;
          else if (s.title.empty() && s.bytes > kMetaPrefix)
            scan(fs::read_prefix(s.path, 96u << 10, buf));
          if (s.id.empty()) return;  // a filename is not a resumable session id
          if (s.cwd.empty()) s.cwd = "(unknown)";
          add(std::move(s));
        });
      });
    });
  });
}

}  // namespace mico
