#include <algorithm>

#include "adapters/pi/pi.h"
#include "adapters/user_text.h"

// The transcript reader pi and omp share; see pi.h.
namespace mico {

std::string edit_script_paths(std::string_view script) {
  std::vector<std::string_view> paths;
  size_t pos = 0;
  while (pos < script.size()) {
    size_t nl = script.find('\n', pos);
    if (nl == std::string_view::npos) nl = script.size();
    std::string_view line = script.substr(pos, nl - pos);
    pos = nl + 1;
    std::string_view path;
    if (line.size() > 2 && line.front() == '[' && line.back() == ']') {
      path = line.substr(1, line.size() - 2);
      if (const size_t hash = path.rfind('#'); hash != std::string_view::npos) path = path.substr(0, hash);
    } else {
      for (std::string_view tag : {std::string_view("*** Update File: "), std::string_view("*** Add File: "),
                                   std::string_view("*** Delete File: ")})
        if (line.starts_with(tag)) path = line.substr(tag.size());
    }
    if (!path.empty() && std::find(paths.begin(), paths.end(), path) == paths.end()) paths.push_back(path);
  }
  std::string out;
  for (std::string_view p : paths) {
    if (!out.empty()) out += ", ";
    out += p;
  }
  return out;
}

namespace {

// content is an array of {"type":"text","text":...} blocks (images and other
// block types are skipped; a chat pane has nowhere to put them yet).
Str add_content(Arena& arena, const js::Value& content) {
  if (content.is_string()) return arena.add_json(content);
  uint32_t off = arena.open();
  if (content.is_array()) {
    bool first = true;
    js::scan_array(content.raw, [&](const js::Value& b) {
      if (!b.is_object()) return true;
      js::scan_object(b.raw, [&](std::string_view k, const js::Value& v) {
        if (k != "text" || !v.is_string()) return true;
        if (!first) arena.put("\n");
        arena.put_json(v);
        first = false;
        return false;
      });
      return arena.since(off) < Arena::kMaxText;
    });
  }
  return arena.close(off);
}

// Preferred argument to show for a tool call, in priority order. "input" leads
// because omp's edit tool carries its whole edit script there; "code" is omp's
// eval. Failing those, omp's "i" — the intent every one of its calls states
// ("Awaiting worker reports") — says more than whichever string comes first.
Str tool_arg_summary(Arena& arena, const js::Value& args) {
  if (!args.is_object()) return {};
  static constexpr std::string_view kByName[] = {"command", "input",       "path",
                                                 "pattern", "query",       "description",
                                                 "prompt",  "url",         "code",
                                                 "i"};
  js::Value best{};
  int best_rank = 99;
  js::Value fallback{};

  js::scan_object(args.raw, [&](std::string_view k, const js::Value& v) {
    if (!v.is_string()) return true;
    for (int i = 0; i < int(std::size(kByName)); i++) {
      if (k == kByName[i]) {
        if (i < best_rank) { best_rank = i; best = v; }
        return best_rank != 0;
      }
    }
    if (fallback.type == js::Type::Null) fallback = v;
    return true;
  });
  const js::Value& pick = best_rank < 99 ? best : fallback;
  return pick.is_string() ? arena.add_json(pick) : Str{};
}

void put_prefixed(Arena& a, std::string_view text, char prefix, size_t max_lines) {
  size_t pos = 0, n = 0;
  while (pos <= text.size() && n < max_lines) {
    size_t nl = text.find('\n', pos);
    size_t end = nl == std::string_view::npos ? text.size() : nl;
    a.put(std::string_view(&prefix, 1));
    a.put(text.substr(pos, end - pos));
    a.put("\n");
    n++;
    if (nl == std::string_view::npos) break;
    pos = nl + 1;
  }
}

// Builds the expanded payload for edit-shaped tools. write's argument is a
// plain path+content pair, shown as a "+" diff like any other new file.
// omp's edit argument is an edit script between apply_patch's markers — once
// apply_patch itself, now omp's own language of "[path#hash]" headers and
// SWAP/INS/DEL/CUT commands — and is shown narrowed to those markers. What it
// did, as a diff, comes with the result; see edit_diff().
Str build_detail(Arena& arena, std::string_view name, const js::Value& args, Str summary) {
  if (name == "write") {
    if (!args.is_object()) return {};
    js::Value content{};
    js::scan_object(args.raw, [&](std::string_view k, const js::Value& v) {
      if (k != "content") return true;
      content = v;
      return false;
    });
    if (!content.is_string()) return {};
    thread_local std::string scratch;
    scratch.clear();
    js::unescape_append(content.body(), scratch);
    uint32_t off = arena.open();
    put_prefixed(arena, scratch, '+', 400);
    return arena.close(off);
  }
  if (name == "edit") {
    std::string_view s = arena.view(summary);
    size_t begin = s.find("*** Begin Patch");
    if (begin == std::string_view::npos) return {};
    size_t end = s.find("*** End Patch", begin);
    end = end == std::string_view::npos ? s.size() : end + 13;
    return Str{uint32_t(summary.off + begin), uint32_t(end - begin)};
  }
  return {};
}

// mico's plot tool, as the extension gives it to pi ("mico_plot") and as omp
// calls an extension's tool: through its write tool, at "xd://mico_plot", the
// arguments as the content. `out` gets the arguments' JSON.
bool plot_args(std::string_view name, const js::Value& args, std::string& out) {
  if (!args.is_object()) return false;
  if (is_mico_plot(name)) {
    out = std::string(args.raw);
    return true;
  }
  if (name != "write") return false;
  std::string_view path;
  js::Value content{};
  js::scan_object(args.raw, [&](std::string_view k, const js::Value& v) {
    if (k == "path") path = v.body();
    else if (k == "content") content = v;
    return true;
  });
  if (path != "xd://mico_plot" || !content.is_string()) return false;
  js::unescape_append(content.body(), out);
  return true;
}

// omp's edit result carries what the edit did as a numbered diff
// (details.diff: " 17|kept", "+18|added", "-19|removed"): the result shows it
// in place of its one-line acknowledgement.
Str edit_diff(Arena& arena, const js::Value& message) {
  std::string_view tool;
  js::Value details{};
  js::scan_object(message.raw, [&](std::string_view k, const js::Value& v) {
    if (k == "toolName") tool = v.body();
    else if (k == "details") details = v;
    return true;
  });
  if (tool != "edit" || !details.is_object()) return {};
  js::Value diff{};
  js::scan_object(details.raw, [&](std::string_view k, const js::Value& v) {
    if (k != "diff") return true;
    diff = v;
    return false;
  });
  return diff.is_string() ? arena.add_json(diff) : Str{};
}

// The one line a displayed omp custom_message gets, or false when it is not
// one mico shows: background jobs finishing (async-result), and other agents'
// messages (irc:incoming). A guest's prompt (collab-prompt, attributed to
// the user) is a user turn.
bool custom_message(Arena& arena, std::string_view raw, Event& e) {
  std::string_view kind, attribution;
  bool display = false;
  js::Value content{}, details{};
  js::scan_object(raw, [&](std::string_view k, const js::Value& v) {
    if (k == "customType") kind = v.body();
    else if (k == "display") display = v.is_true();
    else if (k == "attribution") attribution = v.body();
    else if (k == "content") content = v;
    else if (k == "details") details = v;
    return true;
  });
  if (!display) return false;
  if (attribution == "user") {
    e.text = add_content(arena, content);
    if (e.text.empty()) return false;
    e.kind = EventKind::User;
    return true;
  }
  thread_local std::string text;
  text.clear();
  if (kind == "async-result") {
    // "<system-notice>\nBackground job X has completed. Resume your work …":
    // the sentence before the instructions.
    if (!content.is_string()) return false;
    js::unescape_append(content.body(), text);
    size_t at = text.find("<system-notice>");
    at = at == std::string::npos ? 0 : at + 15;
    while (at < text.size() && text[at] == '\n') at++;
    size_t end = text.find('\n', at);
    std::string line = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
    if (const size_t cut = line.find(" Resume your work"); cut != std::string::npos) line.resize(cut);
    if (line.empty()) return false;
    e.ok = line.find("failed") == std::string::npos && line.find("cancelled") == std::string::npos;
    e.text = arena.add(line);
  } else if (kind == "irc:incoming") {
    // details carries the sender and the message, without the envelope.
    if (!details.is_object()) return false;
    std::string from;
    js::scan_object(details.raw, [&](std::string_view k, const js::Value& v) {
      if (k == "from") js::unescape_append(v.body(), from);
      else if (k == "message") js::unescape_append(v.body(), text);
      return true;
    });
    if (text.empty()) return false;
    e.ok = true;
    e.text = arena.add((from.empty() ? std::string("agent") : from) + ": " + text);
  } else {
    return false;
  }
  e.kind = EventKind::TaskStatus;
  return true;
}

}  // namespace

void PiFamilyAdapter::seed_state(SessionState& st) const {
  st.declare("model", "model");
  st.declare("provider", "provider");
  st.declare("effort", "thinking");
}

void PiFamilyAdapter::observe(std::string_view raw, SessionState& st) const {
  std::string_view type;
  js::Value message{};
  js::scan_object(raw, [&](std::string_view k, const js::Value& v) {
    if (k == "type") {
      type = v.body();
      return type == "model_change" || type == "thinking_level_change" || type == "message" ||
             type == "mode_change" || type == "service_tier_change";
    }
    if (type == "model_change") {
      if (k == "modelId") st.set("model", "model", v.body());
      else if (k == "provider") st.set("provider", "provider", v.body());
      return true;
    }
    if (type == "thinking_level_change") {
      if (k == "thinkingLevel") st.set("effort", "thinking", v.body());
      return true;
    }
    // omp's modes (plan, vibe, goal) and its service tier
    // ({"openai":"priority"}), each back to "none"/null when left.
    if (type == "mode_change") {
      if (k == "mode") st.set("mode", "mode", v.body() == "none" ? "default" : v.body());
      return true;
    }
    if (type == "service_tier_change") {
      if (k != "serviceTier") return true;
      std::string_view tier = "default";
      if (v.is_object())
        js::scan_object(v.raw, [&](std::string_view, const js::Value& t) {
          if (t.is_string()) tier = t.body();
          return !t.is_string();
        });
      st.set("tier", "tier", tier);
      return true;
    }
    if (k == "message") { message = v; return false; }
    return true;
  });
  // A message record also carries the model/provider it was actually
  // answered with — useful the moment a session is opened, before any
  // model_change event has had a chance to fire.
  if (type == "message" && message.is_object()) {
    js::scan_object(message.raw, [&](std::string_view k, const js::Value& v) {
      if (k == "role") return v.body() == "assistant";
      if (k == "model") st.set("model", "model", v.body());
      else if (k == "provider") st.set("provider", "provider", v.body());
      return true;
    });
  }
}

void PiFamilyAdapter::parse(std::string_view raw, Arena& arena, std::vector<Event>& out) const {
  // "type" leads every record, so the hundreds of omp "custom" telemetry
  // lines that dwarf the conversation in a session are rejected after one
  // member; the rest are model_change/thinking_level_change/title records,
  // already handled by observe(), and carry nothing parse() can show.
  std::string_view type;
  js::Value message{};
  js::scan_object(raw, [&](std::string_view k, const js::Value& v) {
    if (k == "type") {
      type = v.body();
      return type == "message";
    }
    if (k == "message") { message = v; return false; }
    return true;
  });
  if (type == "compaction") {
    Event e;
    e.kind = EventKind::Notice;
    e.text = arena.add("Conversation compacted");
    out.push_back(e);
    return;
  }
  if (type == "custom_message") {
    Event e;
    if (custom_message(arena, raw, e)) out.push_back(e);
    return;
  }
  if (type != "message" || !message.is_object()) return;

  std::string_view role, stop_reason;
  js::Value content{};
  js::scan_object(message.raw, [&](std::string_view k, const js::Value& v) {
    if (k == "role") role = v.body();
    else if (k == "stopReason") stop_reason = v.body();
    else if (k == "content") content = v;
    return true;
  });

  if (role == "user") {
    if (content.type == js::Type::Null) return;
    Event e;
    e.text = add_content(arena, content);
    if (e.text.empty()) return;
    e.kind = EventKind::User;
    e.text = unwrap_pasted_content(arena, e.text);
    out.push_back(e);
    return;
  }

  if (role == "toolResult") {
    std::string_view call_id;
    js::Value result{};
    bool is_error = false;
    js::scan_object(message.raw, [&](std::string_view k, const js::Value& v) {
      if (k == "toolCallId") call_id = v.body();
      else if (k == "content") result = v;
      else if (k == "isError") is_error = v.is_true();
      return true;
    });
    Event e;
    e.kind = EventKind::ToolResult;
    e.tool_id = hash_id(call_id);
    e.ok = !is_error;
    if (result.type != js::Type::Null) e.text = add_content(arena, result);
    if (e.ok) e.detail = edit_diff(arena, message);
    out.push_back(e);
    return;
  }

  if (role != "assistant" || !content.is_array()) return;

  js::scan_array(content.raw, [&](const js::Value& item) {
    if (!item.is_object()) return true;
    std::string_view itype;
    js::Value text{}, thinking{}, name{}, id{}, args{};
    js::scan_object(item.raw, [&](std::string_view k, const js::Value& v) {
      if (k == "type") itype = v.body();
      else if (k == "text") text = v;
      else if (k == "thinking") thinking = v;
      else if (k == "name") name = v;
      else if (k == "id") id = v;
      else if (k == "arguments") args = v;
      return true;
    });

    Event e;
    if (itype == "text") {
      if (!text.is_string()) return true;
      e.text = arena.add_json(text);
      if (e.text.empty()) return true;
      e.kind = EventKind::Assistant;
    } else if (itype == "thinking") {
      if (!thinking.is_string()) return true;
      e.text = arena.add_json(thinking);
      if (e.text.empty()) return true;
      e.kind = EventKind::Thinking;
    } else if (itype == "toolCall") {
      e.kind = EventKind::ToolCall;
      e.name = name.is_string() ? arena.add_json(name) : arena.add("?");
      e.tool_id = hash_id(id.body());
      if (std::string chart; plot_args(arena.view(e.name), args, chart)) {
        make_chart_event(e, arena, chart);
      } else if (!(is_question_tool(arena.view(e.name)) && build_question(e, arena, args))) {
        e.summary = tool_arg_summary(arena, args);
        e.detail = build_detail(arena, arena.view(e.name), args, e.summary);
        // An edit script's first line is a marker; the files it edits say more.
        if (arena.view(e.name) == "edit" && !e.detail.empty())
          if (const std::string paths = edit_script_paths(arena.view(e.detail)); !paths.empty())
            e.summary = arena.add(paths);
      }
    } else {
      return true;  // e.g. a redacted/encrypted block with nothing to show
    }
    out.push_back(e);
    return true;
  });
  if (stop_reason == "stop" || stop_reason == "error" || stop_reason == "aborted") {
    Event end;
    end.kind = EventKind::TurnEnd;
    end.ok = stop_reason == "stop";
    out.push_back(end);
  }
}

}  // namespace mico
