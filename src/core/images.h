#pragma once
#include <string>
#include <string_view>
#include <vector>

// Images inside a transcript line, whichever agent wrote it: Claude's
// {"type":"image","source":{"media_type":…,"data":…}} blocks (screenshots, an
// image file the agent read, one the user pasted), codex's input_image data
// URLs, pi's {"type":"image","data":…,"mimeType":…}. Matched on the JSON's own
// quotes, which text inside a string cannot produce (its quotes are escaped).
namespace mico {

// An image in a line: its media type, and where in the line its base64 is.
struct LineImage {
  std::string media;
  size_t at = 0, len = 0;
};
// The images the line holds, at most 64.
std::vector<LineImage> line_images(std::string_view line);
// As an image event's summary keeps it, "image/png 1234 5678", and back: the
// picture is read out of the line when it is laid out, not searched for again.
std::string to_string(const LineImage& im);
bool from_string(std::string_view s, LineImage* im);

// How many images the line holds.
int count_images(std::string_view line);
// The n-th: its media type and base64 data (a view into `line`).
bool transcript_image(std::string_view line, int n, std::string* media, std::string_view* base64);

}  // namespace mico
