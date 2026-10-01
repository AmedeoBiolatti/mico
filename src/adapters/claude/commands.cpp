#include "adapters/claude/claude.h"

#include <algorithm>

#include "adapters/command_table.h"
#include "base/json.h"
#include "base/text.h"

namespace mico {
namespace {

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

// The picker is drawn with cursor positioning rather than spaces, so it only
// reads as text once the emulator has placed it. That is why this parses the
// grid and not the byte stream.
std::string picker_row_text(const VtRow& r) {
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

std::vector<SlashCommand> ClaudeAdapter::builtin_commands() const { return copy(kClaudeInteractive); }

// Everything but the interactive set comes from asking claude: its answer to
// an SDK "initialize" request lists the commands (skills, plugins, the user's
// own) and the models.
std::vector<std::string> ClaudeAdapter::command_probe_argv() const {
  // No transcript: this process never has a conversation to keep.
  return {"claude", "-p", "--no-session-persistence", "--input-format", "stream-json",
          "--output-format", "stream-json", "--verbose"};
}

std::string ClaudeAdapter::command_probe_request() const {
  return "{\"type\":\"control_request\",\"request_id\":\"mico-commands\",\"request\":{\"subtype\":\"initialize\"}}\n";
}

bool ClaudeAdapter::read_command_probe(std::string_view output, CommandProbeAnswer& out) const {
  // The answer is one line; everything before it (hook notices) is skipped.
  for (size_t at = output.find("\"control_response\""); at != std::string_view::npos;
       at = output.find("\"control_response\"", at + 1)) {
    const size_t nl = output.rfind('\n', at);
    const size_t start = nl == std::string_view::npos ? 0 : nl + 1;
    const size_t end = output.find('\n', at);
    if (end == std::string_view::npos) return false;  // not all here yet
    const std::string_view answer = output.substr(start, end - start);
    std::vector<SlashCommand> got = parse_claude_commands(answer);
    if (got.empty()) continue;
    out.commands = std::move(got);
    out.models = parse_claude_models(answer, &out.efforts);
    return true;
  }
  return false;
}

std::string ClaudeAdapter::model_picker_command() const { return "/model\r"; }

std::vector<ModelOption> ClaudeAdapter::read_model_picker(const Vt& vt) const { return parse_model_picker(vt); }

ChipControl ClaudeAdapter::chip_control(std::string_view key) const {
  ChipControl c;
  if (key == "model") {
    // The model list is whatever claude itself said — never a list written
    // down here, which would go stale the day a model ships. Empty until a
    // probe lands, and then the menu offers claude's picker instead.
    c.source = ChipControl::Source::Models;
    c.set_prefix = "/model ";
    c.picker = "/model";
  } else if (key == "effort") {
    // The levels claude's own initialize answer reported, when it has.
    c.source = ChipControl::Source::Efforts;
    c.values = {"low", "medium", "high"};
    c.set_prefix = "/effort ";
  } else if (key == "mode" || key == "perm") {
    // Claude has no one-shot command for the permission mode; Shift+Tab walks a
    // fixed ring. Knowing the current state, each entry sends exactly enough
    // presses to land on it — and "plan" is reachable directly.
    c.ring = {{"default", "default"}, {"acceptEdits", "accept edits"}, {"plan", "plan mode"}};
    c.ring_key = "\x1b[Z";
  } else {
    return Adapter::chip_control(key);
  }
  return c;
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

std::vector<ModelOption> parse_model_picker(const Vt& vt) {
  std::vector<ModelOption> out;
  bool in_picker = false;
  for (int i = 0; i < vt.total_rows(); i++) {
    const std::string line = picker_row_text(vt.row(i));
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

}  // namespace mico
