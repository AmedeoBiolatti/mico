#pragma once
#include <string_view>

namespace mico {

// Opens a link in the user's browser (or mail client, or file manager), on
// this machine, detached: xdg-open, or open on macOS. Only http, https, file
// and mailto links; nothing passes through a shell. False if it could not try.
bool open_url(std::string_view url);

// A notification on this machine's desktop, for when no terminal is attached
// to show one: notify-send, or osascript on macOS. False when this machine has
// no desktop to show it on (a daemon on a server), or it could not try.
bool notify_desktop(std::string_view title, std::string_view body);

}  // namespace mico
