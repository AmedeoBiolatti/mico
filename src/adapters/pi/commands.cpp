#include "adapters/pi/pi.h"

#include <dirent.h>
#include <unistd.h>

#include <cstdio>

#include "adapters/command_table.h"

namespace mico {
namespace {

// pi's BUILTIN_SLASH_COMMANDS.
constexpr Row kPi[] = {
    {"settings", "Open settings menu", ""},
    {"model", "Select model (opens selector UI)", "<provider/model>"},
    {"tree", "Navigate session tree (switch branches)", ""},
    {"thinking", "Set thinking level", "<level>"},
    {"scoped-models", "Enable/disable models for Ctrl+P cycling", ""},
    {"export", "Export session (HTML default, or specify path: .html/.jsonl)", ""},
    {"import", "Import and resume a session from a JSONL file", ""},
    {"share", "Share session as a secret GitHub gist", ""},
    {"bug", "Report a bug to the Pi developers", "<description>"},
    {"copy", "Copy last agent message to clipboard", ""},
    {"name", "Set session display name", ""},
    {"session", "Show session info and stats", ""},
    {"changelog", "Show changelog entries", ""},
    {"hotkeys", "Show all keyboard shortcuts", ""},
    {"fork", "Create a new fork from a previous user message", ""},
    {"clone", "Duplicate the current session at the current position", ""},
    {"trust", "Save project trust decision for future sessions", ""},
    {"login", "Configure provider authentication", "<provider>"},
    {"logout", "Remove provider authentication", ""},
    {"new", "Start a new session", ""},
    {"compact", "Manually compact the session context", ""},
    {"resume", "Resume a different session", ""},
    {"reload", "Reload keybindings, extensions, skills, prompts, themes, and context files", ""},
    {"quit", "Quit pi", ""},
};

// omp's built-in commands.
constexpr Row kOmp[] = {
    {"settings", "Open settings menu", ""},
    {"setup", "Open provider setup", ""},
    {"plan", "Toggle plan mode (agent plans before executing)", "[prompt]"},
    {"plan-review", "Re-open the plan review for the latest plan (plan mode only)", ""},
    {"vibe", "Toggle vibe mode (direct persistent fast/good worker sessions; read-only toolset)", "[prompt]"},
    {"goal", "Toggle goal mode (persistent autonomous objective for this session)", "[objective]"},
    {"guided-goal", "Have the agent interview you in chat, then set up goal mode", "[rough objective]"},
    {"loop", "Toggle loop mode: the next prompt re-submits after every yield", "[count|duration] [prompt]"},
    {"queue", "Queue a message for after the agent yields", "<message>"},
    {"model", "Switch model for this session", ""},
    {"switch", "Switch model for this session; accepts fuzzy ids, provider/id, @role, :level", "[model]"},
    {"fast", "Toggle priority service tier", ""},
    {"skillful", "Toggle listing available skills in the system prompt (session only)", ""},
    {"extended-context", "Toggle extended context windows", ""},
    {"computer", "Toggle the native computer-use eval prelude for this session", ""},
    {"prewalk", "Arm or restart a one-shot model handoff", ""},
    {"advisor", "Toggle the advisor (a second model that reviews each turn and injects notes)", ""},
    {"export", "Export session to HTML file", "[--themes] [path]"},
    {"trace", "Open this session's trace in the stats dashboard", ""},
    {"dump", "Copy session transcript to clipboard", ""},
    {"share", "Share session via an encrypted link", ""},
    {"collab", "Share this session live via a relay", "[start|view|list|stop|status]"},
    {"join", "Join a shared collab session", "<link>"},
    {"leave", "Leave the collab session", ""},
    {"browser", "Toggle browser eval-prelude headless vs visible mode", ""},
    {"copy", "Pick text or code from the conversation to copy", ""},
    {"open", "Open the last link from the conversation in your browser", ""},
    {"ssh", "Manage SSH hosts (add, list, remove)", "<subcommand>"},
    {"new", "Start a new session", ""},
    {"fresh", "Reset provider stream state without changing the local transcript", ""},
    {"clear", "Clear the conversation context in place, keeping the session", ""},
    {"delete", "Delete the current session and start a new one", ""},
    {"compact", "Manually compact the session context", ""},
    {"shake", "Drop heavy content from context (tool results, large blocks)", ""},
    {"handoff", "Summarize the session into a handoff document and compact in place", "[focus instructions]"},
    {"resume", "Resume a different session", "[session id|@claude|@codex]"},
    {"pin", "Pin or unpin a session at the top of the resume list", "[session id]"},
    {"btw", "Ask a side question, or browse this session's BTW history", "[question]"},
    {"tan", "Run a full background agent on tangential work", "<work>"},
    {"omfg", "Forge a TTSR rule from a complaint to stop a recurring behavior", "<complaint>"},
    {"cleanse", "Detect and fix project diagnostics with weighted parallel subagents", "[request] [--all]"},
    {"retry", "Retry the last failed agent turn", ""},
    {"debug", "Open debug tools selector", ""},
    {"memory", "Inspect and operate memory maintenance", ""},
    {"rename", "Rename the current session (omit title to generate)", "[title]"},
    {"move", "Move the current session to a different directory", "[<path>]"},
    {"wt", "Move this session into a new worktree, changes included", "[<branch>]"},
    {"add-dir", "Add a workspace directory to this session (multi-root)", "<path>"},
    {"remove-dir", "Remove a workspace directory from this session", "<path>"},
    {"dirs", "List this session's workspace directories", ""},
    {"restart", "Restart omp with the same launch flags, resuming this session", ""},
    {"force", "Force next turn to use a specific tool", "<tool-name> [prompt]"},
    {"live", "Start Codex-backed realtime voice mode", ""},
    {"record", "Start or stop recording this screen to a replayable file", ""},
    {"pause", "Freeze all agents (main, subagents, advisor) until resumed", ""},
    {"marketplace", "Manage marketplace plugin sources and installed plugins", ""},
    {"plugins", "View and manage installed plugins", ""},
    {"reload-plugins", "Reload all plugins (skills, commands, hooks, tools, agents, MCP)", ""},
    {"todo", "View or modify the agent's todo list", ""},
    {"session", "Session management commands", ""},
    {"jobs", "Show async background jobs status", ""},
    {"usage", "Show provider usage and limits", ""},
    {"stats", "Launch the local stats dashboard", ""},
    {"changelog", "Show changelog entries", ""},
    {"hotkeys", "Show all keyboard shortcuts", ""},
    {"tools", "Show tools currently visible to the agent", ""},
    {"context", "Show estimated context usage breakdown", ""},
    {"extensions", "Open Extension Control Center dashboard", ""},
    {"agents", "Open the agents hub (per-agent model, prewalk, and advisor)", ""},
    {"git", "Open the git UI (split diff viewer, staging, commit composer)", "[revision]"},
    {"hub", "Open the live Agent Hub", ""},
    {"branch", "Rewind to a previous message, keeping the old path as a branch", ""},
    {"fork", "Create a new fork from a previous message", ""},
    {"tree", "Navigate session tree (switch branches)", ""},
    {"login", "Login with OAuth provider", "[provider]"},
    {"logout", "Logout from OAuth provider", "[provider]"},
    {"mcp", "Manage MCP servers (add, list, remove, test)", "<subcommand>"},
    {"skills", "Search, install, and update skills from the skills.omp.sh registry", ""},
    {"exit", "Exit the application", ""},
    {"quit", "Quit the application", ""},
};

// A markdown file's "description:" from its frontmatter, or its first line of
// prose when it has none.
std::string describe_file(const std::string& path) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return {};
  char buf[4096];
  const size_t n = fread(buf, 1, sizeof buf, f);
  fclose(f);
  std::string_view s(buf, n);
  bool front = s.starts_with("---");
  size_t i = front ? s.find('\n') + 1 : 0;
  std::string first;
  while (i < s.size()) {
    size_t e = s.find('\n', i);
    if (e == std::string_view::npos) e = s.size();
    std::string_view line = s.substr(i, e - i);
    i = e + 1;
    if (front) {
      if (line.starts_with("---")) { front = false; continue; }
      if (line.starts_with("description:")) return one_line(line.substr(12));
      continue;
    }
    if (first.empty() && !one_line(line).empty() && !line.starts_with("#")) first = one_line(line);
    if (!first.empty()) break;
  }
  return first;
}

// Calls fn(name, path) for each "*.md" in `dir`, name without the extension.
template <class F>
void each_markdown(const std::string& dir, F&& fn) {
  DIR* d = opendir(dir.c_str());
  if (!d) return;
  while (dirent* e = readdir(d)) {
    std::string_view n(e->d_name);
    if (n.size() > 3 && n.ends_with(".md")) fn(std::string(n.substr(0, n.size() - 3)), dir + "/" + std::string(n));
  }
  closedir(d);
}

// Calls fn(name, path) for each "<name>/SKILL.md" in `dir`.
template <class F>
void each_skill(const std::string& dir, F&& fn) {
  DIR* d = opendir(dir.c_str());
  if (!d) return;
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] == '.') continue;
    const std::string path = dir + "/" + e->d_name + "/SKILL.md";
    if (access(path.c_str(), R_OK) == 0) fn(std::string(e->d_name), path);
  }
  closedir(d);
}

}  // namespace

std::vector<SlashCommand> PiAdapter::builtin_commands() const { return copy(kPi); }
std::vector<SlashCommand> OmpAdapter::builtin_commands() const { return copy(kOmp); }

void PiFamilyAdapter::file_commands(const std::string& cwd, const std::string& home,
                                    std::vector<SlashCommand>& out) const {
  // pi: prompt templates are "/name", skills "/skill:name"; the project's
  // copy wins over the user's.
  const std::string user = home + "/" + std::string(dot_dir()) + "/agent";
  const std::string project = cwd + "/" + std::string(dot_dir());
  for (const std::string& base : {project, user}) {
    each_markdown(base + "/prompts", [&](std::string name, const std::string& path) {
      out.push_back({std::move(name), describe_file(path), ""});
    });
    each_skill(base + "/skills", [&](const std::string& name, const std::string& path) {
      out.push_back({"skill:" + name, describe_file(path), ""});
    });
  }
}

ChipControl PiFamilyAdapter::chip_control(std::string_view key) const {
  // Both open a fuzzy-search picker rather than taking a value inline, so
  // mico cannot offer a one-shot set the way it does for claude and codex.
  ChipControl c;
  if (key == "model") c.picker = "/model";
  else if (key == "effort") c.picker = "/thinking";
  return c;
}

}  // namespace mico
