#include "adapters/pi/pi.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <map>

#include "adapters/listing.h"
#include "base/fs.h"
#include "base/json.h"

namespace mico {

void PiFamilyAdapter::list_sessions(const std::function<void(SessionRef&&)>& add) const {
  const std::string root = sessions_dir();
  std::string buf;
  fs::list_dir(root, true, [&](const std::string& slug) {
    const std::string dir = root + "/" + slug;
    fs::list_dir(dir, false, [&](const std::string& fname) {
      // omp keeps a same-named scratch directory (bash logs) beside each
      // transcript; it carries no extension, so the suffix check alone
      // already steps over it.
      if (!fs::has_suffix(fname, ".jsonl")) return;

      SessionRef s;
      s.agent = id();
      s.path = dir + "/" + fname;
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
      add(std::move(s));
    });
  });
}

}  // namespace mico
