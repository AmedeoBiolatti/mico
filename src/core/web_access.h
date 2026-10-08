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
// Replaces the token with a new one, for when an address with the old one went
// somewhere it should not have. Browsers holding the old one are let go.
// False when no new token could be made; the old one then stands.
bool new_web_token();
// Where to point a browser: the token rides in the fragment, which a browser
// never sends to a server or puts in a Referer.
std::string web_url();

// The name this machine is reached by from elsewhere, when something in front
// of the web view (tailscale serve, over HTTPS) forwards to it: kept in `web-host`,
// empty when none. The daemon still listens on 127.0.0.1 only; this is the
// one other host it answers to, and the page is then https with a wss socket.
std::string web_host();
void set_web_host(const std::string& host);  // "" clears it; lower case, [a-z0-9.-] only
// Where to point a browser on another machine; empty without a web_host().
std::string web_remote_url();
// This machine's name on the tailnet, from `tailscale status`; empty when
// tailscale is not there or not up.
std::string tailscale_name();

// Puts the web view on the tailnet, over HTTPS, with `tailscale serve --bg`:
// only when nothing else is served there, since another configuration is
// never touched. `owned` is whether mico set it up (and so will take it down).
struct TailscaleServe {
  bool ok = false;      // the tailnet reaches the web view
  bool owned = false;   // mico put it there
  std::string note;     // what to tell the user when it is not (or is only partly) done
};
TailscaleServe tailscale_serve(int port);
// Takes down what tailscale_serve put up; nothing else.
void tailscale_unserve();

}  // namespace mico
