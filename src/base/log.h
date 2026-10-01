#pragma once
#include <cstdarg>
#include <string>
#include <string_view>

// A tiny append-only log to a file, because mico owns the terminal and cannot
// use stderr. One line per event, timestamped, with a short tag for who wrote
// it ("daemon", "client", "local"). Path: $XDG_STATE_HOME/mico/mico.log, or
// ~/.local/state/mico/mico.log.
namespace mico::logs {

void init(const char* tag);          // safe to call more than once
void line(std::string_view text);
void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
const std::string& path();

}  // namespace mico::logs

#define MLOG(...) ::mico::logs::logf(__VA_ARGS__)
