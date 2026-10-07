#include "adapters/claude/claude.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <map>

#include "adapters/listing.h"
#include "base/fs.h"
#include "base/json.h"
#include "base/text.h"

namespace mico {

namespace {

// A subagent's run, as Claude keeps it beside the session that started it:
// <session>/subagents/agent-<id>.jsonl, and agent-<id>.meta.json naming the
// call that started it, what it was asked to do and the agent it runs as.
struct ClaudeSubagent {
  std::string path, agent_id, tool_use, description, type;
};

void for_each_claude_subagent(const std::string& transcript, const std::function<void(ClaudeSubagent&&)>& fn) {
  if (!fs::has_suffix(transcript, ".jsonl")) return;
  const std::string dir = transcript.substr(0, transcript.size() - 6) + "/subagents";
  std::string buf;
  fs::list_dir(dir, false, [&](const std::string& name) {
    if (!name.starts_with("agent-") || !fs::has_suffix(name, ".jsonl")) return;
    ClaudeSubagent a;
    a.path = dir + "/" + name;
    a.agent_id = name.substr(6, name.size() - 12);
    const std::string_view meta = fs::read_prefix(a.path.substr(0, a.path.size() - 6) + ".meta.json", 8 << 10, buf);
    js::scan_object(meta, [&](std::string_view k, const js::Value& v) {
      if (k == "toolUseId") a.tool_use = std::string(v.body());
      else if (k == "description") js::unescape_append(v.body(), a.description);
      else if (k == "agentType") js::unescape_append(v.body(), a.type);
      return true;
    });
    fn(std::move(a));
  });
}

}  // namespace

void ClaudeAdapter::call_subagents(const std::string& path, std::string_view, uint64_t tool_id,
                                   std::vector<SubagentRun>& out) const {
  for_each_claude_subagent(path, [&](ClaudeSubagent&& a) {
    if (a.tool_use.empty() || hash_id(a.tool_use) != tool_id) return;
    SubagentRun r;
    r.id = a.agent_id;
    r.name = !a.description.empty() ? a.description : !a.type.empty() ? a.type : a.agent_id;
    r.kind = std::move(a.type);
    r.path = std::move(a.path);
    out.push_back(std::move(r));
  });
}

void ClaudeAdapter::list_sessions(const std::function<void(SessionRef&&)>& add) const {
  std::string buf;
  const std::string root = claude_home() + "/projects";
  fs::list_dir(root, true, [&](const std::string& slug) {
    const std::string dir = root + "/" + slug;
    fs::list_dir(dir, false, [&](const std::string& fname) {
      if (!fs::has_suffix(fname, ".jsonl")) return;

      SessionRef s;
      s.agent = id();
      s.path = dir + "/" + fname;
      s.id = fname.substr(0, fname.size() - 6);
      s.mtime = fs::mtime(s.path, &s.bytes);

      // A transcript's ai-title lands early — measured across this machine's
      // sessions, by 54 KB — so a prefix answers almost every file. Only one
      // that tells us nothing is worth reading in full.
      std::string first_user;
      auto scan = [&](std::string_view blob) {
      fs::for_each_line(blob, [&](std::string_view line) {
        // Listing needs three things: cwd, the ai-title, and the first real
        // user turn. Anything else is skipped before it is scanned.
        const bool maybe_title = text::contains(line, "ai-title");
        const bool maybe_cwd = s.cwd.empty() && text::contains(line, "\"cwd\":");
        const bool maybe_user = first_user.empty() && text::contains(line, "\"type\":\"user\"");
        if (!maybe_title && !maybe_cwd && !maybe_user) return true;

        std::string_view type, title;
        js::Value message{};
        bool meta = false;
        js::scan_object(line, [&](std::string_view k, const js::Value& v) {
          if (k == "cwd" && v.is_string() && s.cwd.empty()) s.cwd = std::string(v.body());
          else if (k == "type") type = v.body();
          else if (k == "aiTitle") title = v.body();
          else if (k == "isMeta") meta = v.is_true();
          else if (k == "message") message = v;
          return true;
        });

        if (type == "ai-title" && !title.empty()) {
          s.title.clear();
          js::unescape_append(title, s.title);
          // The best title available: stop looking, once the folder is known
          // too. Without it the chat would be filed under no tracked folder.
          return s.cwd.empty();
        }
        if (first_user.empty() && type == "user" && !meta && message.is_object()) {
          js::scan_object(message.raw, [&](std::string_view k, const js::Value& v) {
            if (k != "content" || !v.is_string()) return true;
            std::string text;
            js::unescape_append(v.body(), text);
            if (!text.empty() && !is_noise_prompt(text)) first_user = std::move(text);
            return false;
          });
        }
        return true;
      });
      };

      constexpr size_t kPrefix = 128u << 10;
      scan(fs::read_prefix(s.path, kPrefix, buf));
      if (s.title.empty() && first_user.empty() && s.bytes > kPrefix)
        scan(fs::read_prefix(s.path, 4u << 20, buf));

      if (s.title.empty()) s.title = first_user;
      if (s.cwd.empty()) {
        // Fall back to decoding the directory slug: "-home-me-proj" -> "/home/me/proj".
        s.cwd = slug;
        for (char& c : s.cwd)
          if (c == '-') c = '/';
      }
      // Its subagents' runs, each a chat of its own under it.
      for_each_claude_subagent(s.path, [&](ClaudeSubagent&& a) {
        SessionRef r;
        r.agent = s.agent;
        r.path = std::move(a.path);
        r.id = std::move(a.agent_id);
        r.cwd = s.cwd;
        r.mtime = fs::mtime(r.path, &r.bytes);
        r.parent = s.path;
        r.subagent = !a.description.empty() ? a.description : !a.type.empty() ? a.type : r.id;
        r.title = "\xE2\x86\xB3 " + r.subagent;  // ↳
        if (!s.title.empty()) r.title += " \xC2\xB7 " + text::oneline(s.title, 0);
        add(std::move(r));
      });
      add(std::move(s));
    });
  });
}

// Claude Code's per-folder trust lives in ~/.claude.json under
// projects["<abs path>"].hasTrustDialogAccepted. Writing that entry ahead of
// time is exactly what clicking "Yes, I trust this folder" does — so a folder
// the user added to mico never shows the dialog at all. A targeted text merge,
// not a full re-serialise: only insert an entry that is not already there.
void ClaudeAdapter::trust_folder(const std::string& abspath) const {
  const std::string file = claude_state_file();
  std::string buf;
  std::string_view blob = fs::read_prefix(file, 8u << 20, buf);
  if (blob.empty()) return;
  std::string doc(blob);

  const size_t pk = doc.find("\"projects\"");
  if (pk == std::string::npos) return;
  const size_t brace = doc.find('{', pk);
  if (brace == std::string::npos) return;

  // JSON-escape the path (backslash and quote only; Linux paths rarely need it).
  std::string key = "\"";
  for (char c : abspath) {
    if (c == '\\' || c == '"') key.push_back('\\');
    key.push_back(c);
  }
  key.push_back('"');

  // Already present anywhere in the projects object? Leave it alone.
  if (doc.find(key, brace) != std::string::npos) return;

  const std::string entry = "\n    " + key + ": {\"hasTrustDialogAccepted\": true},";
  doc.insert(brace + 1, entry);

  const std::string tmp = file + ".mico.tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return;
  bool ok = write(fd, doc.data(), doc.size()) == ssize_t(doc.size());
  ::close(fd);
  if (ok) rename(tmp.c_str(), file.c_str());
  else unlink(tmp.c_str());
}

}  // namespace mico
