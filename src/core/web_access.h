#pragma once
#include <string>

// The web view: whether the daemon serves it, on which port, and the token a
// browser must present. Kept in mico's config directory: `web` holds
// "on|off [port]", `web-token` the token, readable by the user alone.
namespace mico {

inline constexpr int kDefaultWebPort = 7311;

bool web_enabled();
int web_port();
// Turns the web view on or off and saves it; the daemon follows.
void set_web(bool on, int port = 0);
// The token, made on first use: 32 random bytes as hex. Anyone holding it can
// do whatever mico can, so it lives in a file only the user can read.
std::string web_token();
// Where to point a browser: the token rides in the fragment, which a browser
// never sends to a server or puts in a Referer.
std::string web_url();

}  // namespace mico
