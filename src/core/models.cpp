#include "core/models.h"

#include <time.h>

#include <algorithm>
#include <map>

#include "core/log.h"
#include "term/text.h"

namespace mico {
namespace {

int64_t now_ms() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// The picker is drawn with cursor positioning rather than spaces, so it only
// reads as text once the emulator has placed it. That is why this parses the
// grid and not the byte stream.
std::string row_text(const VtRow& r) {
  std::string out;
  for (const auto& c : r)
    if (c.width) text::encode(c.cp, out);
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

std::string trim(std::string_view s) {
  size_t a = s.find_first_not_of(' ');
  if (a == std::string_view::npos) return {};
  size_t b = s.find_last_not_of(' ');
  return std::string(s.substr(a, b - a + 1));
}

// "Opus" -> "opus"; "Opus 5" -> "claude-opus-5". A bare alias always names the
// latest of its family, so a name carrying a version is a different model and
// has to go over as the full name claude takes.
std::string value_for(const std::string& name) {
  std::string v;
  bool versioned = false;
  for (char c : name) {
    if (c == ' ') { v += '-'; continue; }
    if (c >= '0' && c <= '9') versioned = true;
    v += char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  }
  if (v == "default") return v;
  return versioned ? "claude-" + v : v;
}

// A model reads as a family and a version: "Opus 5.5", "Haiku 4.5". Prose does
// not, and that is the whole distinction being drawn.
bool is_model_name(std::string_view s) {
  if (s.empty()) return false;
  const size_t sp = s.rfind(' ');
  if (sp == std::string_view::npos || sp + 1 >= s.size()) return false;
  if (s.find(' ') != sp) return false;  // one space: family, then version
  for (char c : s.substr(sp + 1))
    if ((c < '0' || c > '9') && c != '.') return false;
  return true;
}

}  // namespace

std::vector<ModelOption> parse_model_picker(const Vt& vt) {
  std::vector<ModelOption> out;
  bool in_picker = false;
  for (int i = 0; i < vt.total_rows(); i++) {
    const std::string line = row_text(vt.row(i));
    if (line.find("Select model") != std::string::npos) {
      // A picker reopened over an earlier one leaves both on screen; the last
      // header wins.
      in_picker = true;
      out.clear();
      continue;
    }
    if (!in_picker) continue;

    // "❯ 2. Opus   Opus 5.5 · Best for everyday…" — the marker and the number
    // are chrome, the name runs to the gap before the description.
    const size_t dot = line.find(". ");
    if (dot == std::string::npos) continue;
    const std::string before = trim(line.substr(0, dot));
    if (before.empty()) continue;
    const size_t digits = before.find_first_of("0123456789");
    if (digits == std::string::npos) continue;
    if (before.find_first_not_of("0123456789", digits) != std::string::npos) continue;

    std::string rest = trim(line.substr(dot + 2));
    // The tick marking the model in use sits inside the name column, so it has
    // to come out before the name is read rather than after.
    const bool current = rest.find("\xE2\x9C\x94") != std::string::npos;  // ✔
    for (std::string_view mark : {"\xE2\x9C\x94", "\xE2\x9D\xAF"})        // ✔ ❯
      for (size_t p; (p = rest.find(mark)) != std::string::npos;)
        rest.erase(p, mark.size());
    const size_t gap = rest.find("  ");
    std::string desc = gap == std::string::npos ? std::string() : trim(rest.substr(gap));
    std::string name = trim(gap == std::string::npos ? rest : rest.substr(0, gap));
    // "Default (recommended)" is one entry whose qualifier is not part of it.
    if (const size_t p = name.find(" ("); p != std::string::npos) name = name.substr(0, p);
    if (name.empty()) continue;

    // The description usually opens with the concrete model ("Opus 5.5 · Best
    // for…"), which is the only thing telling two entries of one family apart.
    // Usually — an entry can lead with prose instead ("Newer version
    // available"), and naming a model after that would be worse than not
    // naming it at all.
    if (const size_t mid = desc.find(" \xC2\xB7 "); mid != std::string::npos)
      desc = desc.substr(0, mid);
    desc = trim(desc);
    if (!is_model_name(desc)) desc.clear();

    ModelOption opt;
    opt.value = value_for(name);
    opt.label = desc.empty() || desc == name ? name : name + " (" + desc + ")";
    opt.current = current;
    if (std::none_of(out.begin(), out.end(),
                     [&](const ModelOption& o) { return o.value == opt.value; }))
      out.push_back(std::move(opt));
  }
  return out;
}

void ModelProbe::start(const std::string& agent, const std::string& cwd) {
  if (started_) return;
  started_ = true;
  agent_ = agent;
  began_ms_ = now_ms();
  quiet_since_ms_ = began_ms_;
  // Big enough that the picker is never truncated or scrolled into a shape
  // the parser cannot see.
  vt_.resize(120, 50);
  if (!pty_.spawn({agent}, cwd, 120, 50)) done_ = true;
}

void ModelProbe::finish(std::vector<ModelOption> found, const char* why) {
  // The command catalog's probe reads the same list out of claude's
  // initialize answer, with descriptions this scrape cannot see; when that
  // landed first, its list stands.
  if (!found.empty() && !known_models(agent_).empty()) return;
  if (!found.empty()) {
    std::string list;
    for (const auto& m : found) list += " " + m.value;
    MLOG("model probe: %s found %zu:%s", agent_.c_str(), found.size(), list.c_str());
    set_known_models(agent_, std::move(found));
  } else {
    // Without the screen there is no telling a restyled picker from one that
    // never opened, so the log gets what the probe saw.
    MLOG("model probe: %s found nothing (%s); screen was:", agent_.c_str(), why);
    for (int i = 0; i < vt_.total_rows(); i++)
      if (std::string r = row_text(vt_.row(i)); !r.empty()) MLOG("  | %s", r.c_str());
  }
  pty_.terminate();
  done_ = true;
}

bool ModelProbe::pump() {
  if (!started_) return false;
  if (done_) {
    pty_.poll_exit();  // reap the killed probe rather than leave a zombie
    return false;
  }
  pty_.poll_exit();
  const int64_t now = now_ms();

  buf_.clear();
  const bool alive = pty_.read_available(buf_);
  if (!buf_.empty()) {
    vt_.write(buf_);
    quiet_since_ms_ = now;
  }

  // A picker that never arrives must not hold a process open for the life of
  // the app.
  if (!alive || pty_.exited() || now - began_ms_ > 25000) {
    finish(parse_model_picker(vt_), pty_.exited() || !alive ? "claude exited" : "timed out");
    return true;
  }

  // The trust dialog wants a decision about the user's files. Answering it on
  // their behalf is not mico's call, so the probe simply gives up.
  for (int i = 0; i < vt_.total_rows(); i++)
    if (row_text(vt_.row(i)).find("trust this folder") != std::string::npos) {
      finish({}, "trust dialog");
      return true;
    }

  switch (step_) {
    case Step::Starting:
      // Claude paints its banner, its tips and its input box in several
      // bursts; a settled screen is the only honest "ready".
      if (now - quiet_since_ms_ > 700 && now - began_ms_ > 1200) {
        pty_.write("/model\r");
        step_ = Step::Asking;
        quiet_since_ms_ = now;
      }
      return false;
    case Step::Asking:
      if (now - quiet_since_ms_ > 500) step_ = Step::Reading;
      return false;
    case Step::Reading: {
      std::vector<ModelOption> found = parse_model_picker(vt_);
      if (found.size() >= 2) {
        finish(std::move(found), "");
        return true;
      }
      // Still drawing, or the picker never opened. The overall timeout above
      // ends it either way.
      return false;
    }
  }
  return false;
}

namespace {
std::map<std::string, std::vector<ModelOption>>& cache() {
  static std::map<std::string, std::vector<ModelOption>> c;
  return c;
}
}  // namespace

const std::vector<ModelOption>& known_models(const std::string& agent) {
  static const std::vector<ModelOption> none;
  auto it = cache().find(agent);
  return it == cache().end() ? none : it->second;
}

const std::vector<std::string>& known_efforts(const std::string& agent) {
  static std::map<std::string, std::vector<std::string>> lists;
  return lists[agent];
}

void set_known_efforts(const std::string& agent, std::vector<std::string> v) {
  const_cast<std::vector<std::string>&>(known_efforts(agent)) = std::move(v);
}

void set_known_models(const std::string& agent, std::vector<ModelOption> v) {
  cache()[agent] = std::move(v);
}

}  // namespace mico
