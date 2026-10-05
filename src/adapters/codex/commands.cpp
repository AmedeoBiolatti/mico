#include "adapters/codex/codex.h"

#include "adapters/command_table.h"
#include "base/json.h"

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
  // Codex's /model takes no value: typed with one, the line goes to the
  // model as a message. It opens a picker of models, then one of reasoning
  // levels, each a numbered menu (Max and Ultra behind "More reasoning…"), so
  // a choice is made by walking them, each row found by its name on screen.
  ChipControl c;
  c.labels = {{"none", "None"},   {"minimal", "Minimal"}, {"low", "Low"}, {"medium", "Medium"},
              {"high", "High"},   {"xhigh", "Extra high"}, {"max", "Max"}, {"ultra", "Ultra"}};
  if (key == "model") {
    c.source = ChipControl::Source::Models;
    c.steps = {"/model", "\r", "\x01pick:{label}", "\x01pick:{effort_label}|More reasoning"};
    c.picker = "/model";
  } else if (key == "effort") {
    c.source = ChipControl::Source::Efforts;
    c.values = {"low", "medium", "high", "xhigh"};
    c.steps = {"/model", "\r", "\x01pick:{model_label}", "\x01pick:{label}|More reasoning"};
    c.picker = "/model";
  } else {
    return Adapter::chip_control(key);
  }
  return c;
}

// {"models":[{"slug","display_name","description","visibility",
// "supported_reasoning_levels":[{"effort",…}]},…]}: those its picker lists.
bool CodexAdapter::read_command_probe(std::string_view output, bool ended, CommandProbeAnswer& out) const {
  if (!ended) return false;
  out = CommandProbeAnswer{};
  js::scan_object(output, [&](std::string_view k, const js::Value& v) {
    if (k != "models" || !v.is_array()) return true;
    js::scan_array(v.raw, [&](const js::Value& m) {
      ModelOption o;
      std::string visibility;
      js::scan_object(m.raw, [&](std::string_view mk, const js::Value& mv) {
        if (mk == "slug") js::unescape_append(mv.body(), o.value);
        else if (mk == "display_name") js::unescape_append(mv.body(), o.label);
        else if (mk == "description") js::unescape_append(mv.body(), o.detail);
        else if (mk == "visibility") js::unescape_append(mv.body(), visibility);
        else if (mk == "supported_reasoning_levels" && mv.is_array())
          js::scan_array(mv.raw, [&](const js::Value& l) {
            js::scan_object(l.raw, [&](std::string_view lk, const js::Value& lv) {
              if (lk == "effort" && lv.is_string()) o.efforts.emplace_back(lv.body());
              return true;
            });
            return true;
          });
        return true;
      });
      if (!o.value.empty() && (visibility.empty() || visibility == "list")) out.models.push_back(std::move(o));
      return true;
    });
    return false;
  });
  return !out.models.empty();
}

}  // namespace mico
