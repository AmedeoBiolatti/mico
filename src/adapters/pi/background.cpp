#include "adapters/pi/pi.h"

#include "adapters/tool_calls.h"
#include "base/text.h"
#include "base/time.h"

// omp's background work, as its transcript records it. pi runs nothing in the
// background.
namespace mico {

using namespace tools;

namespace {

// A tool result's job ids that have ended, from a jobs snapshot ("wait" and
// "hub" list them, each with a status) or a delivery of finished jobs.
void ended_jobs(const js::Value& jobs, BackgroundTasks& t) {
  js::scan_array(jobs.raw, [&](const js::Value& j) {
    std::string id, status;
    js::scan_object(j.raw, [&](std::string_view k, const js::Value& v) {
      if (k == "id" || k == "jobId") id = text_of(v);
      else if (k == "status") status = text_of(v);
      return true;
    });
    if (!id.empty() && status != "running") t.end(id);
    return true;
  });
}

}  // namespace

// What starts work in the background:
//   - a tool result whose details say {"async": {"state": "running", "jobId",
//     "type"}}: a bash command run with async, or a task's subagents (each
//     named in details.progress);
//   - hub's services, details.daemon, running until a result says otherwise;
//   - vibe sessions, between "turn-started" and "turn-settled" records.
// What ends it: the delivery of finished jobs (an "async-result" message),
// a jobs snapshot whose status is no longer "running", a daemon stopped, a
// vibe turn settled or its session gone ("tombstone").
void OmpAdapter::read_background(std::string_view raw, uint64_t offset, BackgroundTasks& t) const {
  const bool call = text::contains(raw, "\"toolCall\"");
  const bool result = text::contains(raw, "\"toolResult\"") &&
                      (text::contains(raw, "\"async\"") || text::contains(raw, "\"jobs\"") ||
                       text::contains(raw, "\"daemon\""));
  const bool delivered = text::contains(raw, "\"async-result\"");
  const bool vibe = text::contains(raw, "\"vibe-session-lifecycle\"");
  if (!call && !result && !delivered && !vibe) return;

  std::string_view type;
  int64_t at = 0;
  js::Value message{}, details{}, data{};
  js::scan_object(raw, [&](std::string_view k, const js::Value& v) {
    if (k == "type") type = v.body();
    else if (k == "timestamp") at = parse_time(v.body());
    else if (k == "message") message = v;
    else if (k == "details") details = v;
    else if (k == "data") data = v;
    return true;
  });

  if (type == "custom_message" && delivered && details.is_object()) {
    js::scan_object(details.raw, [&](std::string_view k, const js::Value& v) {
      if (k == "jobs" && v.is_array()) ended_jobs(v, t);
      return true;
    });
    return;
  }

  if (type == "custom" && vibe && data.is_object()) {
    std::string id, action, cli;
    js::scan_object(data.raw, [&](std::string_view k, const js::Value& v) {
      if (k == "id") id = text_of(v);
      else if (k == "action") action = text_of(v);
      else if (k == "cli") cli = text_of(v);
      return true;
    });
    if (id.empty()) return;
    if (action == "turn-started") {
      BackgroundTasks::Call c;
      c.kind = "agent";
      c.what = id;
      c.at_ms = at;
      c.offset = offset;
      t.start(id, c);
    } else if (action == "turn-settled" || action == "tombstone") {
      t.end(id);
    }
    return;
  }

  if (type != "message" || !message.is_object()) return;
  std::string role, call_id;
  js::Value content{};
  js::scan_object(message.raw, [&](std::string_view k, const js::Value& v) {
    if (k == "role") role = text_of(v);
    else if (k == "toolCallId") call_id = text_of(v);
    else if (k == "content") content = v;
    else if (k == "details") details = v;
    return true;
  });

  if (role == "assistant" && content.is_array()) {
    // The calls that may start something, kept until their result says.
    js::scan_array(content.raw, [&](const js::Value& b) {
      std::string_view btype;
      std::string id, name;
      js::Value args{};
      js::scan_object(b.raw, [&](std::string_view k, const js::Value& v) {
        if (k == "type") btype = v.body();
        else if (k == "id") id = text_of(v);
        else if (k == "name") name = text_of(v);
        else if (k == "arguments") args = v;
        return true;
      });
      if (btype != "toolCall" || id.empty() || !args.is_object()) return true;
      if (name != "bash" && name != "task" && name != "hub") return true;
      std::string intent, command, op;
      bool async = false;
      js::scan_object(args.raw, [&](std::string_view k, const js::Value& v) {
        if (k == "i") intent = text_of(v);
        else if (k == "command") command = text_of(v);
        else if (k == "op") op = text_of(v);
        else if (k == "async") async = v.is_true();
        return true;
      });
      if (name == "bash" && !async) return true;
      if (name == "hub" && op != "start" && op != "restart") return true;
      BackgroundTasks::Call c;
      c.kind = name == "task" ? "agent" : "shell";
      c.what = one_line(intent.empty() ? command : intent, 160);
      c.at_ms = at;
      c.offset = offset;
      t.calls[id] = std::move(c);
      return true;
    });
    return;
  }

  if (role != "toolResult" || !details.is_object()) return;
  BackgroundTasks::Call c;
  if (const auto it = t.calls.find(call_id); it != t.calls.end()) {
    c = std::move(it->second);
    t.calls.erase(it);
  } else {
    c.at_ms = at;
    c.offset = offset;
  }
  js::scan_object(details.raw, [&](std::string_view k, const js::Value& v) {
    if (k == "jobs" && v.is_array()) {
      ended_jobs(v, t);
    } else if (k == "async" && v.is_object()) {
      std::string state, job, kind;
      js::scan_object(v.raw, [&](std::string_view ak, const js::Value& av) {
        if (ak == "state") state = text_of(av);
        else if (ak == "jobId") job = text_of(av);
        else if (ak == "type") kind = text_of(av);
        return true;
      });
      if (state != "running" || job.empty()) return true;
      if (kind == "task") {
        // One job per subagent, each ending on its own.
        bool any = false;
        js::scan_object(details.raw, [&](std::string_view pk, const js::Value& pv) {
          if (pk != "progress" || !pv.is_array()) return true;
          js::scan_array(pv.raw, [&](const js::Value& p) {
            std::string id, agent;
            js::scan_object(p.raw, [&](std::string_view qk, const js::Value& qv) {
              if (qk == "id") id = text_of(qv);
              else if (qk == "agent") agent = text_of(qv);
              return true;
            });
            if (id.empty()) return true;
            BackgroundTasks::Call one = c;
            one.kind = "agent";
            one.what = agent.empty() ? id : id + " \xC2\xB7 " + agent;
            t.start(id, one);
            any = true;
            return true;
          });
          return false;
        });
        if (any) return true;
      }
      if (c.kind.empty()) c.kind = kind == "task" ? "agent" : "shell";
      if (c.what.empty()) c.what = job;
      t.start(job, c);
    } else if (k == "daemon" && v.is_object()) {
      std::string name, state;
      js::scan_object(v.raw, [&](std::string_view dk, const js::Value& dv) {
        if (dk == "name") name = text_of(dv);
        else if (dk == "state") state = text_of(dv);
        return true;
      });
      if (name.empty()) return true;
      if (state == "running") {
        c.kind = "shell";
        if (c.what.empty()) c.what = name;
        t.start(name, c);
      } else {
        t.end(name);
      }
    }
    return true;
  });
}

}  // namespace mico
