#pragma once
#include <string>

// Writing the desktop clipboard on X11, for terminals that ignore OSC 52 —
// GNOME Terminal among them, where every copy used to vanish.
//
// On X11 a copy is not stored anywhere: whoever owns the CLIPBOARD selection
// answers each paste by sending the text. mico's daemon runs for as long as
// the agents do, so it can be that owner itself, with no xclip or xsel. Xlib
// is loaded at runtime, not linked: without a display, or without libX11,
// set_text() just reports false and OSC 52 remains the only path.
namespace mico::x11clip {

// Takes ownership of CLIPBOARD and PRIMARY with `text`. The display is opened
// on first use, not at startup. True when the desktop clipboard now holds it.
bool set_text(std::string text);

// The X connection to poll, or -1 before the first copy / without a display.
int fd();

// Answers pending paste requests. Cheap when there are none; call it whenever
// fd() is readable (and harmless to call more often).
void pump();

}  // namespace mico::x11clip
