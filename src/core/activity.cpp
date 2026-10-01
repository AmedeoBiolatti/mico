#include "core/activity.h"

#include "adapters/adapters.h"
#include "adapters/tool_calls.h"
#include "base/line_reader.h"
#include "base/time.h"

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

struct Reader final : ToolSink {
  ChatActivity& out;
  ActivityIndex::Resume& r;
  Reader(ChatActivity& o, ActivityIndex::Resume& rs) : out(o), r(rs) {}

  void call(std::string id, int64_t at, uint64_t offset, std::string tool, std::string command) override {
    r.pending[std::move(id)] = ActivityIndex::Pending{at, offset, std::move(tool), std::move(command)};
  }
  // A result: the call it answers becomes a run. `exact_ms` is the agent's own
  // measure when it gives one.
  void result(const std::string& id, int64_t at, bool failed, int64_t exact_ms) override {
    if (auto pe = r.pending_edits.find(id); pe != r.pending_edits.end()) {
      if (!failed)
        for (FileEdit& e : pe->second) out.edits.push_back(std::move(e));
      r.pending_edits.erase(pe);
    }
    auto it = r.pending.find(id);
    if (it == r.pending.end()) return;
    run(it->second.start_ms, exact_ms >= 0 ? exact_ms : std::max<int64_t>(0, at - it->second.start_ms),
        it->second.offset, std::move(it->second.tool), std::move(it->second.command), failed, false);
    r.pending.erase(it);
  }
  void run(int64_t start, int64_t dur, uint64_t offset, std::string tool, std::string command, bool failed,
           bool reading) override {
    ToolRun tr;
    tr.start_ms = start;
    tr.dur_ms = dur;
    tr.offset = offset;
    // Classified on the command as written, line breaks and all; shown on one.
    size_t from = 0;
    tr.kind = reading ? ToolKind::Read : classify_tool(tool, command, &tr.group, &from);
    tr.failed = failed;
    // A file tool takes milliseconds. One that took many seconds sat on a
    // permission prompt: that time was spent waiting for the user.
    tr.waited = dur > 15000 && (tr.kind == ToolKind::Read || tr.kind == ToolKind::Edit) &&
                 command.find('\n') == std::string::npos && !one_of(lower(tool), {"bash", "exec", "shell"});
    tr.tool = std::move(tool);
    // Shown from the step that said what it is for, not from `S=/tmp/x; cd y &&`.
    tr.command = tools::one_line(std::string_view(command).substr(std::min(from, command.size())));
    out.runs.push_back(std::move(tr));
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



};

// Reads the complete lines from r.offset on.
void read_from(LineReader& j, const std::string& agent, ChatActivity& out, ActivityIndex::Resume& r) {
  Reader rd{out, r};
  const Adapter* adapter = adapter_for(agent);
  r.offset = j.each(r.offset, [&](std::string_view raw, uint64_t at) {
    // Before the calls: a result's changes look up the call it answers.
    rd.edits(agent, raw, at);
    if (adapter) adapter->read_tools(raw, at, rd);
  });
}

}  // namespace

ChatActivity ActivityIndex::read_file(const std::string& path, const std::string& agent, const std::string& cwd) {
  ChatActivity a;
  a.path = path;
  a.agent = agent;
  a.cwd = cwd;
  Resume r;
  LineReader j(path);
  read_from(j, agent, a, r);
  return a;
}

void ActivityIndex::start(const std::vector<Project>& projects, const Store& store) {
  // A pass still reading is abandoned: its entries are read again below.
  batch_.stop();
  jobs_.clear();
  chats_.clear();
  order_.clear();
  size_t order = 0;
  for (const auto& p : projects)
    for (const auto& s : p.sessions) {
      const std::string* name = store.custom_name(s.agent, s.id);
      std::string title = name ? *name : s.title;
      auto it = cache_.find(s.path);
      if (it != cache_.end() && it->second.mtime == s.mtime && it->second.size == s.bytes) {
        it->second.data.title = title;
        it->second.data.project = p.name;
        chats_.push_back(&it->second.data);
        order_.push_back(order++);
        continue;
      }
      jobs_.push_back(Job{s, p.name, std::move(title), order++});
    }
}

// The files of a pass are read on worker threads, each bringing its own cache
// entry up to date; step() merges what has finished, between frames, and
// once all have, puts the chats back in listing order.
bool ActivityIndex::step(int budget_ms) {
  if (!jobs_.empty() && !batch_.running()) {
    std::vector<Work> work;
    work.reserve(jobs_.size());
    for (Job& job : jobs_) {
      Work w;
      if (auto it = cache_.find(job.s.path); it != cache_.end()) {
        w.c = std::move(it->second);
        cache_.erase(it);
      }
      w.job = std::move(job);
      work.push_back(std::move(w));
    }
    jobs_.clear();
    batch_.start(std::move(work), [](Work& w) {
      Cached& c = w.c;
      const Job& job = w.job;
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
      LineReader j(job.s.path);
      read_from(j, job.s.agent, c.data, c.resume);
    }, Batch<Work>::default_threads());
  }
  // Waits up to the budget for the pass to finish, merging as files do: a
  // view's first frame can afford a moment for real numbers, the background
  // warm-up (budget 0) only merges what is done.
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
  for (;;) {
    batch_.collect([&](Work& w) {
      Cached& c = cache_[w.job.s.path] = std::move(w.c);
      chats_.push_back(&c.data);
      order_.push_back(w.job.order);
    });
    if (!batch_.running() || std::chrono::steady_clock::now() >= until) break;
    batch_.wait(int(std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count()) + 1);
  }
  if (!complete()) return false;
  // Finished: back in listing order, as a pass read on one thread had them.
  std::vector<size_t> idx(chats_.size());
  for (size_t i = 0; i < idx.size(); i++) idx[i] = i;
  std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return order_[a] < order_[b]; });
  std::vector<const ChatActivity*> sorted;
  sorted.reserve(idx.size());
  for (size_t i : idx) sorted.push_back(chats_[i]);
  chats_ = std::move(sorted);
  std::sort(order_.begin(), order_.end());
  return true;
}

}  // namespace mico
