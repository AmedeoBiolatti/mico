#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "adapters/adapter.h"
#include "base/uuid.h"
#include "model/message.h"
#include "core/pty.h"
#include "vt/vt.h"

namespace mico {

// What mico tells agents at launch about what it can draw (charts). On unless
// turned off with `:charts off`; applies to agents started afterwards.
extern const char* const kAgentHints;
bool agent_hints_enabled();
void set_agent_hints(bool on);
// mico's MCP tools (`mico --mcp`: plot) for the agents it starts. Off unless
// turned on with `:mcp on`; applies to agents started afterwards.
bool mcp_tools_enabled();
void set_mcp_tools(bool on);

// mico's tools as an extension pi and omp load (-e), running `exe --mcp` for
// each call; see LaunchExtras::tool_extension.
std::string tool_extension_source(const std::string& exe);

// An answer step that names a menu row rather than a key: "\x01pick:<label>",
// or "\x01pick:<label>|<submenu>" for a row that may sit behind another one
// ("Max" behind "More reasoning…"). See ChipControl::steps.
inline constexpr std::string_view kPickStep = "\x01pick:";
// The key that picks that row off `vt`: its number. When only the submenu's
// row is there, its number, with `again` set: the step is to be taken again
// once the submenu shows. Empty when neither is on the screen.
std::string pick_key(const Vt& vt, std::string_view step, bool* again);

// Whether agents report their usage limits to mico (claude, through its
// status line). On unless turned off.
bool plan_limits_enabled();
void set_plan_limits(bool on);


// A running agent: its pty, its emulated screen, and the transcript it is
// writing. The two planes are always both live; the view toggle only chooses
// which one is painted.
class LiveSession {
 public:
  // What can be known without the hooks plane. "Blocked" is deliberately not
  // here: telling a permission prompt apart from an idle prompt means reading
  // the agent's screen, and that guess would be wrong often enough to be worse
  // than no answer.
  enum class Status { Working, Idle, Waiting, Exited };
  ~LiveSession();
  // Unique among every session this process has run, and never reused: how a
  // client names a session across messages.
  uint64_t serial() const { return serial_; }
  using Launch = mico::Launch;


  // Prepares the launch but does not fork: the pty is spawned by the first
  // set_geometry() call, so the agent starts at the size it will actually be
  // drawn at and never has to repaint over its own startup banner.
  bool start(const Launch& l);
  // Reuse the session identity so its pane and unsent draft survive a restart.
  bool restart(const Launch& l);
  // Spawns on the first call, resizes on later ones. Returns true if this call
  // spawned the process.
  bool set_geometry(int w, int h);
  bool spawned() const { return spawned_; }
  // From the spawn until the user first sends it something (a minute at
  // most): when an agent asks its startup questions, a folder's trust. A
  // resumed chat has its transcript from the start, so that cannot say.
  bool starting() const;
  // The command start() settled on, including anything mico added to it.
  const std::vector<std::string>& argv() const { return argv_; }

  // Reads whatever the pty has produced and feeds the emulator. Also retries
  // transcript discovery until it succeeds.
  // Returns true if anything changed: new output, or a status transition.
  bool pump();

  // Deliver form navigation one key at a time, after the preceding terminal
  // update settles. Owned by the session so changing chats cannot reroute it.
  bool send_answer(std::vector<std::string> steps);
  bool answer_sending() const { return !answer_steps_.empty(); }
  // How soon pump() has work that only the passage of time advances, or -1.
  // Output from the agent wakes the loop by itself; these do not, so the loop
  // has to come back for them. Keys are paced out tens of milliseconds apart;
  // a transcript is looked for at most every 500 ms, and may never appear (a
  // fresh claude writes none before its first message), so looking for it
  // must not hold the loop at the fast rate.
  int timer_ms() const {
    if (answer_sending() || message_sending() || pty_.has_pending_input() ||
        !after_answer_.empty())
      return 30;
    if (!queue_.empty()) return 250;  // to notice the turn end that releases it
    if (turn_open_) return 250;         // to read the turn's end in the transcript
    if (settle_at_ms_) return 100;      // to call the turn finished on time
    if (spawned_ && !pty_.exited() && transcript_.empty() && adapter_) return 500;
    return -1;
  }
  bool answer_failed() const { return answer_failed_; }
  void cancel_answer();

  // A message to send once the answer being delivered has landed: notes on
  // it, which the agent then reads right after the answer. Without an answer
  // in flight it goes after a short pause, for agents whose keys were written
  // at once. An answer that fails hands it back through take_after_answer().
  void send_after_answer(std::string text);
  std::string take_after_answer() { return std::exchange(after_answer_, {}); }

  // Writes a message in steps, each after the agent has redrawn from the one
  // before: an image path pasted on its own, then the text, then Enter.
  // claude attaches a pasted image asynchronously, and text or an Enter
  // arriving in the same burst overtook it: the text landed before the image,
  // and a quick Enter could send the message without it. Unlike an answer, a
  // message is never abandoned: a step with no visible effect is followed by
  // the next after a while, not dropped.
  struct Step {
    std::string bytes;
    int settle_ms = 120;  // least time before the next step
  };
  bool send_message(std::vector<Step> steps);
  bool message_sending() const { return !msg_steps_.empty(); }

  // Sends a composed message now. Text alone goes as one paste and an Enter;
  // with an image it goes through send_message(). While the agent is working
  // this is steering: claude takes it at its next tool call, codex at once.
  // `first`, when given, goes before the message and is given time to land:
  // an Esc that closes a panel which would otherwise take the typing.
  bool send_parts(const std::vector<MessagePart>& parts, std::string_view first = {});
  // When you last used it, unix seconds: started or resumed it, or sent it a
  // message. What the chat list orders by; what the agent does meanwhile
  // does not move it.
  int64_t used_at() const { return used_at_; }

  // Messages held until the agent has finished its turn (Alt+Enter), oldest
  // first. Kept here rather than in the pane so they go out while another
  // chat is on screen. One is sent per turn: the next waits until the agent
  // has worked on the last one and come back to its prompt.
  void enqueue(std::vector<MessagePart> parts) {
    queue_.push_back(std::move(parts));
    ++generation_;
  }
  const std::vector<std::vector<MessagePart>>& queued() const { return queue_; }
  // Takes the newest queued message back, for editing. Empty when none.
  std::vector<MessagePart> take_last_queued();

  Pty& pty() { return pty_; }
  Vt& vt() { return vt_; }
  const std::string& agent() const { return agent_; }
  const std::string& cwd() const { return cwd_; }
  const std::string& session_id() const { return session_id_; }
  const std::string& origin() const { return origin_; }
  bool forked() const { return forked_; }
  const std::string& transcript() const { return transcript_; }
  const Adapter* adapter() const { return adapter_; }
  // The adapter, or the defaults for a command mico has none for: what to ask
  // about the agent's screen and how it takes keys, whichever agent it is.
  const Adapter& driver() const;
  bool exited() const { return pty_.exited(); }
  // True while the agent appears active. Claude uses its visible working
  // footer, Codex its status line and its transcript's turns; other agents
  // use recent terminal output as a fallback.
  bool busy() const;
  // Moves whenever anything about the session a front end shows may have
  // changed: its screen, its status, its transcript, its queue.
  uint64_t generation() const { return generation_; }
  // Changes when work starts, even while this chat is not being drawn.
  uint64_t work_generation() const { return work_generation_; }
  // Live, idle, and showing a prompt that wants a keypress (trust dialog, y/n).
  bool needs_input() const;
  Status status() const;
  // True when this agent stopped working while you were looking at something
  // else. This is the signal that matters when several are running.
  bool unseen() const { return unseen_; }
  void mark_seen() { unseen_ = false; }
  std::string label() const;
  // What the agent is running in the background now — monitors, commands it
  // moved off its turn — from the lines it writes while this process runs:
  // what an earlier process left running went with it.
  const std::vector<BackgroundTask>& background() const { return bg_.running; }
  // Who this is, for a crash report: "claude <id> in <folder>". Kept, so the
  // pointer outlives the call (see logs::doing()).
  const char* crumb();
  const std::string& start_error() const { return pty_.spawn_error(); }
  int exit_status() const { return pty_.exit_status(); }

 private:
  void discover_transcript();
  void follow_turns();
  bool follow_background();
  void go_on_in(const std::string& id);
  bool read_background_progress();
  std::string find_task_output(const std::string& id);

  Pty pty_;
  Vt vt_;
  std::string agent_;
  std::string cwd_;
  std::string session_id_;
  std::string origin_;
  bool forked_ = false;
  std::string transcript_;
  const Adapter* adapter_ = nullptr;
  int64_t started_at_ = 0;
  int64_t used_at_ = 0;
  int64_t last_probe_ = 0;
  int64_t last_output_ms_ = 0;
  bool was_working_ = false;
  uint64_t work_generation_ = 0;
  uint64_t generation_ = 0;
  // busy() and needs_input() read the screen, and a frame asks each a dozen
  // times: the answers are kept until the screen, the turn or the clock moves.
  mutable uint64_t input_gen_ = UINT64_MAX;
  mutable bool input_ = false;
  mutable uint64_t busy_gen_ = UINT64_MAX;
  mutable int64_t busy_tick_ = -1, busy_out_ = -1;
  mutable bool busy_turn_ = false, busy_ = false;
  const uint64_t serial_ = next_serial();
  static uint64_t next_serial() {
    static uint64_t n = 0;
    return ++n;
  }
  bool was_exited_ = false;
  int trust_tries_ = 0;
  int64_t starting_until_ms_ = 0;
  int64_t trust_next_ms_ = 0;
  bool unseen_ = false;
  // When work that stopped counts as a finished turn, if it stays stopped:
  // Claude's spinner blinks out between steps, and each blink is no answer.
  int64_t settle_at_ms_ = 0;
  static constexpr int64_t kSettleMs = 1200;
  // Codex's turns as its transcript records them: task_started, then
  // task_complete or turn_aborted. Read from what it appends, a line's head at
  // a time; a resumed chat's history counts for nothing, since it can end
  // mid-turn if codex was killed.
  bool turn_open_ = false;
  bool turns_from_end_ = false;
  std::string turn_file_;
  uint64_t turn_read_ = 0;
  std::string turn_head_;
  int64_t turn_poll_ms_ = 0;
  // Rollout files that already existed when this session started. Codex offers
  // no id flag, and an *already running* session in the same directory has a
  // fresh mtime and a matching cwd — so "newest matching file" would happily
  // attach to someone else's conversation.
  std::vector<std::string> preexisting_;
  std::string buf_;
  std::vector<std::string> argv_;
  bool spawned_ = false;
  std::vector<std::string> answer_steps_;
  size_t answer_step_ = 0;
  int64_t answer_key_ms_ = 0;
  bool answer_feedback_ = false;
  bool answer_failed_ = false;
  std::string after_answer_;
  int64_t after_answer_ms_ = 0;  // not before this
  void flush_queue();
  std::vector<std::vector<MessagePart>> queue_;
  bool queue_waiting_ = false;  // the last one sent has not visibly started a turn yet
  uint64_t queue_gen_ = 0;
  int64_t queue_sent_ms_ = 0;
  std::vector<Step> msg_steps_;
  size_t msg_step_ = 0;
  int64_t msg_key_ms_ = 0;
  bool msg_feedback_ = false;
  int last_w_ = 0, last_h_ = 0;
  std::string crumb_, crumb_id_;
  BackgroundTasks bg_;
  std::string bg_file_;   // the transcript being followed
  std::string continued_to_;  // the chat its last record hands on to
  bool continued_ = false;    // the file followed now is where the chat went on
  uint64_t bg_read_ = 0;  // how far
  std::string bg_line_;   // a line not yet ended
  bool bg_skip_ = false;  // the rest of a line too long to hold
  int64_t bg_poll_ms_ = 0;
  int64_t bg_progress_ms_ = 0;
  std::string bg_tasks_dir_;  // where Claude writes this session's task output
};


}  // namespace mico
