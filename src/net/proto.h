#pragma once
#include <sys/un.h>

#include <cstdint>
#include <string>
#include <string_view>

#include "term/caps.h"

// The daemon renders; the client paints. That makes the protocol tiny: the
// client ships the bytes its terminal produced, the daemon ships the bytes its
// terminal should consume. Neither side needs to understand the other's state.
namespace mico::proto {

enum class Type : uint8_t {
  Hello = 1,   // C->D  u16 w, u16 h [, u8 flags, u16 cell_w, u16 cell_h]
  Input = 2,   // C->D  raw terminal bytes, undecoded
  Resize = 3,  // C->D  as Hello
  Bye = 4,     // C->D
  Frame = 5,   // D->C  escape sequences to write verbatim
  Detach = 6,  // D->C  the daemon is going away
  Kill = 7,    // C->D  stop the daemon and everything it owns
  OpenUrl = 8, // D->C  the user clicked this link: open it where they are
};

inline constexpr uint32_t kMaxPayload = 8u << 20;

void encode(Type t, std::string_view payload, std::string& out);
void encode_size(Type t, int w, int h, std::string& out);
// Hello and Resize with what the terminal can draw appended. A daemon that
// predates it reads the size and ignores the rest.
void encode_size(Type t, int w, int h, const GfxCaps& caps, std::string& out);

// Pops one complete message off the head of `buf`. False if more bytes are
// needed. Returns false and clears `buf` on a malformed length.
bool decode(std::string& buf, Type* t, std::string* payload);
bool decode_size(std::string_view payload, int* w, int* h);
// False (and `caps` untouched) when the client did not say.
bool decode_caps(std::string_view payload, GfxCaps* caps);

// $XDG_RUNTIME_DIR/mico/default.sock, or /tmp/mico-<uid>/default.sock when
// there is no runtime dir or the path under it is too long for a unix socket.
// Client and daemon both call these, so they always agree on where to meet.
std::string socket_path();
std::string socket_dir();

// Fills a unix socket address. False, rather than truncating, when the path
// does not fit: a truncated path binds a file nobody else will ever look for.
bool socket_addr(const std::string& path, sockaddr_un* out);

// True if `dir` is a real directory owned by this user and closed to everyone
// else. With `create`, makes it first if it is missing. /tmp is shared, so a
// directory someone else made there first must not be trusted with the socket.
bool private_dir(const std::string& dir, bool create);

}  // namespace mico::proto
