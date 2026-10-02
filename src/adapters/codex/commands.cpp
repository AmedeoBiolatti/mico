#include "adapters/codex/codex.h"

#include "adapters/command_table.h"

namespace mico {
namespace {

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

}  // namespace

std::vector<SlashCommand> CodexAdapter::builtin_commands() const { return copy(kCodex); }

ChipControl CodexAdapter::chip_control(std::string_view key) const {
  ChipControl c;
  if (key == "effort") {
    c.values = {"low", "medium", "high"};
    c.set_prefix = "/model ";
  } else {
    return Adapter::chip_control(key);
  }
  return c;
}

}  // namespace mico
