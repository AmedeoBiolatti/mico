#include "base/uuid.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>

namespace mico {

std::string make_uuid_v4() {
  unsigned char b[16];
  int fd = ::open("/dev/urandom", O_RDONLY);
  if (fd < 0 || read(fd, b, sizeof b) != (ssize_t)sizeof b) {
    for (auto& x : b) x = (unsigned char)(rand() & 0xFF);
  }
  if (fd >= 0) ::close(fd);
  b[6] = (b[6] & 0x0F) | 0x40;  // version 4
  b[8] = (b[8] & 0x3F) | 0x80;  // variant 1
  char out[37];
  snprintf(out, sizeof out,
           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11],
           b[12], b[13], b[14], b[15]);
  return out;
}

}  // namespace mico
