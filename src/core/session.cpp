#include "adapters/adapters.h"
#include "base/fs.h"
#include "base/log.h"
#include "core/scope.h"
#include "core/session.h"
#include "core/store.h"
#include "vt/keys.h"

#include <dirent.h>
#include <signal.h>
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

bool plan_limits_enabled() {
  std::string buf;
  return !fs::read_prefix(config_dir() + "/limits", 64, buf).starts_with("off");
}

void set_plan_limits(bool on) {
  mkdir(config_dir().c_str(), 0700);
  if (FILE* f = fopen((config_dir() + "/limits").c_str(), "w")) {
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

}  // namespace

LiveSession::~LiveSession() {
  auto it = claimed_transcripts().find(transcript_);
  if (it != claimed_transcripts().end() && it->second == this) claimed_transcripts().erase(it);
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

  // What mico can show, told to the agent so it can use it: charts, today.
  // And mico's own tools, as an MCP server, when the user turned them on with
  // `:mcp on`. The adapter decides how its agent takes them.
  Launch launch = l;
  LaunchExtras extras;
  const bool mcp = mcp_tools_enabled() && !self_exe().empty();
  if (mcp) extras.mcp_exe = self_exe();
  if (plan_limits_enabled() && !self_exe().empty()) extras.status_exe = self_exe();
  if (agent_hints_enabled()) extras.hints = std::string(kAgentHints) + (mcp ? kMcpHint : "");
  driver().prepare(launch, extras);
  session_id_ = launch.session_id;

  driver().snapshot_transcripts(preexisting_);
  std::sort(preexisting_.begin(), preexisting_.end());

  argv_ = std::move(launch.argv);
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
  ++generation_;
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
  settle_at_ms_ = 0;
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
  starting_until_ms_ = now_ms() + 60000;
  last_w_ = w;
  last_h_ = h;
  vt_.resize(w, h);
  MLOG("session spawn: %s @ %dx%d", agent_.c_str(), w, h);
  // Its own scope: what it runs is killed for memory without the daemon.
  // Named afresh each spawn: a restart must not meet the scope of the run
  // before, which a process it left behind can keep alive.
  static unsigned spawns = 0;
  pty_.spawn(argv_, cwd_, w, h,
             scope::unit_name(agent_.substr(agent_.rfind('/') + 1) + "-" + std::to_string(getpid()) + "-" +
                              std::to_string(++spawns)),
             "mico: " + agent_ + " in " + cwd_);
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

void LiveSession::discover_transcript() {
  if (!adapter_ || !transcript_.empty()) return;
  int64_t t = now_ms();
  if (t - last_probe_ < 500) return;
  last_probe_ = t;

  TranscriptQuery q;
  q.session_id = session_id_;
  q.cwd = cwd_;
  q.origin = origin_;
  q.forked = forked_;
  q.started_at = started_at_;
  q.pid = pty_.pid();
  q.preexisting = &preexisting_;
  q.claimed = [](const std::string& path) { return claimed_transcripts().count(path) > 0; };
  FoundTranscript found;
  if (!adapter_->find_transcript(q, found)) return;
  transcript_ = std::move(found.path);
  if (!found.session_id.empty()) session_id_ = std::move(found.session_id);
  claimed_transcripts()[transcript_] = this;
  MLOG("transcript linked: %s -> %s", agent_.c_str(), transcript_.c_str());
}

bool LiveSession::busy() const {
  if (!spawned_) return false;
  if (pty_.exited()) return false;
  // Time enters only through thresholds of a second and more: a 16 ms tick is
  // fresh enough.
  const int64_t now = now_ms();
  if (vt_.generation() == busy_gen_ && now / 16 == busy_tick_ && turn_open_ == busy_turn_ &&
      last_output_ms_ == busy_out_)
    return busy_;
  busy_gen_ = vt_.generation();
  busy_tick_ = now / 16;
  busy_turn_ = turn_open_;
  busy_out_ = last_output_ms_;
  busy_ = driver().busy(Liveness{vt_, now - last_output_ms_, turn_open_});
  return busy_;
}

bool LiveSession::needs_input() const {
  if (!spawned_ || pty_.exited()) return false;
  if (vt_.generation() != input_gen_) {
    input_gen_ = vt_.generation();
    input_ = driver().awaits_input(vt_);
  }
  return input_;
}

const Adapter& LiveSession::driver() const { return adapter_ ? *adapter_ : plain_adapter(); }

void LiveSession::follow_turns() {
  if (!driver().tracks_turns() || transcript_.empty()) return;
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
  // A record's type comes first; the rest of a line can be megabytes.
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
      if (const int m = driver().turn_marker(turn_head_); m != 0) turn_open_ = m > 0;
      turn_head_.clear();
    }
  }
  close(fd);
}

bool LiveSession::starting() const { return spawned_ && now_ms() < starting_until_ms_; }

bool LiveSession::send_parts(const std::vector<MessagePart>& parts, std::string_view first) {
  if (!spawned_ || pty_.exited() || message_sending()) return false;
  starting_until_ms_ = 0;
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
  ++generation_;
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
  starting_until_ms_ = 0;
  msg_steps_ = std::move(steps);
  msg_step_ = 0;
  msg_key_ms_ = 0;  // the first step goes at once
  msg_feedback_ = false;
  return true;
}

bool LiveSession::send_answer(std::vector<std::string> steps) {
  if (!spawned_ || pty_.exited() || answer_sending() || steps.empty()) return false;
  starting_until_ms_ = 0;
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

const char* LiveSession::crumb() {
  if (crumb_.empty() || crumb_id_ != session_id_) {
    crumb_id_ = session_id_;
    crumb_ = agent_ + " " + (session_id_.empty() ? std::string("(no id yet)") : session_id_) + " in " + cwd_;
  }
  return crumb_.c_str();
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

  // A dialog the agent opens at startup, answered for the user: the folder is
  // on their tracked list, which is trust enough. Only while it is starting:
  // a resumed chat is asked too, its transcript there all along. One key at a
  // time, each once the screen has been still for a moment.
  if (starting() && !answer_sending() && trust_tries_ < 20 &&
      now_ms() >= trust_next_ms_ && now_ms() - last_output_ms_ >= 250) {
    bool confirms = false;
    const std::string key = driver().startup_answer(vt_, &confirms);
    if (!key.empty()) {
      pty_.write(key);
      trust_tries_++;
      trust_next_ms_ = now_ms() + (confirms ? 3000 : 300);
      if (confirms) MLOG("accepted the startup prompt for %s", cwd_.c_str());
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
    // Killed outright: nearly always for memory. Its scope is in the journal.
    if (pty_.exit_signal() == SIGKILL)
      MLOG("session KILLED (SIGKILL): %s; for memory, if the journal says so: journalctl --user -u %s", crumb(),
           pty_.unit().empty() ? "(no scope of its own)" : pty_.unit().c_str());
  }
  bool finished = false;
  if (working || pty_.exited()) settle_at_ms_ = 0;
  else if (was_working_) settle_at_ms_ = now_ms() + kSettleMs;
  else if (settle_at_ms_ && now_ms() >= settle_at_ms_) {
    settle_at_ms_ = 0;
    finished = !unseen_;
    unseen_ = true;
  }
  const bool changed = answer_changed || !buf_.empty() || working != was_working_ ||
                       pty_.exited() != was_exited_ || finished;
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
  const bool moved = changed || queue_.size() != held || transcript_ != before;
  if (moved) ++generation_;
  return moved;
}

}  // namespace mico
