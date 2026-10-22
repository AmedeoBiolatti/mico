#pragma once
#include <poll.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "api/client.h"
#include "core/workspace.h"

// The web view: a page, and a WebSocket carrying the state protocol
// (api/client.h), served by the daemon to a browser on this machine.
//
// It listens on 127.0.0.1, and that is not enough on its own: any page a
// browser has open can reach localhost. So the socket wants the token from
// web_access.h, and a request naming any other host (a DNS-rebinding page) or
// a WebSocket from any other origin is refused.
//
// The token is shown one of two ways. A client may name it as a subprotocol,
// "mico-token.<hex>", except over plain http from the local network, where
// that header crosses the WiFi as written. Or it offers none: the socket opens,
// the daemon sends {"type":"challenge","nonce":hex}, and the client answers
// {"type":"auth","proof":hex} with web_auth_proof(token, nonce); nothing else
// is read or sent until it has. The page always does this, so the token never
// leaves the browser. `:web lan` listens on every address and answers to this
// machine's own: what the page shows still crosses the network in the clear.
namespace mico {

class WebServer {
 public:
  explicit WebServer(Workspace& ws);
  ~WebServer();
  WebServer(const WebServer&) = delete;
  WebServer& operator=(const WebServer&) = delete;

  // Starts or stops listening to follow web_enabled(). Cheap: call it every
  // turn of the loop.
  void sync();
  bool listening() const { return lfd_ >= 0; }
  // Adds what to wait for to the daemon's poll set; after poll(), handle()
  // reads the same entries back.
  void add_fds(std::vector<pollfd>& fds);
  void handle(const std::vector<pollfd>& fds);
  // Sends each connected client what changed. After the workspace has been
  // serviced, so they see the same state the terminal does.
  void pump();

 private:
  struct Conn;
  void accept_all();
  void read_http(Conn& c);
  void read_ws(Conn& c);
  void respond(Conn& c, int status, std::string_view type, std::string_view body);
  void send_text(Conn& c, std::string_view text);
  void send_frame(Conn& c, int opcode, std::string_view payload);
  void flush(Conn& c);
  bool allowed_host(std::string_view host) const;
  bool lan_host(std::string_view host) const;
  void authenticate(Conn& c, std::string_view message);

  Workspace& ws_;
  int lfd_ = -1;
  int port_ = 0;
  bool lan_ = false;     // listening on every address, not only 127.0.0.1
  int failed_port_ = 0;  // the port last found taken, not tried again
  std::vector<std::unique_ptr<Conn>> conns_;
  size_t fds_from_ = 0;  // where this server's entries start in the poll set
  size_t polled_ = 0;    // connections that had an entry
};

// Sec-WebSocket-Accept for a client's Sec-WebSocket-Key (RFC 6455).
std::string websocket_accept(std::string_view key);
// HMAC-SHA256 (RFC 2104) as lower-case hex.
std::string hmac_sha256_hex(std::string_view key, std::string_view message);
// What a client answers a challenge with: HMAC-SHA256, keyed with the token as
// written (64 hex characters), of "mico-auth " and the nonce.
std::string web_auth_proof(std::string_view token, std::string_view nonce);

}  // namespace mico
