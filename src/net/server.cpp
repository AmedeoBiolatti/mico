#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

#include "net/proto.h"
#include "term/encoder.h"
#include "term/input.h"
#include "base/log.h"
#include "base/process.h"
#include "term/kitty.h"
#include "math/math.h"
#include "term/sixel.h"
#include "term/term.h"
#include "net/web.h"
#include "core/away.h"
#include "core/opener.h"
#include "ui/app.h"
#include "ui/theme.h"

namespace mico {
namespace {

// Why the loop ends: a signal's number, or one of these.
volatile sig_atomic_t g_stop = 0;
constexpr int kStopKill = 1000, kStopQuit = 1001;
void on_term(int sig) { g_stop = sig; }

// One attached terminal. Each keeps its own front buffer, so a client that
// joins late gets a full repaint without disturbing anyone already attached.
struct Client {
  int fd = -1;
  std::string in;   // undecoded protocol bytes
  std::string out;  // pending bytes to write
  InputDecoder dec;
  std::chrono::steady_clock::time_point escape_since{};
  Surface front;
  int w = 0, h = 0;
  bool hello = false;
  bool needs_full = true;
  bool set_background = true;  // tell the terminal the theme's background
  bool dead = false;
  bool mouse_on = true;   // what this terminal was last told
  bool mouse_any = false; // 1003 (any-event) vs 1002 (button-event) tracking
  // The window's focus, once the terminal has reported it (mode 1004).
  bool focused = true, focus_known = false;
  GfxCaps caps{};
  math::KittyHeld images;  // the images this terminal already holds
};

bool set_nonblock(int fd) {
  int fl = fcntl(fd, F_GETFL, 0);
  return fl >= 0 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0;
}


// A socket file left behind by a crashed daemon would block binding forever.
// If nothing answers on it, it is stale and safe to remove.
bool socket_is_live(const std::string& path) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return false;
  sockaddr_un addr;
  bool live = proto::socket_addr(path, &addr) && connect(fd, (sockaddr*)&addr, sizeof addr) == 0;
  close(fd);
  return live;
}

int listen_socket(const std::string& path) {
  if (socket_is_live(path)) {
    fprintf(stderr, "mico: a daemon is already running at %s\n", path.c_str());
    return -1;
  }
  unlink(path.c_str());

  sockaddr_un addr;
  if (!proto::socket_addr(path, &addr)) {
    MLOG("socket path too long for a unix socket: %s", path.c_str());
    return -1;
  }
  // Close-on-exec, as every descriptor of the daemon's: an agent, or a
  // browser it opens, must not keep the daemon's sockets past its exit.
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  if (bind(fd, (sockaddr*)&addr, sizeof addr) != 0 || listen(fd, 8) != 0) {
    MLOG("cannot listen on %s: %s", path.c_str(), strerror(errno));
    close(fd);
    return -1;
  }
  chmod(path.c_str(), 0600);
  set_nonblock(fd);
  return fd;
}

// $XDG_STATE_HOME/mico/daemon: "pid started last_alive", unix seconds. There
// while a daemon runs; removed when it stops as it should.
std::string pidfile_path() { return state_dir() + "/daemon"; }

void write_pidfile(int64_t started) {
  char b[96];
  const int n = snprintf(b, sizeof b, "%d %lld %lld\n", int(getpid()), (long long)started, (long long)time(nullptr));
  if (n > 0) write_file_atomic(pidfile_path(), std::string_view(b, size_t(n)));
}

std::string clock_of(int64_t t) {
  const time_t tt = time_t(t);
  tm v{};
  localtime_r(&tt, &v);
  char b[32];
  strftime(b, sizeof b, "%b %d %H:%M:%S", &v);
  return b;
}

// What a command prints, up to `cap` bytes and ten seconds; empty when it
// cannot run.
std::string capture(const std::vector<std::string>& argv, size_t cap) {
  proc::Options opt;
  opt.cap = cap;
  return proc::capture(argv, opt).out;
}

// What became of the last daemon, found on a thread of its own: the
// journal can take a moment, and the new daemon has agents to start.
struct Postmortem {
  std::mutex mu;
  bool done = false;
  std::string status;  // for the status bar; empty when there is nothing to say
};

std::shared_ptr<Postmortem> check_last_daemon() {
  std::string buf;
  int pid = 0;
  long long started = 0, alive = 0;
  if (FILE* f = fopen(pidfile_path().c_str(), "r")) {
    if (fscanf(f, "%d %lld %lld", &pid, &started, &alive) < 2) pid = 0;
    fclose(f);
  }
  if (pid <= 0) return nullptr;
  // A file left behind is the mark of a daemon that did not stop as it
  // should. One still running would hold the socket: we would not be here.
  MLOG("the last daemon (pid %d, started %s, last seen %s) ended without stopping: killed, or crashed",
       pid, clock_of(started).c_str(), clock_of(alive ? alive : started).c_str());
  auto pm = std::make_shared<Postmortem>();
  std::thread([pm, pid, started] {
    const std::string since = "@" + std::to_string(started);
    std::string journal = capture({"journalctl", "--no-pager", "-q", "-o", "short-iso", "--since", since,
                                   "-g", "mico|oom|Killed"},
                                  8u << 20);
    // A journalctl without pattern matching (-g) prints nothing: read it all.
    if (journal.empty())
      journal = capture({"journalctl", "--no-pager", "-q", "-o", "short-iso", "--since", since}, 64u << 20);
    const std::string me = " " + std::to_string(pid) + " (mico)";
    const std::string killed = "Killed process " + std::to_string(pid) + " ";
    // The lines about it, then the lines about the unit it was killed with.
    std::string unit, when, how;
    std::vector<std::string> lines;
    size_t at = 0;
    while (at < journal.size()) {
      size_t e = journal.find('\n', at);
      if (e == std::string::npos) e = journal.size();
      const std::string l = journal.substr(at, e - at);
      at = e + 1;
      if (l.find(me) == std::string::npos && l.find(killed) == std::string::npos) continue;
      lines.push_back(l);
      if (when.empty()) when = l.substr(0, l.find(' '));
      // "…systemd[3166]: app-gnome-kitty-55828.scope: Killing process …"
      const size_t k = l.find(": Killing process");
      if (k != std::string::npos && unit.empty()) {
        const size_t s = l.rfind(' ', k);
        unit = l.substr(s + 1, k - s - 1);
      }
      if (l.find("Killed process") != std::string::npos) how = "the kernel's OOM killer";
    }
    if (!unit.empty()) {
      at = 0;
      while (at < journal.size()) {
        size_t e = journal.find('\n', at);
        if (e == std::string::npos) e = journal.size();
        const std::string l = journal.substr(at, e - at);
        at = e + 1;
        if (l.find(unit) == std::string::npos || std::find(lines.begin(), lines.end(), l) != lines.end()) continue;
        lines.push_back(l);
        if (l.find("systemd-oomd") != std::string::npos || l.find("oom-kill") != std::string::npos)
          how = "systemd-oomd, for memory pressure, with everything in " + unit;
      }
    }
    for (size_t i = 0; i < lines.size() && i < 16; i++) MLOG("the last daemon, from the journal: %s", lines[i].c_str());
    std::string status;
    const std::string t = when.size() >= 19 ? when.substr(11, 8) : when;
    if (!how.empty()) status = "the last daemon was killed at " + t + " by " + how + " (see :log)";
    else if (!lines.empty()) status = "the last daemon was killed at " + t + " (see :log)";
    else {
      MLOG("the last daemon: the journal says nothing of its end; a crash would be logged above it");
      status = "the last daemon ended without stopping (see :log)";
    }
    std::lock_guard<std::mutex> lock(pm->mu);
    pm->status = std::move(status);
    pm->done = true;
  }).detach();
  return pm;
}

void flush_client(Client& c) {
  while (!c.out.empty()) {
    ssize_t n = write(c.fd, c.out.data(), c.out.size());
    if (n > 0) { c.out.erase(0, size_t(n)); continue; }
    if (n < 0 && errno == EINTR) continue;
    break;  // EAGAIN: the rest goes out on the next POLLOUT
  }
}

}  // namespace

int run_daemon() {
  const std::string dir = proto::socket_dir();
  if (!proto::private_dir(dir, true)) {
    MLOG("refusing socket dir %s: not a private directory owned by this user", dir.c_str());
    return 1;
  }
  const std::string path = proto::socket_path();

  int lfd = listen_socket(path);
  if (lfd < 0) return 1;
  MLOG("daemon listening on %s", path.c_str());

  signal(SIGPIPE, SIG_IGN);
  signal(SIGTERM, on_term);
  signal(SIGINT, on_term);
  signal(SIGHUP, on_term);
  // Whether the last daemon stopped as it should, and if not, what the
  // journal says ended it: an out-of-memory kill takes no log line with it.
  std::shared_ptr<Postmortem> postmortem = check_last_daemon();
  write_pidfile(time(nullptr));

  // The daemon owns the agents and what is known about them; the App is how
  // its clients see them. Today every client shares that one view.
  Workspace workspace;
  // Agents the last daemon was running and did not stop on purpose come back:
  // after a reboot, a crash, or `mico kill`. Then this daemon keeps the list.
  const int restored = restore_agents_enabled() ? workspace.restore_running() : 0;
  workspace.remember_running();
  App app(workspace);
  if (restored)
    app.set_status("Resumed " + std::to_string(restored) + " agent" + (restored == 1 ? "" : "s") +
                   " that " + (restored == 1 ? "was" : "were") + " running when mico stopped");
  // The web view, when it is turned on (:web on): a second kind of client,
  // given state rather than frames.
  WebServer web(workspace);
  Surface back;
  std::vector<std::unique_ptr<Client>> clients;
  std::vector<pollfd> fds;
  std::vector<int> session_fds;
  int last_w = 0, last_h = 0;

  std::vector<uint32_t> evicted;
  Color theme_bg = active_theme().bg;
  const int64_t started = time(nullptr);
  int64_t alive_written = started;
  bool postmortem_shown = false;
  while (!g_stop && app.running()) {
    bool dirty = app.service();
    // Alive, once a minute: a daemon that is killed leaves this behind, and
    // the next one can say when it was last seen.
    if (const int64_t now = time(nullptr); now - alive_written >= 60) {
      alive_written = now;
      write_pidfile(started);
    }
    if (postmortem && !postmortem_shown) {
      std::lock_guard<std::mutex> lock(postmortem->mu);
      if (postmortem->done) {
        postmortem_shown = true;
        if (!postmortem->status.empty()) app.set_status(postmortem->status);
      }
    }
    app.save_view_if_changed();

    // An agent finished, or needs you. Nobody is told while someone is
    // looking at mico; with no terminal attached, the desktop is.
    if (auto notices = app.take_notices(); !notices.empty()) {
      bool attached = false, watched = false;
      for (const auto& c : clients) {
        attached |= c->hello;
        watched |= c->hello && c->focus_known && c->focused;
      }
      for (const auto& n : notices) {
        if (!attached) {
          if (notify_mode() == NotifyMode::Desktop) notify_desktop(n.title, n.body);
          continue;
        }
        if (watched) continue;
        for (auto& c : clients) {
          if (!c->hello || !App::announce(n, c->focus_known, c->focused)) continue;
          proto::encode(proto::Type::Frame, App::notice_seq(n, c->caps.notify), c->out);
          flush_client(*c);
        }
      }
    }
    web.sync();
    web.pump();

    // Equations are drawn as images only when every attached terminal can
    // show them: the layout is shared, and a placeholder means nothing to a
    // terminal without the protocol. The first such terminal's cell size
    // decides the image size.
    {
      math::Config mc = math::config();
      bool any = false, all = true;
      int cw = 0, ch = 0;
      for (const auto& c : clients) {
        if (!c->hello) continue;
        any = true;
        if (!c->caps.any()) all = false;
        else if (!cw && c->caps.cell_w > 0) { cw = c->caps.cell_w; ch = c->caps.cell_h; }
      }
      // kitty scales a picture to its cells; sixel paints exact pixels, so a
      // sixel terminal with other-sized cells would get misfit pictures.
      for (const auto& c : clients)
        if (c->hello && c->caps.sixel && (c->caps.cell_w != cw || c->caps.cell_h != ch)) all = false;
      mc.enabled = any && all;
      mc.kitty = mc.enabled;
      for (const auto& c : clients)
        if (c->hello && !c->caps.kitty) mc.kitty = false;
      if (cw > 0) { mc.cell_w = cw; mc.cell_h = ch; }
      mc.fg = active_theme().math;
      const uint64_t gen = math::generation();
      math::configure(mc);
      if (math::generation() != gen) dirty = true;
    }

    // A new theme: every terminal is given its background and repainted.
    if (const Color bg = active_theme().bg; bg != theme_bg) {
      theme_bg = bg;
      for (auto& c : clients) {
        c->set_background = true;
        c->needs_full = true;
      }
      dirty = true;
    }

    // Every attached terminal sees the same layout, so the surface is sized to
    // the smallest of them — the same bargain tmux makes.
    int w = 0, h = 0;
    for (const auto& c : clients) {
      if (!c->hello) continue;
      w = w ? std::min(w, c->w) : c->w;
      h = h ? std::min(h, c->h) : c->h;
    }

    bool anyone_needs_full = false;
    for (const auto& c : clients)
      if (c->hello && c->needs_full) anyone_needs_full = true;

    if (w > 0 && h > 0 && (dirty || anyone_needs_full)) {
      if (w != last_w || h != last_h) {
        last_w = w;
        last_h = h;
        for (auto& c : clients) c->needs_full = true;
      }
      back.resize(w, h);
      app.draw(back);

      evicted.clear();
      math::take_evicted(evicted);
      const bool images = math::config().enabled;
      const std::string clip = app.take_clipboard();
      if (app.take_redraw())
        for (auto& c : clients) c->needs_full = true;
      for (auto& c : clients) {
        if (!c->hello) continue;
        std::string frame;

        // Hand the mouse back to the terminal, or take it again. Also switch
        // to any-event tracking while a popup menu is open, so hovering a menu
        // item with no button held still moves the highlight.
        if (app.wants_mouse() != c->mouse_on || app.wants_motion() != c->mouse_any) {
          c->mouse_on = app.wants_mouse();
          c->mouse_any = app.wants_motion();
          frame += mouse_mode_seq(c->mouse_on, c->mouse_any);
          c->needs_full = true;  // repaint once, then hold still
        }
        if (!clip.empty()) frame += clipboard_seq(clip);
        if (c->set_background) {
          frame += tty::background_seq(theme_bg);
          c->set_background = false;
        }

        // While selecting, the screen must not move: a repaint clears the
        // terminal's own selection out from under the drag.
        // Image data goes ahead of the cells that show it, in messages of its
        // own: a screenful of equations can outgrow one frame's payload.
        std::string pictures;
        if (!evicted.empty()) math::free_images(evicted, c->images, pictures, c->caps.tmux);
        if (!app.selection_mode() || c->needs_full) {
          const ImageMode mode = !images ? ImageMode::None
                                 : c->caps.kitty ? ImageMode::Kitty
                                 : c->caps.sixel ? ImageMode::Sixel
                                                 : ImageMode::None;
          if (mode == ImageMode::Kitty) math::send_images(back, c->images, pictures, c->caps.tmux);
          encode_frame(back, c->front, frame, c->needs_full, mode);
          if (mode == ImageMode::Sixel) math::sixel_pass(back, c->front, frame);
        }
        constexpr size_t kPart = 4u << 20;
        for (size_t off = 0; off < pictures.size(); off += kPart)
          proto::encode(proto::Type::Frame, std::string_view(pictures).substr(off, kPart), c->out);

        if (!frame.empty()) proto::encode(proto::Type::Frame, frame, c->out);
        c->needs_full = false;
        flush_client(*c);
      }
    }

    fds.clear();
    fds.push_back(pollfd{lfd, POLLIN, 0});
    // Remember how many client slots were polled: accept() below can extend
    // `clients`, and those newcomers have no entry in `fds` this round.
    for (auto& c : clients)
      fds.push_back(pollfd{c->fd, short(POLLIN | (c->out.empty() ? 0 : POLLOUT)), 0});
    const size_t polled = clients.size();
    session_fds.clear();
    app.collect_session_fds(session_fds);
    for (int fd : session_fds) fds.push_back(pollfd{fd, POLLIN, 0});
    web.add_fds(fds);

    // With nobody watching there is nothing to draw, so idle cheaply; agents
    // keep running either way, which is the entire point of the daemon.
    // Agent output and client input wake poll by themselves. The timeout only
    // covers work the clock moves, so an idle daemon sleeps for a second at a
    // time instead of waking 30 times a second to find nothing to do.
    int timeout = app.idle_timeout_ms();
    for (auto& c : clients)
      if (c->dec.pending_escape()) timeout = std::min(timeout, 25);
    if (::poll(fds.data(), fds.size(), timeout) < 0 && errno != EINTR) break;

    web.handle(fds);

    if (fds[0].revents & POLLIN) {
      for (;;) {
        int cfd = accept4(lfd, nullptr, nullptr, SOCK_CLOEXEC);
        if (cfd < 0) break;
        set_nonblock(cfd);
        auto c = std::make_unique<Client>();
        c->fd = cfd;
        clients.push_back(std::move(c));
        MLOG("client connected (fd %d), %zu attached", cfd, clients.size());
      }
    }

    for (size_t i = 0; i < polled && i < clients.size(); i++) {
      Client& c = *clients[i];
      const short re = fds[i + 1].revents;
      if (re & (POLLHUP | POLLERR | POLLNVAL)) c.dead = true;

      if (re & POLLOUT) flush_client(c);

      if (re & POLLIN) {
        char buf[16384];
        for (;;) {
          ssize_t n = read(c.fd, buf, sizeof buf);
          if (n > 0) { c.in.append(buf, size_t(n)); continue; }
          if (n == 0) c.dead = true;
          else if (errno == EINTR) continue;
          break;
        }

        proto::Type t;
        std::string payload;
        while (proto::decode(c.in, &t, &payload)) {
          switch (t) {
            case proto::Type::Hello:
              if (proto::decode_size(payload, &c.w, &c.h)) {
                c.hello = true;
                c.needs_full = true;
                proto::decode_caps(payload, &c.caps);
              }
              break;
            case proto::Type::Resize:
              if (proto::decode_size(payload, &c.w, &c.h)) {
                c.needs_full = true;
                proto::decode_caps(payload, &c.caps);
              }
              break;
            case proto::Type::Input:
              // The client never decodes; it forwards what its terminal
              // produced and the daemon interprets it exactly as if local.
              c.dec.feed(payload);
              while (auto ev = c.dec.next()) {
                // Focus is this terminal's alone, not something to act on.
                if (ev->type == InputEvent::Type::Focus) {
                  c.focused = ev->focus_in;
                  c.focus_known = true;
                  continue;
                }
                switch (app.feed(*ev)) {
                  // Only this client leaves; its agents carry on for everyone
                  // else, and for whoever attaches next.
                  case AppAction::Detach: c.dead = true; break;
                  case AppAction::Shutdown: c.dead = true; g_stop = kStopQuit; break;
                  default: break;
                }
                // A link this client's user clicked opens on their machine,
                // which is where the client runs, not necessarily the daemon.
                if (std::string url = app.take_open_url(); !url.empty())
                  proto::encode(proto::Type::OpenUrl, url, c.out);
              }
              c.escape_since = std::chrono::steady_clock::now();
              break;
            case proto::Type::Bye: c.dead = true; break;
            // Detaching leaves the agents running; only an explicit Kill ends them.
            case proto::Type::Kill: c.dead = true; g_stop = kStopKill; break;
            default: break;
          }
        }
      }

    }

    // Process readable input before resolving a lone ESC. Flushing before
    // read() split a bracketed paste or arrow sequence at socket boundaries.
    // Use elapsed time so output from a busy agent cannot starve Escape.
    const auto now = std::chrono::steady_clock::now();
    for (auto& c : clients) {
      if (c->dead || !c->dec.pending_escape() ||
          now - c->escape_since < std::chrono::milliseconds(25)) continue;
      if (auto ev = c->dec.flush()) {
        const auto action = app.feed(*ev);
        if (action != AppAction::None) c->dead = true;
        if (action == AppAction::Shutdown) g_stop = kStopQuit;
      }
    }

    for (size_t i = 0; i < clients.size();) {
      if (clients[i]->dead) {
        MLOG("client fd %d gone, %zu left", clients[i]->fd, clients.size() - 1);
        close(clients[i]->fd);
        clients.erase(clients.begin() + long(i));
        continue;
      }
      i++;
    }
  }

  // Stopped on purpose (:quit, "Stop all agents and quit"): the agents go
  // for good, and the next daemon starts none of them.
  if (!app.running()) workspace.forget_running();
  const int why = g_stop;
  MLOG("daemon stopping: %s, with %zu agents",
       !app.running() || why == kStopQuit ? ":quit" : why == kStopKill ? "mico kill" : why == SIGTERM ? "SIGTERM"
       : why == SIGINT ? "SIGINT" : why == SIGHUP ? "SIGHUP" : "the loop ended",
       workspace.live().size());
  unlink(pidfile_path().c_str());
  for (auto& c : clients) {
    proto::encode(proto::Type::Detach, {}, c->out);
    flush_client(*c);
    close(c->fd);
  }
  close(lfd);
  unlink(path.c_str());
  return 0;
}

}  // namespace mico
