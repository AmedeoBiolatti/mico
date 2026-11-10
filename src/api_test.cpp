// `mico-test --api-test`: the state protocol against a workspace over fixture
// transcripts. Runs under the test HOME ctest sets up; starts no agent.
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <string>
#include <vector>

#include "api/client.h"
#include "base/fs.h"
#include "base/json_write.h"
#include "base/text.h"

namespace mico {
namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
  if (!ok) {
    fprintf(stderr, "FAIL api: %s\n", what);
    g_failures++;
  }
}

// The messages of one poll, as one string, for finding things in.
std::string poll_all(api::Client& c) {
  std::vector<std::string> out;
  c.poll(out);
  std::string all;
  for (const auto& m : out) all += m + "\n";
  if (getenv("MICO_API_DUMP")) fputs(all.c_str(), stderr);  // to see the protocol
  return all;
}

bool has(const std::string& s, std::string_view needle) { return s.find(needle) != std::string::npos; }

bool valid_utf8(std::string_view s) {
  for (size_t i = 0; i < s.size();) {
    const size_t at = i;
    const char32_t cp = text::decode(s, i);
    if (cp == 0xFFFD && !(i - at == 3 && s.substr(at, 3) == "\xEF\xBF\xBD")) return false;
  }
  return true;
}

void append(const std::string& path, const std::string& line) {
  if (FILE* f = fopen(path.c_str(), "a")) {
    fputs((line + "\n").c_str(), f);
    fclose(f);
  }
}

}  // namespace

int run_api_test() {
  // --- the writer: valid UTF-8 whatever comes in -------------------------
  {
    std::string out;
    jw::string(out, std::string("a\"b\\c\n\x01 \xff \xed\xa0\x80 \xc0\x80 \xe2\x9c\x94", 23));
    check(has(out, "a\\\"b\\\\c\\n\\u0001"), "writer: quotes, backslashes and controls are escaped");
    check(valid_utf8(out), "writer: broken bytes, surrogates and overlong forms come out as U+FFFD");
    check(has(out, "\xe2\x9c\x94"), "writer: valid text is kept");
    std::string ids;
    jw::Writer(ids).begin_array().id(0xfedcba9876543210ull).id(1).end_array();
    check(ids == "[\"fedcba9876543210\",\"0000000000000001\"]", "writer: ids are 16 hex digits");
  }

  // --- a workspace over a fixture chat --------------------------------------
  const char* home = getenv("HOME");
  if (!home) {
    fprintf(stderr, "FAIL api: no HOME\n");
    return 1;
  }
  char tmpl[] = "/tmp/mico-api-XXXXXX";
  const std::string dir = mkdtemp(tmpl) ? std::string(tmpl) : std::string();
  check(!dir.empty(), "a folder to track");
  const std::string projects = std::string(home) + "/.claude/projects";
  mkdir((std::string(home) + "/.claude").c_str(), 0700);
  mkdir(projects.c_str(), 0700);
  mkdir((projects + "/api-test").c_str(), 0700);
  const std::string id = "0a0a0a0a-1111-4222-8333-444444444444";
  const std::string path = projects + "/api-test/" + id + ".jsonl";
  unlink(path.c_str());
  append(path, R"({"type":"user","cwd":")" + dir + R"(","message":{"role":"user","content":"draw the loss curve"}})");
  append(path, R"({"type":"assistant","cwd":")" + dir +
                   "\",\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"bad byte \xff here\"}]}}");

  // A codex chat in the same folder, which a rollback will rewind.
  const std::string codex_dir = std::string(home) + "/.codex/sessions/2026/11/06";
  fs::make_dirs(codex_dir);
  const std::string codex_path =
      codex_dir + "/rollout-2026-11-06T00-00-00-0b0b0b0b-1111-4222-8333-444444444444.jsonl";
  unlink(codex_path.c_str());
  const auto codex_message = [](std::string_view role, std::string_view text) {
    return R"({"type":"response_item","payload":{"type":"message","role":")" + std::string(role) +
           R"(","content":[{"type":")" + (role == "user" ? "input_text" : "output_text") + R"(","text":")" +
           std::string(text) + "\"}]}}";
  };
  append(codex_path, R"({"type":"session_meta","payload":{"id":"0b0b0b0b-1111-4222-8333-444444444444","cwd":")" + dir +
                         R"(","cli_version":"0.167.0"}})");
  append(codex_path, codex_message("user", "keep this question"));
  append(codex_path, codex_message("assistant", "kept answer"));
  append(codex_path, codex_message("user", "rewound question"));
  append(codex_path, codex_message("assistant", "rewound answer"));

  Workspace ws;
  ws.store().add_folder(dir, false);
  api::Client c(ws);

  std::string got = poll_all(c);
  check(has(got, R"("type":"hello","protocol":1)") && has(got, R"({"id":"claude","name":"Claude Code"})"),
        "hello comes first, naming the agents a client can start");
  check(has(got, R"("type":"folders")") && has(got, "\"path\":\"" + path + "\"") &&
            has(got, R"("title":"draw the loss curve")"),
        "the listing names the fixture chat by its first message");
  check(!has(got, R"("type":"folders")") || poll_all(c).find("folders") == std::string::npos,
        "an unchanged listing is not sent again");

  c.receive(R"({"type":"open","path":")" + path + "\"}");
  got = poll_all(c);
  check(has(got, R"("type":"chat")") && has(got, R"("where":"tail")") && has(got, R"("start":true)"),
        "opening a short chat sends its whole tail");
  check(has(got, R"("k":"user","at":0,"text":"draw the loss curve")"), "user turns carry their text and line offset");
  check(has(got, R"("k":"assistant")") && valid_utf8(got), "a reply with a broken byte still goes out as valid UTF-8");
  check(has(got, R"("type":"chat_state")"), "the chat's state follows it");

  append(path, R"({"type":"user","cwd":")" + dir + R"(","message":{"role":"user","content":"and the accuracy"}})");
  got = poll_all(c);
  check(has(got, R"("where":"newer")") && has(got, "and the accuracy"), "a line appended to the file is sent as it lands");
  check(!has(got, "draw the loss curve"), "only what is new is sent");
  check(poll_all(c).find("\"chat\"") == std::string::npos, "nothing is sent while nothing changes");

  // A rollback under turns already sent sends the tail again without them.
  c.receive(R"({"type":"open","path":")" + codex_path + "\"}");
  got = poll_all(c);
  check(has(got, R"("where":"tail")") && has(got, "rewound answer"), "the codex chat opens with every turn");
  append(codex_path, R"({"type":"event_msg","payload":{"type":"thread_rolled_back","num_turns":1}})");
  append(codex_path, codex_message("user", "after the rollback"));
  got = poll_all(c);
  check(has(got, R"("where":"tail")") && has(got, "kept answer") && has(got, "after the rollback") &&
            !has(got, "rewound"),
        "a rollback resends the codex chat's tail without the dropped turn");

  // Work metadata is cheap to follow; output is read only for a selected
  // command or subagent belonging to the open parent chat.
  append(path, R"({"type":"assistant","message":{"content":[{"type":"tool_use","id":"panel-command","name":"Bash","input":{"command":"printf panel"}}]}})");
  append(path, R"({"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"panel-command","content":"panel-output-marker"}]}})");
  c.receive(R"({"type":"work","path":")" + path + R"(","open":false})");
  got = poll_all(c);
  check(has(got, R"("type":"work_state")") && has(got, R"("kind":"terminal")") && !has(got, R"("output":)"),
        "work follows commands without eagerly sending their output");
  char tool_key[40];
  snprintf(tool_key, sizeof tool_key, "tool:%016llx", (unsigned long long)hash_id("panel-command"));
  c.receive(R"({"type":"work","path":")" + path + R"(","open":true,"item":")" + tool_key + R"("})");
  check(has(poll_all(c), R"("output":"panel-output-marker")"), "selecting a terminal reads its output");
  c.receive(R"({"type":"work","path":")" + path + R"(","open":true,"item":"/etc/passwd"})");
  check(!has(poll_all(c), R"("output":)"), "work selection cannot name arbitrary files");

  const std::string run_dir = path.substr(0, path.size() - 6);
  mkdir(run_dir.c_str(), 0700);
  mkdir((run_dir + "/subagents").c_str(), 0700);
  const std::string child = run_dir + "/subagents/agent-panel-child.jsonl";
  const std::string meta = run_dir + "/subagents/agent-panel-child.meta.json";
  append(meta, R"({"toolUseId":"panel-agent","description":"Review the change","agentType":"Explore"})");
  append(child, R"({"type":"assistant","message":{"content":[{"type":"text","text":"child-answer-marker"}]}})");
  append(path, R"({"type":"assistant","message":{"content":[{"type":"tool_use","id":"panel-agent","name":"Agent","input":{"description":"Review the change"}}]}})");
  c.receive(R"({"type":"work","path":")" + path + R"(","open":true,"item":"agent:)" + child + R"("})");
  got = poll_all(c);
  check(has(got, R"("kind":"agent")") && has(got, "child-answer-marker"),
        "a selected subagent's conversation appears beside its parent");
  append(child, R"({"type":"assistant","message":{"content":[{"type":"text","text":"child-update-marker"}]}})");
  c.receive(R"({"type":"work","path":")" + path + R"(","open":true,"item":"agent:)" + child + R"("})");
  check(has(poll_all(c), "child-update-marker"), "the selected subagent follows new messages");
  c.receive(R"({"type":"work","path":")" + path + R"(","open":false})");
  check(!has(poll_all(c), R"("output":)"), "closing the work panel stops sending detail");
  unlink(child.c_str()); unlink(meta.c_str());
  rmdir((run_dir + "/subagents").c_str()); rmdir(run_dir.c_str());

  // --- mico's own marks on a chat: a name, an archive flag ---------------------
  c.receive(R"({"type":"rename","rid":"m1","agent":"claude","id":")" + id + R"(","title":" Loss  curves\n"})");
  got = poll_all(c);
  check(has(got, R"("rid":"m1","ok":true)") && has(got, R"("title":"Loss curves")"),
        "a rename answers, and the listing comes again under the new name");
  c.receive(R"({"type":"archive","rid":"m2","agent":"claude","id":")" + id + R"(","on":true})");
  got = poll_all(c);
  check(has(got, R"("rid":"m2","ok":true)") && has(got, R"("archived":true)"),
        "archiving marks the chat in the listing");
  c.receive(R"({"type":"archive","rid":"m3","agent":"claude","id":")" + id + R"(","on":false})");
  got = poll_all(c);
  check(has(got, R"("rid":"m3","ok":true)") && has(got, R"("archived":false)"), "and unarchiving clears the mark");
  c.receive(R"({"type":"rename","rid":"m4","agent":"claude","id":")" + id + R"(","title":""})");
  got = poll_all(c);
  check(has(got, R"("rid":"m4","ok":true)") && has(got, R"("title":"draw the loss curve")"),
        "an empty name gives the chat its own title back");
  c.receive(R"({"type":"archive","rid":"m5","agent":"claude","id":"no-such-chat","on":true})");
  got = poll_all(c);
  check(has(got, R"("rid":"m5","ok":false)") && has(got, "no chat"), "a chat the listing does not name cannot be marked");
  c.receive(R"({"type":"rename","rid":"m6","agent":"","id":"","title":"x"})");
  check(has(poll_all(c), R"("rid":"m6","ok":false)"), "nor one with no agent and id");

  // --- what a client may not do ---------------------------------------------
  c.receive(R"({"type":"open","path":"/etc/passwd"})");
  got = poll_all(c);
  check(has(got, R"("type":"error")") && !has(got, "root:"), "a path no chat lives at is refused");
  c.receive(R"({"type":"start","rid":"r1","agent":"sh","cwd":")" + dir + "\"}");
  got = poll_all(c);
  check(has(got, R"("rid":"r1","ok":false)") && has(got, "no adapter"), "only agents mico has an adapter for start");
  c.receive(R"({"type":"start","rid":"r2","agent":"claude","cwd":"/"})");
  got = poll_all(c);
  check(has(got, R"("rid":"r2","ok":false)") && has(got, "not a tracked folder"), "only in tracked folders");
  c.receive(R"({"type":"send","rid":7,"key":999,"text":"hi"})");
  got = poll_all(c);
  check(has(got, R"("rid":"7","ok":false)"), "no message goes to an agent that does not exist");
  c.receive("not json");
  check(has(poll_all(c), R"("type":"error")"), "a malformed message is answered with an error");
  c.receive(R"({"type":"close","path":")" + path + "\"}");
  append(path, R"({"type":"user","cwd":")" + dir + R"(","message":{"role":"user","content":"after close"}})");
  check(!has(poll_all(c), "after close"), "a closed chat is no longer followed");

  // --- a session no front end sizes still starts ------------------------------
  {
    LiveSession* s = ws.start_command({"/bin/cat"}, dir);
    check(s && !s->spawned(), "a new session waits for a front end to size it");
    for (int i = 0; i < 40 && s && !s->spawned(); i++) {
      ws.service(false);
      usleep(50 * 1000);
    }
    check(s && s->spawned() && !s->exited(), "one that nothing sizes is launched at a default size");
    if (s) {
      // Pictures a browser sends are saved where a pasted one is, by what
      // their bytes are; anything else is refused before the agent sees it.
      const std::string runtime = dir + "/runtime";
      mkdir(runtime.c_str(), 0700);
      const char* was = getenv("XDG_RUNTIME_DIR");
      const std::string saved_rt = was ? was : "";
      setenv("XDG_RUNTIME_DIR", runtime.c_str(), 1);
      const std::string key = std::to_string(s->serial());
      const std::string png =
          "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==";
      c.receive(R"({"type":"send","rid":"i1","key":)" + key + R"(,"text":"","images":["aGVsbG8="]})");
      got = poll_all(c);
      check(has(got, R"("rid":"i1","ok":false)") && has(got, "not a PNG"), "a send's image must be a picture");
      c.receive(R"({"type":"send","rid":"i2","key":)" + key + R"(,"text":"look","images":[")" + png + R"("]})");
      got = poll_all(c);
      check(has(got, R"("rid":"i2","ok":true)"), "a message with a picture is sent");
      std::string listing;
      if (DIR* d = opendir((runtime + "/mico/images").c_str())) {
        while (dirent* e = readdir(d)) if (e->d_name[0] != '.') listing += e->d_name;
        closedir(d);
      }
      check(listing.starts_with("paste-") && listing.ends_with(".png"), "the picture is saved as a PNG for the agent");
      c.receive(R"({"type":"send","rid":"i3","key":)" + key + R"(,"text":"","images":[]})");
      check(has(poll_all(c), R"("rid":"i3","ok":false)"), "an empty message is still refused");
      if (was) setenv("XDG_RUNTIME_DIR", saved_rt.c_str(), 1);
      else unsetenv("XDG_RUNTIME_DIR");
      std::filesystem::remove_all(runtime);
      ws.close(s);
      ws.reap([](LiveSession*) {});
    }
  }

  unlink(path.c_str());
  rmdir(dir.c_str());
  if (g_failures == 0) printf("api: all checks passed\n");
  return g_failures == 0 ? 0 : 1;
}

}  // namespace mico
