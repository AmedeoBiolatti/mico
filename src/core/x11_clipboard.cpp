#include "core/x11_clipboard.h"

#include <dlfcn.h>

#include <cstdlib>
#include <cstring>
#include <type_traits>

#include "base/log.h"

namespace mico::x11clip {
namespace {

// The few Xlib types and calls used here, declared by hand so neither the
// headers nor the library are needed to build. Layouts are Xlib's on LP64.
using Display = void;
using Window = unsigned long;
using Atom = unsigned long;
using Time = unsigned long;
using Bool = int;

struct SelectionRequest {  // XSelectionRequestEvent
  int type;
  unsigned long serial;
  Bool send_event;
  Display* display;
  Window owner, requestor;
  Atom selection, target, property;
  Time time;
};
struct SelectionNotify {  // XSelectionEvent
  int type;
  unsigned long serial;
  Bool send_event;
  Display* display;
  Window requestor;
  Atom selection, target, property;
  Time time;
};
struct SelectionClear {  // XSelectionClearEvent
  int type;
  unsigned long serial;
  Bool send_event;
  Display* display;
  Window window;
  Atom selection;
  Time time;
};
union XEvent {
  int type;
  SelectionRequest request;
  SelectionNotify notify;
  SelectionClear clear;
  long pad[24];
};
constexpr int kSelectionClear = 29, kSelectionRequest = 30, kSelectionNotify = 31;
constexpr Atom kAtomAtom = 4, kAtomString = 31;
constexpr int kPropReplace = 0;

struct Xlib {
  Display* (*OpenDisplay)(const char*);
  Window (*DefaultRootWindow)(Display*);
  Window (*CreateSimpleWindow)(Display*, Window, int, int, unsigned, unsigned, unsigned,
                               unsigned long, unsigned long);
  Atom (*InternAtom)(Display*, const char*, Bool);
  int (*SetSelectionOwner)(Display*, Atom, Window, Time);
  Window (*GetSelectionOwner)(Display*, Atom);
  int (*ConnectionNumber)(Display*);
  int (*Pending)(Display*);
  int (*NextEvent)(Display*, XEvent*);
  int (*ChangeProperty)(Display*, Window, Atom, Atom, int, int, const unsigned char*, int);
  int (*SendEvent)(Display*, Window, Bool, long, XEvent*);
  int (*Flush)(Display*);
  long (*ExtendedMaxRequestSize)(Display*);
  long (*MaxRequestSize)(Display*);
  void* (*SetErrorHandler)(int (*)(Display*, void*));
  void* (*SetIOErrorHandler)(int (*)(Display*));
  void* (*SetIOErrorExitHandler)(Display*, void (*)(Display*, void*), void*);  // libX11 >= 1.7
};

struct State {
  bool tried = false;
  bool dead = false;  // the connection is gone; never touch it again
  Xlib x{};
  Display* dpy = nullptr;
  Window win = 0;
  Atom clipboard = 0, primary = 1, targets = 0, utf8 = 0, text = 0, plain = 0;
  size_t max_bytes = 0;
  std::string data;
  bool owned = false;
};
State& st() {
  static State s;
  return s;
}

// Xlib's default handlers print and exit the process. For the daemon that
// would mean every running agent dying because a pasting window vanished
// mid-request (an ordinary BadWindow), or because the X server went away.
int ignore_error(Display*, void*) { return 0; }
// Returning from here, rather than exiting as the default handler does, is
// what lets Xlib go on to io_gone() below instead of ending the process.
int io_error(Display*) { return 0; }
void io_gone(Display*, void*) {
  st().dead = true;
  st().owned = false;
  MLOG("x11 clipboard: display connection lost");
}

bool open_display() {
  State& s = st();
  if (s.tried) return s.dpy != nullptr && !s.dead;
  s.tried = true;
  if (!getenv("DISPLAY")) return false;
  void* lib = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
  if (!lib) return false;
  const auto sym = [&](auto& fn, const char* name) {
    fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name));
    return fn != nullptr;
  };
  Xlib& x = s.x;
  const bool ok = sym(x.OpenDisplay, "XOpenDisplay") && sym(x.DefaultRootWindow, "XDefaultRootWindow") &&
      sym(x.CreateSimpleWindow, "XCreateSimpleWindow") && sym(x.InternAtom, "XInternAtom") &&
      sym(x.SetSelectionOwner, "XSetSelectionOwner") && sym(x.GetSelectionOwner, "XGetSelectionOwner") &&
      sym(x.ConnectionNumber, "XConnectionNumber") && sym(x.Pending, "XPending") &&
      sym(x.NextEvent, "XNextEvent") && sym(x.ChangeProperty, "XChangeProperty") &&
      sym(x.SendEvent, "XSendEvent") && sym(x.Flush, "XFlush") &&
      sym(x.MaxRequestSize, "XMaxRequestSize") && sym(x.SetErrorHandler, "XSetErrorHandler") &&
      sym(x.SetIOErrorHandler, "XSetIOErrorHandler");
  sym(x.ExtendedMaxRequestSize, "XExtendedMaxRequestSize");
  sym(x.SetIOErrorExitHandler, "XSetIOErrorExitHandler");
  if (!ok) {
    MLOG("x11 clipboard: libX11 lacks a needed symbol");
    return false;
  }
  // Without a way to survive a lost connection, Xlib would exit the daemon:
  // better to have no clipboard than that.
  if (!x.SetIOErrorExitHandler) {
    MLOG("x11 clipboard: libX11 too old to recover from a lost display; not used");
    return false;
  }
  x.SetErrorHandler(ignore_error);
  x.SetIOErrorHandler(io_error);
  s.dpy = x.OpenDisplay(nullptr);
  if (!s.dpy) {
    MLOG("x11 clipboard: cannot open display %s", getenv("DISPLAY"));
    return false;
  }
  x.SetIOErrorExitHandler(s.dpy, io_gone, nullptr);
  s.win = x.CreateSimpleWindow(s.dpy, x.DefaultRootWindow(s.dpy), 0, 0, 1, 1, 0, 0, 0);
  s.clipboard = x.InternAtom(s.dpy, "CLIPBOARD", 0);
  s.targets = x.InternAtom(s.dpy, "TARGETS", 0);
  s.utf8 = x.InternAtom(s.dpy, "UTF8_STRING", 0);
  s.text = x.InternAtom(s.dpy, "TEXT", 0);
  s.plain = x.InternAtom(s.dpy, "text/plain;charset=utf-8", 0);
  // The largest property one request can carry, in bytes, less headroom for
  // the request header. Bigger copies would need the INCR protocol.
  long units = x.ExtendedMaxRequestSize ? x.ExtendedMaxRequestSize(s.dpy) : 0;
  if (units <= 0) units = x.MaxRequestSize(s.dpy);
  s.max_bytes = size_t(units) * 4 - 256;
  MLOG("x11 clipboard: ready on %s", getenv("DISPLAY"));
  return true;
}

void answer(const SelectionRequest& r) {
  State& s = st();
  XEvent reply{};
  reply.notify.type = kSelectionNotify;
  reply.notify.display = r.display;
  reply.notify.requestor = r.requestor;
  reply.notify.selection = r.selection;
  reply.notify.target = r.target;
  reply.notify.time = r.time;
  reply.notify.property = 0;  // None: refused, unless set below
  // Old clients leave the property as None and mean "use the target's name".
  const Atom prop = r.property ? r.property : r.target;
  if (s.owned && r.target == s.targets) {
    const Atom list[] = {s.targets, s.utf8, s.plain, kAtomString, s.text};
    s.x.ChangeProperty(s.dpy, r.requestor, prop, kAtomAtom, 32, kPropReplace,
                       reinterpret_cast<const unsigned char*>(list), int(sizeof list / sizeof list[0]));
    reply.notify.property = prop;
  } else if (s.owned && (r.target == s.utf8 || r.target == s.plain || r.target == kAtomString ||
                         r.target == s.text) &&
             s.data.size() <= s.max_bytes) {
    s.x.ChangeProperty(s.dpy, r.requestor, prop, r.target == kAtomString ? kAtomString : s.utf8, 8,
                       kPropReplace, reinterpret_cast<const unsigned char*>(s.data.data()),
                       int(s.data.size()));
    reply.notify.property = prop;
  }
  s.x.SendEvent(s.dpy, r.requestor, 0, 0, &reply);
}

}  // namespace

bool set_text(std::string text) {
  if (!open_display()) return false;
  State& s = st();
  if (text.size() > s.max_bytes) {
    MLOG("x11 clipboard: %zu bytes is more than one X request carries; not copied", text.size());
    return false;
  }
  s.data = std::move(text);
  s.x.SetSelectionOwner(s.dpy, s.clipboard, s.win, 0);
  s.x.SetSelectionOwner(s.dpy, s.primary, s.win, 0);
  s.owned = s.x.GetSelectionOwner(s.dpy, s.clipboard) == s.win;
  s.x.Flush(s.dpy);
  return s.owned && !s.dead;
}

int fd() {
  const State& s = st();
  return s.dpy && !s.dead ? s.x.ConnectionNumber(s.dpy) : -1;
}

void pump() {
  State& s = st();
  if (!s.dpy || s.dead) return;
  while (!s.dead && s.x.Pending(s.dpy) > 0) {
    XEvent ev{};
    s.x.NextEvent(s.dpy, &ev);
    if (ev.type == kSelectionRequest) answer(ev.request);
    // Someone else copied: the clipboard is theirs now. PRIMARY changes with
    // every selection anywhere, so only CLIPBOARD decides.
    else if (ev.type == kSelectionClear && ev.clear.selection == s.clipboard) s.owned = false;
  }
  if (!s.dead) s.x.Flush(s.dpy);
}

}  // namespace mico::x11clip
