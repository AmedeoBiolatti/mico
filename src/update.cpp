// `mico update`: replaces this binary with the latest GitHub release's, after
// checking the download against the release's SHA256SUMS. It runs curl, tar and
// sha256sum rather than linking an HTTP stack, and never goes through a shell.
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef MICO_VERSION
#define MICO_VERSION "unknown"
#endif

namespace mico {
namespace {

constexpr const char* kRepo = "AmedeoBiolatti/mico";

// Runs argv (looked up on PATH). Output goes to `out` when given, else is
// discarded; stderr stays visible. Returns the exit status, or -1.
int run(const std::vector<std::string>& args, std::string* out = nullptr, const std::string& cwd = "") {
  int pipefd[2] = {-1, -1};
  if (out && pipe(pipefd) < 0) return -1;
  const pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    if (!cwd.empty() && chdir(cwd.c_str()) < 0) _exit(127);
    if (out) {
      dup2(pipefd[1], STDOUT_FILENO);
      close(pipefd[0]);
      close(pipefd[1]);
    } else if (FILE* nul = fopen("/dev/null", "w")) {
      dup2(fileno(nul), STDOUT_FILENO);
    }
    std::vector<char*> av;
    for (const auto& a : args) av.push_back(const_cast<char*>(a.c_str()));
    av.push_back(nullptr);
    execvp(av[0], av.data());
    _exit(127);
  }
  if (out) {
    close(pipefd[1]);
    char buf[4096];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof buf)) > 0) out->append(buf, size_t(n));
    close(pipefd[0]);
  }
  int st = 0;
  if (waitpid(pid, &st, 0) < 0) return -1;
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

bool have(const char* tool) { return run({"sh", "-c", std::string("command -v ") + tool}) == 0; }

// "v0.1.2" or "0.1.2" -> {0,1,2}.
std::vector<int> parse_version(std::string v) {
  if (!v.empty() && v[0] == 'v') v.erase(0, 1);
  std::vector<int> parts;
  std::stringstream ss(v);
  std::string p;
  while (std::getline(ss, p, '.')) parts.push_back(atoi(p.c_str()));
  return parts;
}

// The tag name out of the releases API's JSON: no JSON parser needed for one string.
std::string tag_of(const std::string& json) {
  const size_t k = json.find("\"tag_name\"");
  if (k == std::string::npos) return {};
  const size_t a = json.find('"', json.find(':', k) + 1);
  if (a == std::string::npos) return {};
  const size_t b = json.find('"', a + 1);
  return b == std::string::npos ? std::string() : json.substr(a + 1, b - a - 1);
}

std::string self_path() {
  char buf[4096];
  const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n <= 0) return {};
  buf[n] = 0;
  std::string p = buf;
  // A binary replaced while running reads back as "... (deleted)".
  if (p.size() > 10 && p.compare(p.size() - 10, 10, " (deleted)") == 0) p.resize(p.size() - 10);
  return p;
}

}  // namespace

int run_update(bool check_only) {
  if (!have("curl")) {
    fprintf(stderr, "mico update: needs curl\n");
    return 1;
  }
  std::string json;
  const std::string api = std::string("https://api.github.com/repos/") + kRepo + "/releases/latest";
  if (run({"curl", "-fsSL", "-H", "Accept: application/vnd.github+json", api}, &json) != 0) {
    fprintf(stderr, "mico update: could not reach GitHub for the latest release\n");
    return 1;
  }
  const std::string tag = tag_of(json);
  if (tag.empty()) {
    fprintf(stderr, "mico update: the latest release has no tag\n");
    return 1;
  }
  const std::vector<int> have_v = parse_version(MICO_VERSION), latest = parse_version(tag);
  if (have_v >= latest) {
    printf("mico %s is up to date (latest release: %s)\n", MICO_VERSION, tag.c_str());
    return 0;
  }
  printf("mico %s -> %s\n", MICO_VERSION, tag.c_str());
  if (check_only) {
    printf("run 'mico update' to install it\n");
    return 0;
  }

  utsname u{};
  uname(&u);
  const std::string arch = u.machine;
  if (arch != "x86_64" && arch != "aarch64") {
    fprintf(stderr, "mico update: no release binary for %s\n", arch.c_str());
    return 1;
  }
  const std::string self = self_path();
  if (self.empty() || access(self.c_str(), W_OK) != 0 || access((self.substr(0, self.rfind('/')) + "/.").c_str(), W_OK) != 0) {
    fprintf(stderr, "mico update: cannot write %s; mico never uses sudo\n", self.c_str());
    return 1;
  }
  if (self.find("/build/") != std::string::npos)
    fprintf(stderr, "mico update: note: replacing a build-folder binary (%s)\n", self.c_str());

  char tmpl[] = "/tmp/mico-update-XXXXXX";
  if (!mkdtemp(tmpl)) {
    perror("mico update: mkdtemp");
    return 1;
  }
  const std::string dir = tmpl;
  const auto cleanup = [&] { run({"rm", "-rf", dir}); };
  const std::string name = "mico-" + tag + "-linux-" + arch;
  const std::string base = std::string("https://github.com/") + kRepo + "/releases/download/" + tag + "/";
  for (const std::string& f : {name + ".tar.gz", std::string("SHA256SUMS")}) {
    if (run({"curl", "-fSL", "--progress-bar", "-o", dir + "/" + f, base + f}) != 0) {
      fprintf(stderr, "mico update: could not download %s\n", f.c_str());
      cleanup();
      return 1;
    }
  }
  if (!have("sha256sum") || run({"sha256sum", "-c", "--ignore-missing", "--status", "SHA256SUMS"}, nullptr, dir) != 0) {
    fprintf(stderr, "mico update: the download does not match SHA256SUMS; nothing was changed\n");
    cleanup();
    return 1;
  }
  if (run({"tar", "-xzf", name + ".tar.gz"}, nullptr, dir) != 0) {
    fprintf(stderr, "mico update: could not unpack the download\n");
    cleanup();
    return 1;
  }
  // Copy beside the running binary, then rename over it: atomic, and fine for
  // a binary that is running (the daemon keeps its old copy until restarted).
  const std::string next = self + ".new";
  if (run({"cp", dir + "/" + name + "/bin/mico", next}) != 0 || chmod(next.c_str(), 0755) != 0 ||
      rename(next.c_str(), self.c_str()) != 0) {
    perror("mico update: install");
    unlink(next.c_str());
    cleanup();
    return 1;
  }
  cleanup();
  printf("updated %s to %s\n", self.c_str(), tag.c_str());
  printf("a running daemon keeps the old copy; run 'mico kill' when ready, and the next 'mico' starts the new one\n");
  printf("(the download's checksum was verified; to also verify its provenance: gh attestation verify)\n");
  return 0;
}

const char* version() { return MICO_VERSION; }

}  // namespace mico
