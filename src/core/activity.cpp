#include "core/activity.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstring>

#include "base/json.h"
#include "base/jsonl.h"

namespace mico {

const char* tool_kind_name(ToolKind k) {
  switch (k) {
    case ToolKind::Build: return "build";
    case ToolKind::Test: return "test";
    case ToolKind::Run: return "run";
    case ToolKind::Git: return "git";
    case ToolKind::Install: return "install";
    case ToolKind::Network: return "network";
    case ToolKind::Read: return "read & search";
    case ToolKind::Edit: return "edit files";
    case ToolKind::Agent: return "subagents";
    case ToolKind::Wait: return "waiting for you";
    case ToolKind::Other: return "other";
  }
  return "other";
}

namespace {

std::string lower(std::string_view s) {
  std::string out(s);
  for (char& c : out)
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  return out;
}

std::string_view basename(std::string_view p) {
  const size_t slash = p.rfind('/');
  return slash == std::string_view::npos ? p : p.substr(slash + 1);
}

bool one_of(std::string_view s, std::initializer_list<std::string_view> set) {
  return std::find(set.begin(), set.end(), s) != set.end();
}

bool is_assignment(std::string_view w) {
  const size_t eq = w.find('=');
  if (eq == std::string_view::npos || eq == 0) return false;
  for (size_t i = 0; i < eq; i++) {
    const char c = w[i];
    if (!(c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (i > 0 && c >= '0' && c <= '9')))
      return false;
  }
  return true;
}

// One step of a shell command line: its words, and where it starts.
struct Step {
  size_t at = 0;
  std::vector<std::string> words;
};

// Splits a command line into steps at the top-level separators (&&, ||, ;,
// |, &, newline) and each step into words, the way a shell would group them:
// quotes and $(...) hold together, and a heredoc's text is skipped, not read
// as more commands. Quotes are removed from the words.
std::vector<Step> lex(std::string_view cmd) {
  std::vector<Step> steps;
  Step cur;
  std::string word;
  bool in_word = false;
  std::string heredoc;  // the delimiter awaited, once the line ends
  const auto end_word = [&] {
    if (in_word) cur.words.push_back(std::move(word));
    word.clear();
    in_word = false;
  };
  const auto end_step = [&](size_t next) {
    end_word();
    // A step starts at its first word, not at the blank after a separator.
    while (cur.at < cmd.size() && (cmd[cur.at] == ' ' || cmd[cur.at] == '\t')) cur.at++;
    if (!cur.words.empty()) steps.push_back(std::move(cur));
    cur = Step{};
    cur.at = next;
  };
  size_t i = 0;
  while (i < cmd.size()) {
    const char c = cmd[i];
    if (c == '\\' && i + 1 < cmd.size()) {
      if (cmd[i + 1] != '\n') { word += cmd[i + 1]; in_word = true; }
      i += 2;
      continue;
    }
    if (c == '\'' || c == '"') {
      const size_t close = cmd.find(c, i + 1);
      const size_t e = close == std::string_view::npos ? cmd.size() : close;
      word.append(cmd.substr(i + 1, e - i - 1));
      in_word = true;
      i = e + 1;
      continue;
    }
    if (c == '$' && i + 1 < cmd.size() && (cmd[i + 1] == '(' || cmd[i + 1] == '{')) {
      // $(...) or ${...}, nesting counted: one word however much is inside.
      const char open = cmd[i + 1], close = open == '(' ? ')' : '}';
      int depth = 0;
      size_t j = i + 1;
      for (; j < cmd.size(); j++) {
        if (cmd[j] == open) depth++;
        else if (cmd[j] == close && --depth == 0) break;
      }
      word.append(cmd.substr(i, std::min(cmd.size(), j + 1) - i));
      in_word = true;
      i = j + 1;
      continue;
    }
    if (c == '<' && cmd.compare(i, 2, "<<") == 0 && cmd.compare(i, 3, "<<<") != 0) {
      // A heredoc: note its delimiter; its text starts on the next line.
      size_t j = i + 2;
      if (j < cmd.size() && cmd[j] == '-') j++;
      while (j < cmd.size() && cmd[j] == ' ') j++;
      std::string delim;
      while (j < cmd.size() && cmd[j] != ' ' && cmd[j] != '\n' && cmd[j] != ';' && cmd[j] != '&' && cmd[j] != '|' && cmd[j] != ')') {
        if (cmd[j] != '\'' && cmd[j] != '"') delim += cmd[j];
        j++;
      }
      end_word();
      cur.words.push_back("<<");
      heredoc = delim;
      i = j;
      continue;
    }
    if (c == '\n') {
      end_step(i + 1);
      i++;
      if (!heredoc.empty()) {
        // Skip to the line that closes the heredoc.
        while (i < cmd.size()) {
          size_t e = cmd.find('\n', i);
          if (e == std::string_view::npos) e = cmd.size();
          std::string_view line = cmd.substr(i, e - i);
          while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
          i = e + 1;
          if (line == heredoc) break;
        }
        heredoc.clear();
        cur.at = std::min(i, cmd.size());
      }
      continue;
    }
    if (c == ';' || c == '|' || c == '&') {
      size_t n = 1;
      if (i + 1 < cmd.size() && (cmd[i + 1] == c || (c == '|' && cmd[i + 1] == '&'))) n = 2;
      // "2>&1" and ">&2" are redirections, not separators.
      if (c == '&' && n == 1 && i > 0 && (cmd[i - 1] == '>' || cmd[i - 1] == '<')) {
        word += c;
        in_word = true;
        i++;
        continue;
      }
      end_step(i + n);
      i += n;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '(' || c == ')') {
      end_word();
      i++;
      continue;
    }
    word += c;
    in_word = true;
    i++;
  }
  end_step(cmd.size());
  return steps;
}

// A step with its wrappers taken off: assignments (S=/tmp/x), sudo, time,
// timeout 60, a loop's do/then. Empty when nothing is left.
std::vector<std::string_view> core_words(const Step& st) {
  std::vector<std::string_view> w(st.words.begin(), st.words.end());
  size_t i = 0;
  while (i < w.size()) {
    const std::string_view x = w[i];
    if (is_assignment(x)) { i++; continue; }
    if (one_of(x, {"sudo", "time", "nice", "env", "exec", "command", "nohup", "stdbuf", "do", "then",
                   "else", "{", "!", "xargs"})) { i++; continue; }
    if (x == "timeout") { i += 2; continue; }  // timeout N cmd
    if (x == "uv" && i + 1 < w.size() && w[i + 1] == "run") { i += 2; continue; }
    break;
  }
  w.erase(w.begin(), w.begin() + long(std::min(i, w.size())));
  return w;
}

bool mentions_test(std::string_view w) {
  const std::string l = lower(w);
  return l.find("test") != std::string::npos || l.find("spec") != std::string::npos;
}

// One step, by its program: what kind of work, and the group it names.
ToolKind classify_words(const std::vector<std::string_view>& w, std::string* group) {
  const std::string prog = lower(basename(w[0]));
  // The first word after the program that is not a flag: a subcommand, a
  // script, a make target.
  std::string_view sub;
  for (size_t i = 1; i < w.size(); i++)
    if (w[i].front() != '-') { sub = w[i]; break; }
  const bool has_i = std::any_of(w.begin() + 1, w.end(), [](std::string_view x) {
    return x == "-i" || x.starts_with("-i.") || x == "--in-place";
  });

  std::string g = prog;
  ToolKind k = ToolKind::Run;
  const bool interp = one_of(prog, {"python", "python3", "node", "bash", "sh", "zsh", "ruby", "perl",
                                    "deno", "bun", "julia", "rscript", "lua"}) ||
                      prog.starts_with("python3.");
  if (interp) {
    if (w.size() > 1 && (w[1] == "-" || w[1] == "-c" || w[1] == "<<" || w[1] == "-e")) {
      g = prog + " (inline)";
      if (prog == "perl" && has_i) k = ToolKind::Edit;
    } else if (w.size() > 2 && w[1] == "-m") {
      g = prog + " -m " + std::string(w[2]);
      if (one_of(w[2], {"pytest", "unittest"})) k = ToolKind::Test;
      else if (w[2] == "pip") k = ToolKind::Install;
      else if (one_of(w[2], {"build", "compileall"})) k = ToolKind::Build;
    } else if (!sub.empty()) {
      g = prog + " " + std::string(basename(sub));
      if (mentions_test(sub)) k = ToolKind::Test;
    }
    if (group) *group = g;
    return k;
  }

  const std::string s = lower(sub);
  if (one_of(prog, {"git", "gh"})) {
    g = prog + (s.empty() ? "" : " " + s);
    k = ToolKind::Git;
  } else if (one_of(prog, {"make", "ninja", "gcc", "g++", "cc", "c++", "clang", "clang++", "rustc",
                           "tsc", "javac", "mvn", "gradle", "meson", "scons", "nvcc", "zig"})) {
    k = s.find("test") != std::string::npos || s == "check" ? ToolKind::Test : ToolKind::Build;
    if (one_of(prog, {"make", "mvn", "gradle", "meson"}) && !s.empty()) g = prog + " " + s;
  } else if (prog == "cmake") {
    // "cmake --build" builds; "cmake -S . -B build" configures, also a build step.
    const bool build = std::any_of(w.begin(), w.end(), [](std::string_view x) { return x == "--build"; });
    g = build ? "cmake --build" : "cmake";
    k = ToolKind::Build;
  } else if (one_of(prog, {"ctest", "pytest", "jest", "vitest", "mocha", "tox", "nox", "phpunit", "rspec"})) {
    k = ToolKind::Test;
  } else if (one_of(prog, {"cargo", "go", "npm", "pnpm", "yarn", "bazel", "dotnet", "swift", "mix", "stack", "uv", "pip", "pip3",
                           "apt", "apt-get", "brew", "conda", "mamba", "poetry", "docker", "podman"})) {
    std::string sub2 = s;
    if (one_of(prog, {"npm", "pnpm", "yarn"}) && s == "run") {
      for (size_t i = 1; i + 1 < w.size(); i++)
        if (w[i] == "run") { sub2 = "run " + lower(w[i + 1]); break; }
    }
    g = prog + (sub2.empty() ? "" : " " + sub2);
    if (sub2.find("test") != std::string::npos) k = ToolKind::Test;
    else if (one_of(s, {"install", "add", "i", "ci", "sync", "update", "upgrade"})) k = ToolKind::Install;
    else if (sub2.find("build") != std::string::npos || one_of(s, {"check", "clippy", "compile", "vet"}))
      k = ToolKind::Build;
    else if (one_of(prog, {"apt", "apt-get", "brew", "conda", "mamba", "pip", "pip3"})) k = ToolKind::Install;
  } else if (one_of(prog, {"curl", "wget", "http", "https", "ssh", "scp", "rsync", "ping", "nc"})) {
    k = ToolKind::Network;
  } else if (prog == "sed" || prog == "awk") {
    k = has_i ? ToolKind::Edit : ToolKind::Read;
  } else if (one_of(prog, {"cat", "head", "tail", "less", "more", "grep", "rg", "ag", "egrep", "fgrep", "find", "fd",
                           "ls", "tree", "wc", "stat", "file", "du", "df", "diff", "jq", "nl", "which", "pwd",
                           "readlink", "realpath", "xxd", "od", "strings", "sort", "uniq", "cut", "tr",
                           "column", "basename", "dirname", "date", "whoami", "uname", "ps", "env", "printenv",
                           "type", "locate", "md5sum", "sha256sum", "cmp", "comm"})) {
    k = ToolKind::Read;
  } else if (one_of(prog, {"mv", "cp", "rm", "mkdir", "touch", "ln", "chmod", "chown", "tee", "patch",
                           "rmdir", "install", "truncate", "unzip", "tar", "gzip", "zip"})) {
    k = ToolKind::Edit;
  } else if (one_of(prog, {"echo", "printf", "true", "false", "sleep", "exit", "kill", "pkill", "wait", "clear"})) {
    k = ToolKind::Other;
  } else {
    // Something that runs: a built binary, a script, a program. Its first
    // flag says more than the path it lives at: "mico --selftest".
    if (w.size() > 1 && w[1].starts_with("--")) g = prog + " " + std::string(w[1]);
    k = mentions_test(w[0]) || (w.size() > 1 && mentions_test(w[1])) ? ToolKind::Test : ToolKind::Run;
  }
  if (group) *group = g;
  return k;
}

// Housekeeping around the real work: never what a command line is for when
// anything else is in it.
bool setup_step(std::string_view prog) {
  return one_of(prog, {"cd", "pushd", "popd", "export", "set", "source", ".", "true", "unset", "local",
                       "pkill", "kill", "killall", "sleep", "echo", "printf", "clear", "mkdir", "ulimit",
                       "trap", "wait", "rm", "for", "while", "until", "if", "case", "done", "fi", "esac",
                       "}", "read", "exit", "shift", "test", "["});
}

// Which kind wins when a command line does several things: the heaviest.
int weight(ToolKind k) {
  switch (k) {
    case ToolKind::Build: return 9;
    case ToolKind::Test: return 8;
    case ToolKind::Install: return 7;
    case ToolKind::Run: return 6;
    case ToolKind::Network: return 5;
    case ToolKind::Git: return 4;
    case ToolKind::Edit: return 3;
    case ToolKind::Read: return 2;
    default: return 1;
  }
}

// A shell command line: every step is classified and the heaviest names it.
// `cp a.bak; sed -i ...; cmake --build build && ./mico --selftest` is a build.
ToolKind classify_shell(std::string_view cmd, std::string* group, size_t* shown_from) {
  const std::vector<Step> steps = lex(cmd);
  int best = -1;
  ToolKind kind = ToolKind::Other;
  std::string g;
  size_t from = 0;
  for (const Step& st : steps) {
    const std::vector<std::string_view> w = core_words(st);
    if (w.empty() || setup_step(basename(w[0]))) continue;
    std::string sg;
    const ToolKind k = classify_words(w, &sg);
    if (weight(k) > best) {
      best = weight(k);
      kind = k;
      g = std::move(sg);
      from = st.at;
    }
  }
  if (best < 0) {
    // Nothing but housekeeping: named by its first step.
    for (const Step& st : steps) {
      const std::vector<std::string_view> w = core_words(st);
      if (w.empty()) continue;
      g = std::string(basename(w[0]));
      from = st.at;
      break;
    }
    if (g.empty()) g = "shell";
  }
  if (group) *group = g;
  if (shown_from) *shown_from = from;
  return kind;
}

}  // namespace

ToolKind classify_tool(std::string_view tool, std::string_view command, std::string* group,
                       size_t* shown_from) {
  if (shown_from) *shown_from = 0;
  const std::string t = lower(tool);
  const auto named = [&](ToolKind k) {
    if (group) *group = std::string(tool);
    return k;
  };
  if (one_of(t, {"edit", "write", "multiedit", "notebookedit", "apply_patch", "filechange", "edit_file",
                 "write_file", "str_replace_based_edit_tool", "create_file"}))
    return named(ToolKind::Edit);
  if (one_of(t, {"read", "grep", "glob", "ls", "view", "read_file", "list_files", "list_dir", "search",
                 "find", "view_image", "imageview", "toolsearch"}))
    return named(ToolKind::Read);
  if (one_of(t, {"webfetch", "websearch", "web_search", "fetch", "web_fetch"})) return named(ToolKind::Network);
  if (one_of(t, {"task", "agent", "spawn_agent", "subagent", "hub"})) return named(ToolKind::Agent);
  if (one_of(t, {"askuserquestion", "request_user_input", "exitplanmode", "ask"})) return named(ToolKind::Wait);
  if (one_of(t, {"bash", "shell", "exec_command", "local_shell", "container.exec", "commandexecution",
                 "unified_exec", "run_terminal_cmd", "terminal", "exec"}) && !command.empty())
    return classify_shell(command, group, shown_from);
  return named(ToolKind::Other);
}

namespace {

// "2026-09-17T18:11:23.637Z" -> unix milliseconds. 0 when it is not one.
int64_t parse_time(std::string_view s) {
  if (s.size() < 19 || s[4] != '-' || s[10] != 'T') return 0;
  const auto num = [&](size_t at, size_t n) {
    int v = 0;
    for (size_t i = at; i < at + n; i++) {
      if (s[i] < '0' || s[i] > '9') return -1;
      v = v * 10 + (s[i] - '0');
    }
    return v;
  };
  const int Y = num(0, 4), M = num(5, 2), D = num(8, 2), h = num(11, 2), m = num(14, 2), sec = num(17, 2);
  if (Y < 0 || M < 1 || D < 1 || h < 0 || m < 0 || sec < 0) return 0;
  // Days from the civil date (Howard Hinnant's algorithm).
  const int y = Y - (M <= 2);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = unsigned(y - era * 400);
  const unsigned doy = unsigned((153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1);
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const int64_t days = int64_t(era) * 146097 + int64_t(doe) - 719468;
  int64_t ms = ((days * 24 + h) * 60 + m) * 60000 + int64_t(sec) * 1000;
  if (s.size() > 20 && s[19] == '.') {
    int frac = 0, digits = 0;
    for (size_t i = 20; i < s.size() && s[i] >= '0' && s[i] <= '9' && digits < 3; i++, digits++)
      frac = frac * 10 + (s[i] - '0');
    while (digits++ < 3) frac *= 10;
    ms += frac;
  }
  return ms;
}

std::string one_line(std::string_view s, size_t max = 300) {
  std::string out;
  bool space = false;
  for (size_t i = 0; i < s.size() && out.size() < max; i++) {
    const char c = s[i];
    if (c == '\n' || c == '\t' || c == '\r' || c == ' ') {
      if (!out.empty()) space = true;
      continue;
    }
    if (space) out += ' ', space = false;
    out += c;
  }
  // Never end inside a character.
  while (!out.empty() && (uint8_t(out.back()) & 0xC0) == 0x80) out.pop_back();
  if (!out.empty() && (uint8_t(out.back()) & 0x80)) out.pop_back();
  return out;
}

std::string text_of(const js::Value& v) {
  std::string s;
  if (v.is_string()) js::unescape_append(v.body(), s);
  return s;
}

// What a call works on, from its arguments: the command line, else a file,
// a pattern or a URL.
std::string subject(const js::Value& input) {
  std::string cmd, other;
  if (input.is_object()) {
    js::scan_object(input.raw, [&](std::string_view k, const js::Value& v) {
      if ((k == "command" || k == "cmd") && v.is_string()) cmd = text_of(v);
      else if ((k == "command" || k == "cmd") && v.is_array()) {
        // ["bash", "-lc", "make"]: the script is what ran.
        std::vector<std::string> parts;
        js::scan_array(v.raw, [&](const js::Value& p) { parts.push_back(text_of(p)); return true; });
        if (parts.size() >= 3 && (parts[1] == "-lc" || parts[1] == "-c")) cmd = parts.back();
        else for (const auto& p : parts) cmd += (cmd.empty() ? "" : " ") + p;
      } else if (other.empty() && (k == "file_path" || k == "path" || k == "notebook_path" || k == "pattern" ||
                                   k == "url" || k == "query" || k == "description")) {
        other = text_of(v);
      }
      return true;
    });
  }
  std::string& s = cmd.empty() ? other : cmd;
  if (s.size() > 4096) s.resize(4096);
  return s;
}

struct Reader {
  ChatActivity& out;
  ActivityIndex::Resume& r;

  void call(std::string id, int64_t at, uint64_t offset, std::string tool, std::string command) {
    r.pending[std::move(id)] = ActivityIndex::Pending{at, offset, std::move(tool), std::move(command)};
  }
  // A result: the call it answers becomes a run. `exact_ms` is the agent's own
  // measure when it gives one.
  void result(const std::string& id, int64_t at, bool failed, int64_t exact_ms = -1) {
    if (auto pe = r.pending_edits.find(id); pe != r.pending_edits.end()) {
      if (!failed)
        for (FileEdit& e : pe->second) out.edits.push_back(std::move(e));
      r.pending_edits.erase(pe);
    }
    auto it = r.pending.find(id);
    if (it == r.pending.end()) return;
    add(it->second.start_ms, exact_ms >= 0 ? exact_ms : std::max<int64_t>(0, at - it->second.start_ms),
        it->second.offset, std::move(it->second.tool), std::move(it->second.command), failed);
    r.pending.erase(it);
  }
  void add(int64_t start, int64_t dur, uint64_t offset, std::string tool, std::string command, bool failed) {
    ToolRun run;
    run.start_ms = start;
    run.dur_ms = dur;
    run.offset = offset;
    // Classified on the command as written, line breaks and all; shown on one.
    size_t from = 0;
    run.kind = classify_tool(tool, command, &run.group, &from);
    run.failed = failed;
    // A file tool takes milliseconds. One that took many seconds sat on a
    // permission prompt: that time was spent waiting for the user.
    run.waited = dur > 15000 && (run.kind == ToolKind::Read || run.kind == ToolKind::Edit) &&
                 command.find('\n') == std::string::npos && !one_of(lower(tool), {"bash", "exec", "shell"});
    run.tool = std::move(tool);
    // Shown from the step that said what it is for, not from `S=/tmp/x; cd y &&`.
    run.command = one_line(std::string_view(command).substr(std::min(from, command.size())));
    out.runs.push_back(std::move(run));
  }

  // The file changes a line records. A result's are known to have gone
  // through; a call's wait for its result.
  void edits(std::string_view agent, std::string_view raw, uint64_t offset) {
    if (!may_have_changes(agent, raw)) return;
    std::vector<LineChanges> groups = read_changes(agent, raw, out.cwd, false);
    if (groups.empty()) return;
    int64_t at = 0;
    js::scan_object(raw, [&](std::string_view k, const js::Value& v) {
      if (k == "timestamp" && v.is_string()) {
        at = parse_time(v.body());
        return false;
      }
      return true;
    });
    for (LineChanges& g : groups) {
      if (g.failed) continue;
      uint64_t call_at = offset;
      std::string call_file;
      // Codex writes a patch twice, as the call and as the change it made.
      if (!g.result_of.empty()) r.pending_edits.erase(g.result_of);
      if (!g.result_of.empty())
        if (auto it = r.pending.find(g.result_of); it != r.pending.end()) {
          call_at = it->second.offset;
          call_file = it->second.command;
          if (!at) at = it->second.start_ms;
        }
      std::vector<FileEdit> v;
      for (FileChange& c : g.changes) {
        FileEdit e;
        e.at_ms = at;
        e.offset = offset;
        e.call_offset = call_at;
        e.file = c.file.empty() ? resolve_path(out.cwd, call_file) : std::move(c.file);
        e.moved_to = std::move(c.moved_to);
        e.op = c.op;
        e.added = c.added;
        e.removed = c.removed;
        if (!e.file.empty() && e.file != "/") v.push_back(std::move(e));
      }
      if (!g.call_id.empty()) r.pending_edits[g.call_id] = std::move(v);
      else
        for (FileEdit& e : v) out.edits.push_back(std::move(e));
    }
  }

  // claude: tool_use blocks in assistant messages, tool_result blocks in
  // user messages, each record stamped.
  void claude(std::string_view raw, uint64_t offset) {
    if (raw.find("\"tool_use") == std::string_view::npos && raw.find("\"tool_result\"") == std::string_view::npos)
      return;
    int64_t at = 0;
    js::Value message{};
    js::scan_object(raw, [&](std::string_view k, const js::Value& v) {
      if (k == "timestamp") at = parse_time(v.body());
      else if (k == "message") message = v;
      return true;
    });
    if (!message.is_object()) return;
    js::Value content{};
    js::scan_object(message.raw, [&](std::string_view k, const js::Value& v) {
      if (k == "content") { content = v; return false; }
      return true;
    });
    if (!content.is_array()) return;
    js::scan_array(content.raw, [&](const js::Value& b) {
      if (!b.is_object()) return true;
      std::string_view type;
      std::string id, name, use_id;
      js::Value input{};
      bool error = false;
      js::scan_object(b.raw, [&](std::string_view k, const js::Value& v) {
        if (k == "type") type = v.body();
        else if (k == "id") id = text_of(v);
        else if (k == "name") name = text_of(v);
        else if (k == "input") input = v;
        else if (k == "tool_use_id") use_id = text_of(v);
        else if (k == "is_error") error = v.is_true();
        return true;
      });
      if (type == "tool_use" && !id.empty()) call(id, at, offset, name, subject(input));
      else if (type == "tool_result" && !use_id.empty()) result(use_id, at, error);
      return true;
    });
  }

  // codex: newer rollouts record each command with its duration and exit
  // code; older ones a function call, and an output headed "Exit code: N /
  // Wall time: X seconds".
  void codex(std::string_view raw, uint64_t offset) {
    const bool item = raw.find("\"item_completed\"") != std::string_view::npos;
    if (!item && raw.find("function_call") == std::string_view::npos &&
        raw.find("custom_tool_call") == std::string_view::npos)
      return;
    int64_t at = 0;
    js::Value payload{};
    js::scan_object(raw, [&](std::string_view k, const js::Value& v) {
      if (k == "timestamp") at = parse_time(v.body());
      else if (k == "payload") payload = v;
      return true;
    });
    if (!payload.is_object()) return;
    std::string_view ptype;
    std::string name, call_id, output, input_text;
    js::Value it{}, args{};
    js::scan_object(payload.raw, [&](std::string_view k, const js::Value& v) {
      if (k == "type") ptype = v.body();
      else if (k == "item") it = v;
      else if (k == "name") name = text_of(v);
      else if (k == "call_id") call_id = text_of(v);
      else if (k == "arguments") args = v;
      else if (k == "output") output = v.is_string() ? text_of(v) : std::string(v.raw);
      else if (k == "input") input_text = text_of(v);
      return true;
    });

    if (ptype == "item_completed" && it.is_object()) {
      std::string_view itype;
      js::Value command{}, duration{}, parsed{}, changes{};
      int exit_code = 0;
      std::string status, server, tool;
      js::scan_object(it.raw, [&](std::string_view k, const js::Value& v) {
        if (k == "type") itype = v.body();
        else if (k == "command") command = v;
        else if (k == "duration") duration = v;
        else if (k == "exit_code" && v.type == js::Type::Number) exit_code = std::atoi(std::string(v.raw).c_str());
        else if (k == "status") status = text_of(v);
        else if (k == "parsed_cmd") parsed = v;
        else if (k == "changes") changes = v;
        else if (k == "server") server = text_of(v);
        else if (k == "tool") tool = text_of(v);
        return true;
      });
      int64_t dur = -1;
      if (duration.is_object()) {
        int64_t secs = 0, nanos = 0;
        js::scan_object(duration.raw, [&](std::string_view k, const js::Value& v) {
          if (k == "secs") secs = std::atoll(std::string(v.raw).c_str());
          else if (k == "nanos") nanos = std::atoll(std::string(v.raw).c_str());
          return true;
        });
        dur = secs * 1000 + nanos / 1000000;
      }
      const int64_t start = dur > 0 ? at - dur : at;
      if (itype == "CommandExecution") {
        std::string cmd;
        if (command.is_array()) {
          std::string obj = "{\"command\":" + std::string(command.raw) + "}";
          cmd = subject(js::Value{obj, js::Type::Object});
        } else {
          cmd = text_of(command);
        }
        // codex's own reading of the command: read / search / list_files.
        std::string hint;
        js::scan_array(parsed.raw, [&](const js::Value& p) {
          js::scan_object(p.raw, [&](std::string_view k, const js::Value& v) {
            if (k == "type") hint = text_of(v);
            return true;
          });
          return false;
        });
        add(start, std::max<int64_t>(0, dur), offset, "exec", cmd, exit_code != 0 || status == "failed");
        if (one_of(hint, {"read", "search", "list_files"})) out.runs.back().kind = ToolKind::Read;
      } else if (itype == "McpToolCall") {
        add(start, std::max<int64_t>(0, dur), offset, server + "." + tool, "", status == "failed");
      } else if (itype == "FileChange") {
        std::string files;
        int n = 0;
        js::scan_object(changes.raw, [&](std::string_view k, const js::Value&) {
          if (n++ < 3) files += (files.empty() ? "" : ", ") + std::string(basename(k));
          return true;
        });
        if (n > 3) files += ", +" + std::to_string(n - 3) + " more";
        add(at, 0, offset, "FileChange", files, status == "failed");
      }
      return;
    }
    if (ptype == "function_call" && !call_id.empty()) {
      // Arguments arrive as a JSON string.
      std::string a = text_of(args);
      std::string cmd = a.empty() ? std::string() : subject(js::Value{a, js::Type::Object});
      if (name == "request_user_input_async") return;  // the agent does not wait on it
      call(call_id, at, offset, name, cmd);
    } else if (ptype == "custom_tool_call" && !call_id.empty() && name == "apply_patch") {
      std::string files;
      for (size_t p = input_text.find("*** "); p != std::string::npos; p = input_text.find("*** ", p + 4)) {
        for (std::string_view tag : {"*** Update File: ", "*** Add File: ", "*** Delete File: "})
          if (input_text.compare(p, tag.size(), tag) == 0) {
            const size_t e = input_text.find('\n', p);
            const std::string f(basename(std::string_view(input_text).substr(p + tag.size(), e - p - tag.size())));
            if (files.find(f) == std::string::npos) files += (files.empty() ? "" : ", ") + f;
          }
      }
      call(call_id, at, offset, "apply_patch", one_line(files));
    } else if ((ptype == "function_call_output" || ptype == "custom_tool_call_output") && !call_id.empty()) {
      // "Exit code: 1\nWall time: 2.5 seconds", or JSON with a metadata block.
      int64_t exact = -1;
      bool failed = false;
      if (const size_t e = output.find("Exit code: "); e != std::string::npos && e < 64)
        failed = std::atoi(output.c_str() + e + 11) != 0;
      if (const size_t w = output.find("Wall time: "); w != std::string::npos && w < 128)
        exact = int64_t(std::strtod(output.c_str() + w + 11, nullptr) * 1000);
      if (const size_t m = output.find("\"exit_code\":"); m != std::string::npos && m < 512)
        failed = std::atoi(output.c_str() + m + 12) != 0;
      if (const size_t d = output.find("\"duration_seconds\":"); d != std::string::npos && d < 512)
        exact = int64_t(std::strtod(output.c_str() + d + 19, nullptr) * 1000);
      // A patch that did not apply says so in words, with no exit code.
      if (output.find("verification failed") < 256 || output.starts_with("Failed to"))
        failed = true;
      result(call_id, at, failed, exact);
    }
  }

  // pi and omp: toolCall blocks in assistant messages, and toolResult
  // messages that name the call.
  void pi(std::string_view raw, uint64_t offset) {
    if (raw.find("\"toolCall\"") == std::string_view::npos && raw.find("\"toolResult\"") == std::string_view::npos)
      return;
    int64_t at = 0;
    js::Value message{};
    js::scan_object(raw, [&](std::string_view k, const js::Value& v) {
      if (k == "timestamp") at = parse_time(v.body());
      else if (k == "message") message = v;
      return true;
    });
    if (!message.is_object()) return;
    std::string role, call_id;
    bool error = false;
    js::Value content{};
    js::scan_object(message.raw, [&](std::string_view k, const js::Value& v) {
      if (k == "role") role = text_of(v);
      else if (k == "toolCallId") call_id = text_of(v);
      else if (k == "isError") error = v.is_true();
      else if (k == "content") content = v;
      return true;
    });
    if (role == "toolResult") {
      result(call_id, at, error);
      return;
    }
    if (!content.is_array()) return;
    js::scan_array(content.raw, [&](const js::Value& b) {
      std::string_view type;
      std::string id, name;
      js::Value arguments{};
      js::scan_object(b.raw, [&](std::string_view k, const js::Value& v) {
        if (k == "type") type = v.body();
        else if (k == "id") id = text_of(v);
        else if (k == "name") name = text_of(v);
        else if (k == "arguments") arguments = v;
        return true;
      });
      if (type == "toolCall" && !id.empty()) call(id, at, offset, name, subject(arguments));
      return true;
    });
  }
};

// Reads the complete lines from r.offset on.
void read_from(Jsonl& j, const std::string& agent, ChatActivity& out, ActivityIndex::Resume& r) {
  while (!j.complete() && j.indexed_from() > r.offset) j.extend_back();
  const size_t n = j.line_count();
  size_t i = r.offset ? j.line_at_byte(r.offset) : 0;
  while (i < n && j.line_offset(i) < r.offset) i++;
  Reader rd{out, r};
  constexpr size_t kChunk = 4096;
  for (; i < n; i++) {
    if (i % kChunk == 0) j.will_read(i, std::min(n, i + kChunk));
    const std::string_view raw = j.line(i);
    if (j.line_offset(i) + raw.size() >= j.line_offset(i + 1)) break;  // still being written
    const uint64_t at = j.line_offset(i);
    r.offset = j.line_offset(i + 1);
    // Before the calls: a result's changes look up the call it answers.
    rd.edits(agent, raw, at);
    if (agent == "claude") rd.claude(raw, at);
    else if (agent == "codex") rd.codex(raw, at);
    else if (agent == "pi" || agent == "omp") rd.pi(raw, at);
  }
}

}  // namespace

ChatActivity ActivityIndex::read_file(const std::string& path, const std::string& agent, const std::string& cwd) {
  ChatActivity a;
  a.path = path;
  a.agent = agent;
  a.cwd = cwd;
  Resume r;
  Jsonl j;
  if (j.open(path)) read_from(j, agent, a, r);
  return a;
}

void ActivityIndex::start(const std::vector<Project>& projects, const Store& store) {
  jobs_.clear();
  chats_.clear();
  for (const auto& p : projects)
    for (const auto& s : p.sessions) {
      const std::string* name = store.custom_name(s.agent, s.id);
      std::string title = name ? *name : s.title;
      auto it = cache_.find(s.path);
      if (it != cache_.end() && it->second.mtime == s.mtime && it->second.size == s.bytes) {
        it->second.data.title = title;
        it->second.data.project = p.name;
        chats_.push_back(&it->second.data);
        continue;
      }
      jobs_.push_back(Job{s, p.name, std::move(title)});
    }
}

bool ActivityIndex::step(int budget_ms) {
  const auto t0 = std::chrono::steady_clock::now();
  while (!jobs_.empty()) {
    Job job = std::move(jobs_.back());
    jobs_.pop_back();
    Cached& c = cache_[job.s.path];
    // A transcript only grows; one that shrank was rewritten.
    if (job.s.bytes < c.size) c = Cached{};
    c.mtime = job.s.mtime;
    c.size = job.s.bytes;
    c.data.path = job.s.path;
    c.data.agent = job.s.agent;
    c.data.id = job.s.id;
    c.data.title = job.title;
    c.data.project = job.project;
    c.data.cwd = job.s.cwd.empty() ? "/" : job.s.cwd;
    Jsonl j;
    if (j.open(job.s.path)) read_from(j, job.s.agent, c.data, c.resume);
    chats_.push_back(&c.data);
    if (std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(budget_ms)) break;
  }
  return jobs_.empty();
}

}  // namespace mico
