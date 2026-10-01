#include <algorithm>

#include "core/adapter.h"
#include "core/user_text.h"

// Shared adapter for earendil-works/pi-coding-agent ("pi") and Oh My Pi
// ("omp"), which is built on the same engine: both write the same
// {"type":"message","message":{"role":...,"content":[...]}} record shape, the
// same model_change / thinking_level_change events, and the same toolCall /
// toolResult content blocks. Only the agent id string differs, so one class
// serves both — see pi_adapter() / omp_adapter() below.
namespace mico {
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
// because omp's edit tool carries its whole apply_patch body there.
Str tool_arg_summary(Arena& arena, const js::Value& args) {
  if (!args.is_object()) return {};
  static constexpr std::string_view kByName[] = {"command", "input",      "path",
                                                 "pattern", "query",      "description",
                                                 "prompt",  "url"};
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
// edit's argument (both pi and omp) already *is* an apply_patch body, so it
// only needs narrowing to the patch markers — the same trick codex's
// apply_patch gets, because it is the same patch language.
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

class PiAdapter final : public Adapter {
 public:
  explicit PiAdapter(std::string_view id) : id_(id) {}
  std::string_view id() const override { return id_; }

  void seed_state(SessionState& st) const override {
    st.declare("model", "model");
    st.declare("provider", "provider");
    st.declare("effort", "thinking");
  }

  void observe(std::string_view raw, SessionState& st) const override {
    std::string_view type;
    js::Value message{};
    js::scan_object(raw, [&](std::string_view k, const js::Value& v) {
      if (k == "type") {
        type = v.body();
        return type == "model_change" || type == "thinking_level_change" || type == "message";
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

  void parse(std::string_view raw, Arena& arena, std::vector<Event>& out) const override {
    // "type" leads every record, so the hundreds of omp "custom"/telemetry
    // lines that dwarf the conversation in a session are rejected after one
    // member; the rest are model_change/thinking_level_change/title records,
    // already handled by observe(), and carry nothing parse() can show.
    std::string_view type;
    js::Value message{};
    js::scan_object(raw, [&](std::string_view k, const js::Value& v) {
      if (k == "type") { type = v.body(); return type == "message"; }
      if (k == "message") { message = v; return false; }
      return true;
    });
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
        if (!(is_question_tool(arena.view(e.name)) && build_question(e, arena, args))) {
          e.summary = tool_arg_summary(arena, args);
          e.detail = build_detail(arena, arena.view(e.name), args, e.summary);
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

 private:
  std::string_view id_;
};

const PiAdapter g_pi("pi");
const PiAdapter g_omp("omp");

}  // namespace

const Adapter& pi_adapter() { return g_pi; }
const Adapter& omp_adapter() { return g_omp; }

}  // namespace mico
