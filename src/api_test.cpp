// `mico-test --api-test`: the state protocol against a workspace over fixture
// transcripts. Runs under the test HOME ctest sets up; starts no agent.
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "api/client.h"
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
