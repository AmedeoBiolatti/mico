#pragma once
#include <memory>
#include <optional>
#include <utility>
#include <string>
#include <vector>

#include "core/workspace.h"
#include "term/term.h"
#include "ui/layout.h"
#include "ui/picker.h"
#include "ui/theme.h"

namespace mico {

// What a keystroke asks the surrounding process to do. The App never decides
// this itself: detaching means something different to a client attached over a
// socket than it does to a single local process.
enum class AppAction { None, Detach, Shutdown };

// The Diff tab's choices, kept while its pane comes and goes.
struct DiffSettings {
  int span = 1;          // last 7 days
  bool by_chat = false;  // rows are chats rather than files
  std::string sel_key;   // the selected file, or chat's transcript
};

class App {
 public:
  static constexpr int kIndexSliceMs = Workspace::kIndexSliceMs;
  // A front end over `ws`, which outlives it. The default owns one.
  App();
  explicit App(Workspace& ws);
  ~App();
  Workspace& workspace() { return ws_; }

  int run();                      // interactive, single process; requires a tty
  // How long the event loop may sleep before service() has timed work to do.
  // Input and agent output wake the loop on their own; this only covers what
  // the clock moves. An idle mico wakes once a second, for relative times.
  int idle_timeout_ms() const;
  // Writes the view down (see load_view()) when it has changed since it was
  // last written. The interactive loops call it; a --dump or a test leaves
  // the file alone.
  void save_view_if_changed();
  int dump(int w, int h);         // render one frame as plain text to stdout

  // Headless driving, used by the daemon. The daemon owns the event loop and
  // the sockets; App only knows how to advance and how to draw.
  // Returns what the caller should do about this event, if anything.
  AppAction feed(const InputEvent& e) {
    // Several input events may arrive before the next frame. Route each one
    // against the selection established by the preceding event.
    if ((layout_dirty_ || ws_.closing()) && viewport_w_ > 0 && viewport_h_ > 0) {
      Surface scratch;
      scratch.resize(viewport_w_, viewport_h_);
      render(scratch);
    }
    action_ = AppAction::None;
    dirty_ = true;
    handle(e);
    return action_;
  }
  void mark_dirty() { dirty_ = true; }
  void draw(Surface& s) { render(s); }
  // Advances agents. Returns true if anything changed and the screen needs
  // rebuilding: an attached client watching an idle agent should cost nothing.
  bool service();
  void collect_session_fds(std::vector<int>& out) const;
  bool running() const { return running_; }
  Store& store() { return ws_.store(); }
  const Theme& theme() const { return active_theme(); }
  Filters& filters() { return filters_; }
  // Advances a few times a second while an agent is working, so a spinner can
  // turn. It stops when nothing is busy, keeping an idle daemon free.
  uint64_t anim() const { return anim_; }

  // The selected folder, followed by its path: the list re-sorts as chats
  // move, and an index would land on whichever folder moved into its place.
  int project_index() const;
  // A sub-project of the selected folder, or null for the whole folder.
  const SubProject* current_sub() const;
  void select_sub(const std::string& name);
  // Where a new chat in the selection runs: the sub-project's folder, else
  // the project's.
  std::string selected_cwd() const;
  // The sub-project a running chat is in: the one it was started from while
  // its id is still unknown, else what the store says.
  std::string live_sub(const LiveSession& s) const;
  void select_project(int i);
  void force_select(int p, int s);
  const Project* current_project() const;
  const SessionRef* current_session() const;  // the selected stored chat, if any

  // The unified chat list selects either a running session or a stored
  // transcript; exactly one is active at a time.
  void select_live(LiveSession* s);
  void select_stored(const std::string& transcript_path);
  // Explicit opening resumes stopped/stored chats and focuses existing runs.
  // Selection alone remains a read-only preview.
  bool open_selected_chat();
  std::string session_title(const LiveSession& session) const;
  LiveSession* selected_live() const { return selected_live_; }

  // The chat list's order, held while the list is in use: rows that move
  // under the pointer or the cursor get the wrong chat opened. `ids` is the
  // order last shown, for `scope` (the folder and sub-project listed).
  struct ChatOrder {
    std::string scope;
    std::vector<std::string> ids;
    int64_t held_until_ms = 0;
  };
  ChatOrder& chat_order() { return chat_order_; }
  const std::string& selected_path() const { return selected_path_; }

  // Spawns an agent in `cwd` and gives it a pane. Returns false if the binary
  // could not be started.
  bool spawn_agent(const std::string& agent, const std::string& cwd);
  // Continues an existing conversation. `fork` branches it into a new session
  // instead of reopening the original. Both shell out to the agent's own
  // resume/fork support rather than touching its transcript files.
  bool spawn_continuation(const std::string& agent, const std::string& session_id,
                          const std::string& cwd, bool fork);
  // Runs an arbitrary command in a pane. Used for agents mico has no adapter
  // for, and for testing the pty plane without starting a real agent.
  bool spawn_raw(std::vector<std::string> argv, const std::string& cwd);
  // A file:// link, opened in $VISUAL / $EDITOR at its #L line, in a pane.
  void open_in_editor(const std::string& url);

  // A directory that actually exists to run in. A tracked folder can be gone —
  // a disconnected network drive, a deleted checkout — and spawning there just
  // produces a dead pane. Returns `want` when it is usable, else the selected
  // folder, another tracked folder, or $HOME.
  std::string usable_cwd(const std::string& want) const;
  // Deferred: the caller is usually the pane being removed.
  // Queued, and carried out before the next layout. Several can be queued in
  // one frame: archiving a selection stops every idle agent in it.
  void close_session(LiveSession* s) { ws_.close(s); }
  bool has_live() const { return ws_.has_live(); }
  std::vector<LiveSession*> live_sessions() const;
  // Moves focus to the pane showing `s`, if one is on screen.
  void focus_session(LiveSession* s);

  // A one-line modal question. `action` identifies what to do with the answer;
  // `carry` threads an earlier answer through a two-step question.
  void ask(std::string label, std::string action, std::string initial = {},
           std::string carry = {});
  // Choosing a folder to track: browses from home, a folder at a time.
  void pick_folder();
  // Every chat in every tracked folder, running ones first, to jump to by
  // name (Ctrl+K, F12, :go).
  void open_switcher();
  // Starting an agent: which one, then where (F7, :new). A command given
  // skips to where.
  void open_new_agent(std::string command = {});
  // Adding a sub-project to the folder `project`: which folder it runs in —
  // the project's own, or one under it — then its name.
  void pick_subproject_folder(const std::string& project);

  // Routes every mouse event to `p` until the button is released. Without it a
  // drag dies the moment the pointer leaves the pane that started it.
  void capture_mouse(Pane* p) { mouse_capture_ = p; }

  // A popup of choices at `screen_pos`. Typing narrows it. `title` names it
  // in its border.
  void open_menu(Pane* owner, Point screen_pos, std::vector<MenuItem> items,
                 std::string title = {});
  // A centered picker of `owner`'s: choosing an item sends its id to the
  // pane's on_action(), as a menu does. The cursor starts on `cursor_item`.
  void open_picker(Pane* owner, std::string title, std::vector<PickItem> items, int cursor_item = 0,
                   std::string footer = {});
  // The same, sitting on top of the control that opened it: its bottom edge
  // on the row above `anchor`, its left edge on anchor.x. The query row only
  // shows for a long list; typing still filters a short one.
  void open_picker_above(Pane* owner, Point anchor, std::string title, std::vector<PickItem> items,
                         int cursor_item = 0);
  void close_menu() { menu_.reset(); }
  // Leave. In a client this closes the connection and leaves every agent
  // running; locally there is nothing to detach from, so it exits.
  void detach() { action_ = AppAction::Detach; }
  // Stop everything, agents included. Deliberate only: never bound to a bare
  // keystroke, because it is not recoverable.
  void shutdown() { action_ = AppAction::Shutdown; running_ = false; }
  void set_status(std::string s) {
    status_ = std::move(s);
    status_at_ = ++tick_;
  }
  // Runs one mico command, as typed on the command line.
  void run_command(std::string line);
  void open_command_line() { cmd_active_ = true; cmd_hist_ = -1; }

  // Selection mode hands the mouse back to the terminal so its own
  // click-drag-copy works, and stops repainting — a redraw mid-drag clears the
  // selection in most terminals, which makes copying impossible in practice.
  bool selection_mode() const { return selection_; }
  void toggle_selection() {
    selection_ = !selection_;
    sel_drawn_ = false;
    // Both edges need a clean frame: entering, so selection starts from an
    // untouched screen; leaving, so the terminal's own highlight is painted
    // over rather than left behind on cells the diff never rewrites.
    redraw_ = true;
  }
  // True while mico wants mouse reporting. The renderer emits the mode change.
  bool wants_mouse() const { return !selection_; }
  // Button-event tracking (mode 1002) only reports motion while a button is
  // held, which is right for everything except a popup menu: browsing one by
  // hover needs motion with no button down, mode 1003. Scoped to just while a
  // menu is open, so the ordinary case still avoids the flood of motion
  // events 1003 sends for the whole terminal.
  bool wants_motion() const { return bool(menu_); }

  // Queues text for the system clipboard. Delivered as OSC 52 inside the next
  // frame, so it crosses an ssh connection like everything else.
  // Both ways at once: OSC 52 through the terminal (and over ssh), and the X
  // clipboard directly, for terminals that ignore OSC 52 such as GNOME's.
  void copy_to_clipboard(std::string text);
  std::string take_clipboard() { return std::exchange(clipboard_, {}); }
  // A link the user clicked, for whoever owns their terminal to open.
  std::string take_open_url() { return std::exchange(open_url_, {}); }

  // Repaint every cell. Terminals leave selection highlighting on cells a diff
  // renderer never rewrites, so there has to be a way to say "draw it all".
  void force_redraw() { redraw_ = true; }
  bool take_redraw() { return std::exchange(redraw_, false); }
  // Test hook: start the chat view scrolled back N rows, to exercise the
  // backward-parsing path without an interactive session.
  int start_scroll = 0;
  // Search across chats: opens the Search tab with `query` typed in (empty
  // leaves the last one). The tab takes the request on its next frame.
  void open_search(std::string query);
  bool take_search_request(std::string* query) {
    if (!search_requested_) return false;
    search_requested_ = false;
    *query = std::move(search_request_);
    return true;
  }
  // Opens the chat a search found — its running session when there is one —
  // at the match, with the query lit and the find bar open.
  void open_search_hit(const SearchHit& hit, const std::string& query) {
    open_at(hit.path, hit.offset, query);
  }
  // Opens the chat whose transcript is `path` — its running session when
  // there is one — scrolled to the line at byte `offset`, `query` lit.
  void open_at(const std::string& path, uint64_t offset, const std::string& query);
  // For the pane showing transcript `path`: where to reveal, once.
  bool take_reveal(const std::string& path, uint64_t* offset, std::string* query) {
    if (!reveal_ || reveal_->path != path) return false;
    *offset = reveal_->offset;
    *query = std::move(reveal_->query);
    reveal_.reset();
    return true;
  }

  // What "/" offers for `agent` in `cwd`, and what "@" can name there. Both
  // are kept here, not in a pane, so a folder is asked once for all its chats.
  const std::vector<SlashCommand>& slash_commands(const std::string& agent, const std::string& cwd) {
    return ws_.commands().get(agent, cwd);
  }
  uint64_t commands_version() const { return ws_.commands().version(); }
  const std::vector<std::string>& project_files(const std::string& cwd) { return ws_.files().get(cwd); }
  uint64_t files_version() const { return ws_.files().version(); }

  // --- the sidebar as a filter ------------------------------------------
  // The Projects and Chats column stays on every tab. On Sessions it picks
  // the chat to open; on Usage, Search and Tools it picks what they cover: a
  // folder or every folder, then the whole folder or one chat in it.
  bool filtering() const { return tab_ != 0; }
  bool all_folders() const { return all_folders_; }
  void set_all_folders(bool on) {
    if (on == all_folders_) return;
    all_folders_ = on;
    if (on) chat_filter_ = false;
    filter_version_++;
    mark_dirty();
  }
  bool chat_filter() const { return chat_filter_ && !all_folders_; }
  void set_chat_filter(bool on) {
    if (on == chat_filter_) return;
    chat_filter_ = on;
    filter_version_++;
    mark_dirty();
  }
  struct ViewFilter {
    const Project* project = nullptr;  // null: every folder
    const SubProject* sub = nullptr;   // null: the whole folder
    std::string chat_path;             // empty: every chat in the folder
    std::string label;                 // "all folders", "mico", "mico · Fix the tabs"
  };
  ViewFilter view_filter() const;
  // Changes whenever the filter does, so a view that computed something
  // from it (a search) knows to start again.
  uint64_t filter_version() const { return filter_version_; }
  // The tracked folders as the filter narrows them, for a view that walks
  // sessions itself: one folder, with only the one chat when there is one.
  std::vector<Project> filtered_projects() const;
  // True when a chat of folder `project_name`, transcript `path`, is inside
  // the filter: for views that walk their own index of chats.
  bool in_filter(const std::string& project_name, const std::string& path) const;

  // Test hook: open a tab before the first frame, for `mico --dump --usage`.
  void open_tab(size_t i) { show_tab(i); }

 private:
  std::unique_ptr<Workspace> own_ws_;  // when constructed without one
  Workspace& ws_;

  struct Menu {
    Pane* owner = nullptr;
    Point at{};
    Rect rect{};  // where the last frame drew it
    Picker picker;
    // A flow is a picker mico answers itself rather than a pane's menu:
    // "switch", "agent", "agent_dir", "folder". `carry` holds an earlier
    // step's answer.
    std::string flow;
    std::string carry;
    bool centered = false;  // a palette in the middle, not a popup at a point
    bool above = false;     // bottom edge pinned to the row above `at`
  };
  // Opens `flow` as a centered picker.
  void open_flow(std::string flow, std::string title, std::string footer, std::string carry = {},
                 std::string query = {});
  std::vector<PickItem> flow_items(const std::string& flow, const std::string& query);
  void flow_chosen(const PickItem& it);
  void flow_complete(const PickItem& it);
  void flow_back();
  std::vector<PickItem> switcher_items() const;
  std::vector<PickItem> folder_items(const std::string& query, bool tracked);
  void remember_command(const std::string& cmd);
  // Commands typed into the new-agent picker, newest first; kept in
  // $XDG_CONFIG_HOME/mico/commands.
  std::vector<std::string> recent_commands_;
  bool recent_loaded_ = false;
  // The last folder listed while browsing, so a keystroke does not list it again.
  std::string browse_dir_;
  std::vector<std::string> browse_names_;

  void build_layout();
  void load_layout();
  void save_layout();
  // How the view was left, kept in ~/.config/mico/view: the density, the tab,
  // the folder, sub-project and chat selected, the sidebar's filters, the
  // Diff tab's choices and the command history.
  void load_view();
  std::string view_state() const;
  std::string saved_view_;  // what the file holds
  void show_tab(size_t i);
  void reap_sessions();
  void render(Surface& s);
  void render_chrome(Surface& s, const Node::Placed& p, bool focused);
  void render_status(Surface& s);
  void render_menu(Surface& s);
  void render_prompt(Surface& s);
  void render_command(Surface& s);
  void render_tabs(Surface& s);
  bool command_key(const KeyEvent& k);
  bool prompt_key(const KeyEvent& k);
  void prompt_submit();
  void render_dividers(Surface& s);
  bool divider_press(Point p);
  void handle(const InputEvent& e);
  void handle_paste(const std::string& text);
  void handle_key(const KeyEvent& k);
  void handle_mouse(const MouseEvent& m);

  // Mouse text selection, in screen coordinates. A left-drag over any pane
  // highlights the cells it covers and copies the text (OSC 52) on release; a
  // plain click still reaches the pane. The range is clamped to the pane it
  // started in so dragging past an edge cannot select a neighbour's chrome.
  void cancel_selection();
  bool normalized_selection(Point& top, Point& bot) const;
  std::string selected_text(const Surface& s) const;
  void render_selection(Surface& s);
  bool menu_mouse(const MouseEvent& m);
  // Carries out what the menu's picker reported; true once the menu is done.
  void menu_result(Picker::Result r);
  void focus_next(int delta);
  void notify_state_changed();

  DiffSettings diff_;
  ChatOrder chat_order_;
  std::string search_request_;
  bool search_requested_ = false;
  struct Reveal {
    std::string path;
    uint64_t offset;
    std::string query;
  };
  std::optional<Reveal> reveal_;
  Filters filters_;
  Term term_;
  LiveSession* focus_after_build_ = nullptr;  // pane to focus once layout rebuilds
  bool focus_chat_after_build_ = false;       // focus the main chat pane then
  bool layout_dirty_ = true;
  std::unique_ptr<Node> root_;
  std::map<LiveSession*, PanePtr> session_panes_;
  int viewport_w_ = 0, viewport_h_ = 0;
  // Split fractions the user dragged, kept across rebuilds and restarts.
  std::vector<std::vector<float>> layout_fracs_;
  std::vector<Node::Placed> placed_;
  std::vector<Node::Divider> dividers_;

  // An in-progress split drag. Held by pointer into the layout tree, so it is
  // cleared whenever the tree is rebuilt.
  struct Drag {
    Node* node = nullptr;
    size_t index = 0;
    Rect area{};
    Rect hit{};
  };
  std::optional<Drag> drag_;
  Pane* mouse_capture_ = nullptr;
  std::optional<Menu> menu_;

  struct Prompt {
    std::string label, action, text, carry;
  };
  std::optional<Prompt> prompt_;

  // mico's own command line, kept separate from the box that types at an
  // agent: ":" is unambiguous, where "/model" belongs to the agent.
  // Views across the top. Only one exists today; the strip is here so adding
  // an overview or a log view is a list entry rather than a layout rewrite.
  struct Tab {
    std::string name;
  };
  std::vector<Tab> tabs_{{"Sessions"}, {"Usage"}, {"Search"}, {"Tools"}, {"Diff"}, {"Settings"}};
  size_t tab_ = 0;
  std::vector<Rect> tab_hit_;
  int tab_row_ = -1;
  int cmd_row_ = -1;

  bool cmd_active_ = false;
  std::string cmd_text_;
  std::vector<std::string> cmd_history_;
  int cmd_hist_ = -1;

  int project_ = 0;
  std::string project_path_;  // what project_ was when chosen; wins over it
  std::string sub_;           // selected sub-project's name; "" whole folder
  bool all_folders_ = false;
  bool chat_filter_ = false;
  uint64_t filter_version_ = 1;
  std::string selected_path_;       // transcript path of the selected stored chat
  LiveSession* selected_live_ = nullptr;
  size_t focus_ = 0;
  bool running_ = true;
  AppAction action_ = AppAction::None;
  bool selection_ = false;
  bool sel_drawn_ = false;
  bool sel_has_ = false;         // a selected range exists
  bool sel_dragging_ = false;    // the left button is down and may select
  bool sel_copy_pending_ = false;
  Point sel_anchor_{};
  Point sel_cursor_{};
  Rect sel_area_{};              // pane interior the range is clamped to
  bool redraw_ = false;
  bool dirty_ = true;
  std::string clipboard_;
  std::string open_url_;
  std::string status_;
  // Feedback is transient: it takes the bar for a while, then gives it back.
  uint64_t status_at_ = 0;
  uint64_t tick_ = 0;
  uint64_t anim_ = 0;
  // Consecutive wakeups where stdin claimed readable but produced nothing.
  // With VMIN=0 a real EOF is indistinguishable from "no data" on a single
  // read, so it is detected by repetition instead — otherwise closing the
  // terminal spins the event loop at 100% CPU.
  int stdin_silent_ = 0;
};

}  // namespace mico
