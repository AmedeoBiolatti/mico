#include "core/away.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "base/fs.h"
#include "core/store.h"

namespace mico {

NotifyMode notify_mode() {
  const std::string v = read_setting("notify");
  if (v.starts_with("off")) return NotifyMode::Off;
  if (v.starts_with("bell")) return NotifyMode::Bell;
  return NotifyMode::Desktop;
}

void set_notify_mode(NotifyMode m) {
  write_setting("notify", m == NotifyMode::Off ? "off" : m == NotifyMode::Bell ? "bell" : "desktop");
}

bool restore_agents_enabled() { return setting_on("restore", true); }

void set_restore_agents(bool on) { set_setting_on("restore", on); }

std::string running_path() { return state_dir() + "/running"; }

std::vector<RunningAgent> read_running() {
  std::vector<RunningAgent> out;
  std::string buf;
  const std::string_view blob = fs::read_prefix(running_path(), 256u << 10, buf);
  fs::for_each_line(blob, [&](std::string_view line) {
    const size_t a = line.find('\t');
    const size_t b = a == std::string_view::npos ? a : line.find('\t', a + 1);
    if (b == std::string_view::npos) return true;
    RunningAgent r{std::string(line.substr(0, a)), std::string(line.substr(a + 1, b - a - 1)),
                   std::string(line.substr(b + 1))};
    if (!r.agent.empty() && !r.session_id.empty()) out.push_back(std::move(r));
    return true;
  });
  return out;
}

void write_running(const std::vector<RunningAgent>& agents) {
  const std::string path = running_path();
  if (agents.empty()) {
    unlink(path.c_str());
    return;
  }
  std::string body;
  for (const auto& a : agents) {
    // A field that holds the separators could not be read back as written.
    const std::string line = a.agent + "\t" + a.session_id + "\t" + a.cwd;
    if (std::count(line.begin(), line.end(), '\t') == 2 && line.find('\n') == std::string::npos)
      body += line + "\n";
  }
  fs::make_dirs(state_dir());
  const std::string tmp = path + ".tmp";
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return;
  const bool ok = write(fd, body.data(), body.size()) == ssize_t(body.size());
  ::close(fd);
  if (ok) rename(tmp.c_str(), path.c_str());
  else unlink(tmp.c_str());
}

}  // namespace mico
