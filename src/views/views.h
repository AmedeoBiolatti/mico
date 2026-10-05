#pragma once
#include "adapters/adapters.h"
#include "model/background.h"
#include "ui/pane.h"
#include "ui/picker.h"
#include "ui/theme.h"

namespace mico {
class App;
class LiveSession;
class UsageIndex;
class ChatSearch;
class ActivityIndex;
struct SessionState;

inline std::string agent_label(std::string_view agent) {
  if (const Adapter* a = adapter_for(agent)) return std::string(a->label());
  return std::string(agent);
}

// A chat's state as the sidebar shows it: its mark, the mark's colour, and
// the word for it. `live` is null for a stored transcript. `rank` orders
// chats by how much they want you, which is also how a folder sums up its
// chats: 0 needs you (a question or a permission), 1 a reply not yet looked
// at, 2 working (a spinner, turned by `anim`), 3 ready, 4 saved or stopped.
struct ChatState {
  char32_t glyph;
  Color color;
  const char* word;
  int rank;
};
ChatState chat_state(const LiveSession* live, const Theme& th, uint64_t anim = 0);
// One frame of the braille spinner a working agent shows, for App::anim().
char32_t spinner_glyph(uint64_t anim);

// One state chip's choices, as picker items: the values mico can set (the
// current one checked), or the agent's own picker, plus "copy value". `live`
// enables the actions that talk to a running agent; a stored transcript can
// only be read from. `cursor` gets the current value's index, `title` the
// chip's label and value.
std::vector<PickItem> chip_pick_items(const SessionState& st, const std::string& key, bool live,
                                      const std::string& agent, int* cursor = nullptr,
                                      std::string* title = nullptr);
// Opens those choices as a picker of `owner`'s, sitting on top of the chip
// that was clicked: `above` is the chip's first cell, in screen coordinates.
void open_chip_picker(App* app, Pane* owner, Point above, const SessionState& st,
                      const std::string& key, bool live, const std::string& agent);
// The same choices as menu items, for the chip's right-click menu.
std::vector<MenuItem> chip_menu(const SessionState& st, const std::string& key, bool live);
std::vector<MenuItem> chip_menu(const SessionState& st, const std::string& key, bool live,
                                const std::string& agent);
// Command an agent understands for changing `key`, or empty if none is known.
std::string chip_command(const std::string& key);
// What an agent runs in the background, in words: "2 monitors · 1 command".
std::string background_summary(const std::vector<BackgroundTask>& tasks);
// How far a task has got, when it prints progress: "▕████▌    ▏ 45% ·
// 450/1000 · 2m left", the bar `cols` cells wide (0 for none). The time left
// is the task's own estimate, else worked out from its pace. Empty when it
// prints none.
std::string background_progress(const BackgroundTask& t, int cols);
// The percentage of the first running task that shows one: " 45%", or "".
std::string background_percent(const std::vector<BackgroundTask>& tasks);
// Each task as a picker row: what it is, how long it has run, its last event.
// Choosing one gives "bg:<offset>", the line of the call that started it.
std::vector<PickItem> background_items(const std::vector<BackgroundTask>& tasks, const Theme& th);
}  // namespace mico

namespace mico {
PanePtr make_project_list();
PanePtr make_chat_list();
PanePtr make_chat_view();
PanePtr make_session_pane(LiveSession* s);
// The index outlives the pane so switching tabs does not throw away the
// per-file cache and re-read every transcript.
PanePtr make_usage_view(UsageIndex& index);
// Search across every chat. The search outlives the pane, so leaving the tab
// neither loses the results nor stops a search still running.
PanePtr make_search_view(ChatSearch& search);
// Where the agents' time went, by kind of tool call and by command.
PanePtr make_tools_view(ActivityIndex& index);
// What the agents changed in files, and which chat changed it. Its settings
// outlive the pane, which is rebuilt whenever the sidebar selection moves.
struct DiffSettings;
PanePtr make_diff_view(ActivityIndex& index, DiffSettings& settings);
// The selected folder's repository: its work trees and who works in each,
// its branches, what is uncommitted and its commits, each with the chat
// behind it. What it has focused outlives the pane.
struct GitTabState;
PanePtr make_git_view(GitTabState& state);
// What the chat renders (pictures, LaTeX, charts…) and what agents are given,
// each switched on or off.
PanePtr make_settings_view();
}  // namespace mico
