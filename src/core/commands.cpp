#include "core/commands.h"

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "core/json.h"
#include "core/log.h"
#include "core/pty.h"

namespace mico {

namespace {

int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// A table row; std::string cannot be constexpr.
struct Row {
  const char* name;
  const char* description;
  const char* hint;
};

// Claude's interactive commands: the ones that open its own screens, which a
// `claude -p` process does not offer and so does not list. Everything else —
// /compact, /model, skills, plugins, your own commands — comes from claude.
constexpr Row kClaudeInteractive[] = {
    {"add-dir", "Add a new working directory", "<path>"},
    {"background", "Send this session to the background and free the terminal", "[prompt]"},
    {"branch", "Create a branch of the current conversation at this point", "[name]"},
    {"btw", "Ask a quick side question without interrupting the main conversation", "[question]"},
    {"cd", "Move this session to a new working directory", "<path>"},
    {"copy", "Copy Claude's last response to clipboard (or /copy N for the Nth-latest)", ""},
    {"exit", "Exit the CLI", ""},
    {"export", "Export the current conversation to a file or clipboard", "[filename]"},
    {"feedback", "Send feedback to Anthropic or report a bug", "[report]"},
    {"fork", "Spawn a background agent that inherits the full conversation", "<directive>"},
    {"help", "Show help and available commands", ""},
    {"hooks", "View hook configurations for tool events", ""},
    {"ide", "Manage IDE integrations and show status", ""},
    {"keybindings", "Open your keyboard shortcuts file", ""},
    {"login", "Sign in with your Anthropic account", ""},
    {"logout", "Sign out from your Anthropic account", ""},
    {"memory", "Edit CLAUDE.md files and memory settings", ""},
    {"permissions", "Manage allow and deny tool permission rules", ""},
    {"plan", "Enable plan mode or view the current session plan", "[open|<description>]"},
    {"plugin", "Manage Claude Code plugins", ""},
    {"release-notes", "View release notes", ""},
    {"remote-control", "Control this session from your phone or claude.ai/code", ""},
    {"resume", "Resume a previous conversation", "[conversation id or search term]"},
    {"rewind", "Restore the code and/or conversation to a previous point", ""},
    {"skills", "List available skills", ""},
    {"status", "Show Claude Code status including version, model, account and API connectivity", ""},
    {"statusline", "Set up Claude Code's status line UI", ""},
    {"tasks", "View and manage everything running in the background", ""},
    {"teleport", "Send this session to the cloud, or resume one from claude.ai", ""},
    {"terminal-setup", "Install the Shift+Enter key binding for newlines", ""},
    {"theme", "Change the theme", ""},
    {"voice", "Toggle voice mode", "[hold|tap|off]"},
};

// Codex's menu, as codex 0.158 lists it.
constexpr Row kCodex[] = {
    {"model", "choose what model and reasoning effort to use", ""},
    {"permissions", "choose what Codex is allowed to do", ""},
    {"ide", "include current selection, open files, and other context from your IDE", ""},
    {"keymap", "remap TUI shortcuts", ""},
    {"experimental", "toggle experimental features", ""},
    {"skills", "use skills to improve how Codex performs specific tasks", ""},
    {"import", "import setup, this project, and recent chats from Claude Code", ""},
    {"hooks", "view and manage lifecycle hooks", ""},
    {"review", "review my current changes and find issues", ""},
    {"rename", "rename the current thread", ""},
    {"new", "start a new chat during a conversation", ""},
    {"archive", "archive this session", ""},
    {"delete", "permanently delete this session", ""},
    {"resume", "resume a saved chat", ""},
    {"fork", "fork the current chat", ""},
    {"worktree", "start or continue a conversation in a new worktree", ""},
    {"init", "create an AGENTS.md file with instructions for Codex", ""},
    {"compact", "summarize conversation to prevent hitting the context limit", ""},
    {"recap", "summarize the current conversation now", ""},
    {"plan", "switch to Plan mode", ""},
    {"voice", "start or stop voice; use /voice settings to choose a voice", ""},
    {"goal", "set or view the goal for a long-running task", ""},
    {"agents", "open the agent command center", ""},
    {"side", "start a side conversation in an ephemeral fork", ""},
    {"copy", "copy the last response or part of it", ""},
    {"export", "export the conversation as markdown", ""},
    {"diff", "show git diff (including untracked files)", ""},
    {"mention", "mention a file", ""},
    {"status", "show current session configuration and token usage", ""},
    {"daemon", "manage the local background server", ""},
    {"cd", "change the current working directory", ""},
    {"pwd", "show the current working directory", ""},
    {"usage", "view account usage", ""},
    {"debug-config", "show config layers and requirement sources for debugging", ""},
    {"title", "configure which items appear in the terminal title", ""},
    {"statusline", "configure which items appear in the status line", ""},
    {"theme", "choose a syntax highlighting theme", ""},
    {"fast", "toggle fast mode", ""},
    {"vim", "toggle vim mode", ""},
    {"mcp", "list configured MCP tools; use /mcp verbose for details", ""},
    {"plugins", "browse plugins", ""},
    {"feedback", "send logs to maintainers", ""},
    {"rollout", "print the rollout file path", ""},
    {"ps", "list background terminals", ""},
    {"stop", "stop all background terminals", ""},
    {"clear", "clear the terminal and start a new chat", ""},
    {"subagents", "switch between this session's subagents", ""},
    {"logout", "log out of Codex", ""},
    {"quit", "exit Codex", ""},
};

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

template <size_t N>
std::vector<SlashCommand> copy(const Row (&a)[N]) {
  std::vector<SlashCommand> out;
  out.reserve(N);
  for (const Row& r : a) out.push_back({r.name, r.description, r.hint});
  return out;
}

// The first line of a description, trimmed: menus show one row.
std::string one_line(std::string_view s) {
  size_t a = s.find_first_not_of(" \t\r\n\"'");
  if (a == std::string_view::npos) return {};
  s.remove_prefix(a);
  s = s.substr(0, s.find('\n'));
  while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '"' || s.back() == '\''))
    s.remove_suffix(1);
  return std::string(s);
}

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

std::vector<SlashCommand> builtin_commands(std::string_view agent) {
  if (agent == "claude") return copy(kClaudeInteractive);
  if (agent == "codex") return copy(kCodex);
  if (agent == "pi") return copy(kPi);
  if (agent == "omp") return copy(kOmp);
  return {};
}

std::vector<SlashCommand> parse_claude_commands(std::string_view line) {
  std::vector<SlashCommand> out;
  js::Value commands;
  js::scan_object(line, [&](std::string_view k, js::Value v) {
    if (k != "response") return true;
    js::scan_object(v.raw, [&](std::string_view k2, js::Value v2) {
      if (k2 != "response") return true;
      js::scan_object(v2.raw, [&](std::string_view k3, js::Value v3) {
        if (k3 == "commands") commands = v3;
        return k3 != "commands";
      });
      return false;
    });
    return false;
  });
  js::scan_array(commands.raw, [&](js::Value c) {
    SlashCommand cmd;
    std::string desc;
    js::scan_object(c.raw, [&](std::string_view k, js::Value v) {
      if (k == "name") js::unescape_append(v.body(), cmd.name);
      else if (k == "description") js::unescape_append(v.body(), desc);
      else if (k == "argumentHint") js::unescape_append(v.body(), cmd.hint);
      return true;
    });
    // Commands for sessions the cloud started are no use in a terminal.
    if (cmd.name.empty() || cmd.name.starts_with("__")) return true;
    cmd.description = one_line(desc);
    out.push_back(std::move(cmd));
    return true;
  });
  return out;
}

std::vector<ModelOption> parse_claude_models(std::string_view line, std::vector<std::string>* efforts) {
  std::vector<ModelOption> out;
  if (efforts) efforts->clear();
  js::Value models;
  js::scan_object(line, [&](std::string_view k, js::Value v) {
    if (k != "response") return true;
    js::scan_object(v.raw, [&](std::string_view k2, js::Value v2) {
      if (k2 != "response") return true;
      js::scan_object(v2.raw, [&](std::string_view k3, js::Value v3) {
        if (k3 == "models") models = v3;
        return k3 != "models";
      });
      return false;
    });
    return false;
  });
  js::scan_array(models.raw, [&](js::Value m) {
    ModelOption o;
    js::scan_object(m.raw, [&](std::string_view k, js::Value v) {
      if (k == "value") js::unescape_append(v.body(), o.value);
      else if (k == "displayName") js::unescape_append(v.body(), o.label);
      else if (k == "description") js::unescape_append(v.body(), o.detail);
      else if (k == "resolvedModel") js::unescape_append(v.body(), o.resolved);
      else if (k == "supportedEffortLevels" && efforts) {
        js::scan_array(v.raw, [&](js::Value e) {
          std::string level;
          js::unescape_append(e.body(), level);
          if (!level.empty() &&
              std::find(efforts->begin(), efforts->end(), level) == efforts->end())
            efforts->push_back(std::move(level));
          return true;
        });
      }
      return true;
    });
    if (!o.value.empty()) out.push_back(std::move(o));
    return true;
  });
  return out;
}

std::vector<SlashCommand> file_commands(std::string_view agent, const std::string& cwd,
                                        const std::string& home) {
  std::vector<SlashCommand> out;
  if (agent != "pi" && agent != "omp") return out;
  // pi: prompt templates are "/name", skills "/skill:name"; the project's
  // copy wins over the user's.
  const std::string user = home + (agent == "pi" ? "/.pi/agent" : "/.omp/agent");
  const std::string project = cwd + (agent == "pi" ? "/.pi" : "/.omp");
  for (const std::string& base : {project, user}) {
    each_markdown(base + "/prompts", [&](std::string name, const std::string& path) {
      out.push_back({std::move(name), describe_file(path), ""});
    });
    each_skill(base + "/skills", [&](const std::string& name, const std::string& path) {
      out.push_back({"skill:" + name, describe_file(path), ""});
    });
  }
  return out;
}

// ------------------------------------------------------------------ catalog

CommandCatalog::~CommandCatalog() {
  for (auto& [key, e] : entries_) end_probe(e);
}

bool CommandCatalog::probing() const {
  for (const auto& [key, e] : entries_)
    if (e.probe.pid > 0) return true;
  return false;
}

void CommandCatalog::rebuild(Entry& e, const std::string& agent, const std::string& cwd) {
  const char* home = getenv("HOME");
  std::vector<SlashCommand> list = e.probed;
  // Claude's own answer comes first, so its wording wins a name both have.
  for (auto& c : builtin_commands(agent)) list.push_back(std::move(c));
  for (auto& c : file_commands(agent, cwd, home ? home : "")) list.push_back(std::move(c));
  std::stable_sort(list.begin(), list.end(),
                   [](const SlashCommand& a, const SlashCommand& b) { return a.name < b.name; });
  list.erase(std::unique(list.begin(), list.end(),
                         [](const SlashCommand& a, const SlashCommand& b) { return a.name == b.name; }),
             list.end());
  e.list = std::move(list);
  e.built_ms = now_ms();
  version_++;
}

const std::vector<SlashCommand>& CommandCatalog::get(const std::string& agent, const std::string& cwd) {
  Entry& e = entries_[agent + "\n" + cwd];
  const int64_t now = now_ms();
  // Files are re-read now and then, so a new skill shows without a restart.
  if (e.built_ms == 0 || now - e.built_ms > 30'000) rebuild(e, agent, cwd);
  // Claude is asked again every few minutes: an installed plugin, a new
  // command file.
  if (agent == "claude" && e.probe.pid < 0 && (e.probed_ms < 0 || now - e.probed_ms > 300'000))
    start_probe(e, cwd);
  return e.list;
}

void CommandCatalog::warm(const std::string& agent, const std::string& cwd) {
  if (agent != "claude" || cwd.empty()) return;
  Entry& e = entries_[agent + "\n" + cwd];
  if (e.probed_ms < 0 && e.probe.pid < 0) start_probe(e, cwd);
}

void CommandCatalog::start_probe(Entry& e, const std::string& cwd) {
  e.probed_ms = now_ms();  // an attempt, successful or not, waits its turn
  int in[2], out[2];
  if (pipe2(in, O_CLOEXEC) != 0) return;
  if (pipe2(out, O_CLOEXEC) != 0) {
    close(in[0]);
    close(in[1]);
    return;
  }
  const pid_t pid = fork();
  if (pid < 0) {
    for (int fd : {in[0], in[1], out[0], out[1]}) close(fd);
    return;
  }
  if (pid == 0) {
    dup2(in[0], 0);
    dup2(out[1], 1);
    const int null = open("/dev/null", O_WRONLY);
    if (null >= 0) dup2(null, 2);
    if (chdir(cwd.c_str()) != 0) _exit(127);
    setsid();
    scrub_agent_env();
    // No transcript: this process never has a conversation to keep.
    const char* argv[] = {"claude", "-p", "--no-session-persistence", "--input-format", "stream-json",
                          "--output-format", "stream-json", "--verbose", nullptr};
    execvp(argv[0], const_cast<char* const*>(argv));
    _exit(127);
  }
  close(in[0]);
  close(out[1]);
  static constexpr char kAsk[] =
      "{\"type\":\"control_request\",\"request_id\":\"mico-commands\",\"request\":{\"subtype\":\"initialize\"}}\n";
  if (write(in[1], kAsk, sizeof kAsk - 1) < 0) {}
  // Closing stdin now would end claude before it answers: it goes with the
  // process once the answer is in.
  e.probe.pid = pid;
  e.probe.in = in[1];
  e.probe.out = out[0];
  e.probe.started_ms = now_ms();
  e.probe.buf.clear();
  fcntl(out[0], F_SETFL, fcntl(out[0], F_GETFL) | O_NONBLOCK);
  logs::line("commands: asking claude in " + cwd + " (pid " + std::to_string(pid) + ")");
}

void CommandCatalog::end_probe(Entry& e) {
  if (e.probe.pid <= 0) return;
  kill(-e.probe.pid, SIGTERM);
  kill(e.probe.pid, SIGTERM);
  waitpid(e.probe.pid, nullptr, 0);
  if (e.probe.in >= 0) close(e.probe.in);
  if (e.probe.out >= 0) close(e.probe.out);
  e.probe = Probe{};
}

bool CommandCatalog::pump() {
  bool finished = false;
  for (auto& [key, e] : entries_) {
    Probe& p = e.probe;
    if (p.pid <= 0) continue;
    char buf[65536];
    bool eof = false;
    for (;;) {
      const ssize_t n = read(p.out, buf, sizeof buf);
      if (n > 0) { p.buf.append(buf, size_t(n)); continue; }
      if (n == 0) eof = true;
      break;
    }
    // The answer is one line; everything before it (hook notices) is skipped.
    bool answered = false;
    for (size_t at = p.buf.find("\"control_response\""); at != std::string::npos;
         at = p.buf.find("\"control_response\"", at + 1)) {
      const size_t start = p.buf.rfind('\n', at) == std::string::npos ? 0 : p.buf.rfind('\n', at) + 1;
      const size_t end = p.buf.find('\n', at);
      if (end == std::string::npos) break;  // not all here yet
      const std::string_view answer = std::string_view(p.buf).substr(start, end - start);
      std::vector<SlashCommand> got = parse_claude_commands(answer);
      if (got.empty()) continue;
      logs::line("commands: claude listed " + std::to_string(got.size()));
      // The same answer names the models and their effort levels: the chip
      // pickers' list, with descriptions the screen-scrape probe never had.
      std::vector<std::string> efforts;
      if (auto models = parse_claude_models(answer, &efforts); !models.empty()) {
        logs::line("commands: claude named " + std::to_string(models.size()) + " models");
        set_known_models("claude", std::move(models));
        if (!efforts.empty()) set_known_efforts("claude", std::move(efforts));
      }
      e.probed = std::move(got);
      const size_t nl = key.find('\n');
      const std::string agent = key.substr(0, nl), cwd = key.substr(nl + 1);
      end_probe(e);
      rebuild(e, agent, cwd);
      answered = true;
      break;
    }
    if (answered) {
      finished = true;
      continue;
    }
    if (eof || now_ms() - p.started_ms > 30'000) {
      logs::line(std::string("commands: claude gave no list (") + (eof ? "exited" : "timed out") + ")");
      end_probe(e);
      finished = true;
    }
  }
  return finished;
}

}  // namespace mico
