#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace mico::text {

// Decodes one UTF-8 scalar starting at `i`, advancing `i`. Invalid bytes decode
// to U+FFFD and advance by one, so decoding always terminates.
char32_t decode(std::string_view s, size_t& i);
void encode(char32_t cp, std::string& out);

// Display columns for a code point (0 for combining marks, 2 for wide/emoji).
int cp_width(char32_t cp);
int str_width(std::string_view s);

// Truncates to `max_cols` display columns, appending "…" when it cuts.
std::string ellipsize(std::string_view s, int max_cols);
// Case-insensitive search, ASCII letters folded (anything else must match
// exactly). `needle` must already be lower-cased with fold(). Returns npos
// when absent. Scans with memchr for the needle's first byte in both cases,
// so a miss over a large file costs little more than a plain memchr.
std::string fold(std::string_view s);
size_t find_folded(std::string_view hay, std::string_view needle, size_t from = 0);

// Where `needle` first occurs in `hay` at or after `from`, or npos. What the
// index passes scan a gigabyte of transcripts with: their needles start with
// a quote, which JSON has every few bytes, and string_view::find stops at
// each one. This tests sixteen positions at a time instead.
size_t find(std::string_view hay, std::string_view needle, size_t from = 0);
inline bool contains(std::string_view hay, std::string_view needle) {
  return find(hay, needle) != std::string_view::npos;
}

// Collapses all whitespace runs to single spaces; used for one-line previews.
std::string oneline(std::string_view s, int max_cols);

// A slice of the input, so wrapping copies nothing.
struct Span {
  uint32_t off;
  uint32_t len;
};

// Wraps to `cols` display columns, breaking on word boundaries where possible
// and hard-breaking words longer than the line. Appends spans relative to `s`;
// the caller's vector is reused across calls so steady-state layout allocates
// nothing at all.
// Stops after `max_spans` lines: a collapsed block shows three, and wrapping
// the other ten thousand only to drop them was most of its layout cost.
void wrap_spans(std::string_view s, int cols, std::vector<Span>& out,
                size_t max_spans = size_t(-1));

}  // namespace mico::text
