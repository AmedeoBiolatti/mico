#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "adapters/adapters.h"
#include "adapters/claude/claude.h"
#include "adapters/usage_scan.h"
#include "base/fs.h"
#include "base/json.h"
#include "base/path.h"

// Claude's subscription limits reach no transcript: claude reads them off the
// API's replies and hands them to its status line command, as rate_limits in
// the JSON on its stdin. mico gives claude itself as that command
// (`mico --claude-status`), which keeps the latest in a file the Usage tab
// reads. Nothing is asked of the API, and no credential is touched.
namespace mico {
namespace {


// One line per window: "5h <percent used> <resets at, unix seconds>", and
// "7d …". The file's mtime is when claude last reported them.
std::string limits_path() { return state_dir() + "/claude-limits"; }

bool read_window(std::string_view raw, double* pct, int64_t* resets) {
  bool any = false;
  js::scan_object(raw, [&](std::string_view k, const js::Value& v) {
    if (k == "used_percentage") *pct = usage::f64(v), any = true;
    else if (k == "resets_at") *resets = int64_t(usage::u64(v));
    return true;
  });
  return any;
}

// The status line the user set for claude, if any, where claude looks for
// it: the project's local settings, its shared ones, then their own.
std::string user_status_command(const std::string& project) {
  std::vector<std::string> files;
  if (!project.empty()) {
    files.push_back(project + "/.claude/settings.local.json");
    files.push_back(project + "/.claude/settings.json");
  }
  files.push_back(fs::home() + "/.claude/settings.json");
  for (const auto& f : files) {
    std::string buf, cmd;
    js::scan_object(fs::read_prefix(f, 1u << 20, buf), [&](std::string_view k, const js::Value& v) {
      if (k != "statusLine" || !v.is_object()) return true;
      js::scan_object(v.raw, [&](std::string_view k2, const js::Value& c) {
        if (k2 == "command" && c.is_string()) js::unescape_append(c.body(), cmd);
        return true;
      });
      return false;
    });
    if (!cmd.empty()) return cmd;
  }
  return {};
}

}  // namespace

int claude_status_line() {
  std::string in;
  char buf[1 << 14];
  for (size_t n; (n = fread(buf, 1, sizeof buf, stdin)) > 0;) in.append(buf, n);

  double pct5 = 0, pct7 = 0;
  int64_t reset5 = 0, reset7 = 0;
  bool has5 = false, has7 = false;
  std::string project;
  js::scan_object(in, [&](std::string_view k, const js::Value& v) {
    if (k == "rate_limits" && v.is_object()) {
      js::scan_object(v.raw, [&](std::string_view w, const js::Value& wv) {
        if (w == "five_hour") has5 = read_window(wv.raw, &pct5, &reset5);
        else if (w == "seven_day") has7 = read_window(wv.raw, &pct7, &reset7);
        return true;
      });
    } else if (k == "workspace" && v.is_object()) {
      js::scan_object(v.raw, [&](std::string_view w, const js::Value& wv) {
        if (w == "project_dir" && wv.is_string()) js::unescape_append(wv.body(), project);
        return true;
      });
    }
    return true;
  });

  if (has5 || has7) {
    char line[2][80] = {};
    if (has5) snprintf(line[0], sizeof line[0], "5h %.1f %lld\n", pct5, (long long)reset5);
    if (has7) snprintf(line[1], sizeof line[1], "7d %.1f %lld\n", pct7, (long long)reset7);
    fs::make_dirs(state_dir());
    write_file_atomic(limits_path(), std::string(line[0]) + line[1]);
  }

  // The user's own status line still shows: it is run on the same input, and
  // what it prints is what claude shows. Without one, nothing is shown.
  const std::string mine = user_status_command(project);
  if (!mine.empty() && mine.find("--claude-status") == std::string::npos) {
    if (FILE* p = popen(mine.c_str(), "w")) {
      fwrite(in.data(), 1, in.size(), p);
      return pclose(p) == 0 ? 0 : 1;
    }
  }
  return 0;
}

void ClaudeAdapter::plan_limits(std::vector<PlanLimit>& out) const {
  const std::string path = limits_path();
  const int64_t as_of = fs::mtime(path);
  if (!as_of) return;
  std::string buf;
  std::string_view s = fs::read_prefix(path, 4096, buf);
  while (!s.empty()) {
    const size_t nl = s.find('\n');
    const std::string line(s.substr(0, nl));
    s = nl == std::string_view::npos ? std::string_view{} : s.substr(nl + 1);
    char key[8] = {};
    double pct = 0;
    long long resets = 0;
    if (sscanf(line.c_str(), "%7s %lf %lld", key, &pct, &resets) != 3) continue;
    const std::string_view k = key;
    if (k != "5h" && k != "7d") continue;
    out.push_back(PlanLimit{"claude", k == "5h" ? "5 hours" : "week", pct, int64_t(resets), as_of});
  }
}

}  // namespace mico
