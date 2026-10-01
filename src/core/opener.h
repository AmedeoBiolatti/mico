#pragma once
#include <string_view>

namespace mico {

// Opens a link in the user's browser (or mail client, or file manager), on
// this machine, detached: xdg-open, or open on macOS. Only http, https, file
// and mailto links; nothing passes through a shell. False if it could not try.
bool open_url(std::string_view url);

}  // namespace mico
