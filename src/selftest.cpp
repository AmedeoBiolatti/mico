#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <chrono>
#include <thread>
#include <cstdio>
#include <unistd.h>

#include <cmath>
#include <random>
#include <map>
#include <unordered_set>
#include <utility>
#include <string>

#include "adapters/adapters.h"
#include "base/fs.h"
#include "adapters/claude/claude.h"
#include "adapters/codex/codex.h"
#include "adapters/pi/pi.h"
#include "adapters/screen.h"
#include "base/json.h"
#include "adapters/adapter.h"
#include "term/encoder.h"
#include "vt/vt.h"
#include "term/input.h"
#include "vt/keys.h"
#include "core/activity.h"
#include "core/edits.h"
#include "core/search.h"
#include "views/chart.h"
#include "views/markdown.h"
#include "core/store.h"
#include "core/usage.h"
#include "base/text.h"
#include "core/clipboard.h"
#include "core/models.h"
#include "model/state.h"
#include "views/markdown.h"
#include "views/latex.h"
#include "views/prompt_editor.h"
#include "views/views.h"
#include "core/session.h"
#include "model/background.h"
#include "ui/app.h"
#include "ui/picker.h"
#include "views/completion.h"
#include "core/commands.h"
#include "views/chat_render.h"
#include "views/code.h"
#include "views/diagram.h"
#include "base/process.h"
#include "base/progress.h"
#include "views/json_view.h"
#include "views/notebook.h"
#include "core/settings.h"
#include "core/away.h"
#include "core/git.h"
#include "core/procmem.h"
#include <sys/wait.h>
#include "net/proto.h"
#include "term/links.h"
#include "core/images.h"
#include "math/picture.h"
#include "math/deflate.h"
#include "term/kitty.h"
#include "term/sixel.h"
#include "term/term.h"
#include "math/layout.h"
#include "math/math.h"
#include "math/tex.h"

namespace mico {
int run_regression_tests();
std::string mcp_handle(std::string_view msg, const std::string& cwd);
namespace {

int g_fail = 0;

void check(bool ok, const char* what) {
  if (!ok) { printf("  FAIL  %s\n", what); g_fail++; }
}

// Writes `body` to `path`, replacing it.
void put_file(const std::string& path, const std::string& body) {
  if (FILE* f = fopen(path.c_str(), "wb")) {
    fwrite(body.data(), 1, body.size(), f);
    fclose(f);
  }
}

// Inflates a zlib stream made of fixed-Huffman blocks — all the compressor
// writes — so the tests can round-trip it without a zlib of their own.
bool inflate_fixed(std::string_view z, std::string& out) {
  if (z.size() < 6 || uint8_t(z[0]) != 0x78) return false;
  size_t pos = 2;
  uint32_t acc = 0;
  int n = 0;
  const auto bit = [&]() -> int {
    if (n == 0) {
      if (pos >= z.size()) return -1;
      acc = uint8_t(z[pos++]);
      n = 8;
    }
    const int b = int(acc & 1);
    acc >>= 1;
    n--;
    return b;
  };
  const auto bits = [&](int k) {
    uint32_t v = 0;
    for (int i = 0; i < k; i++) v |= uint32_t(bit()) << i;
    return v;
  };
  static constexpr uint16_t kLen[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
  static constexpr uint8_t kLenX[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                      2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
  static constexpr uint16_t kDist[] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                       193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                                       6145, 8193, 12289, 16385, 24577};
  static constexpr uint8_t kDistX[] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                       6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
  for (;;) {
    const int final = bit();
    if (bits(2) != 1) return false;
    for (;;) {
      uint32_t code = 0;
      int sym = -1;
      for (int len = 1; len <= 9 && sym < 0; len++) {
        const int b = bit();
        if (b < 0) return false;
        code = (code << 1) | uint32_t(b);
        if (len == 7 && code <= 0x17) sym = int(256 + code);
        else if (len == 8 && code >= 0x30 && code <= 0xBF) sym = int(code - 0x30);
        else if (len == 8 && code >= 0xC0 && code <= 0xC7) sym = int(280 + code - 0xC0);
        else if (len == 9 && code >= 0x190) sym = int(144 + code - 0x190);
      }
      if (sym < 0) return false;
      if (sym < 256) { out.push_back(char(sym)); continue; }
      if (sym == 256) break;
      const int li = sym - 257;
      if (li > 28) return false;
      const size_t len = kLen[li] + bits(kLenX[li]);
      uint32_t dc = 0;
      for (int i = 0; i < 5; i++) dc = (dc << 1) | uint32_t(bit());
      if (dc > 29) return false;
      const size_t dist = kDist[dc] + bits(kDistX[dc]);
      if (dist > out.size()) return false;
      for (size_t i = 0; i < len; i++) out.push_back(out[out.size() - dist]);
    }
    if (final) break;
  }
  return true;
}

// Decodes a sixel picture back to palette indices, w x h.
bool decode_sixel(std::string_view s, int w, int h, std::vector<int>& px) {
  px.assign(size_t(w) * size_t(h), -1);
  size_t i = s.find('q');
  if (i == std::string_view::npos) return false;
  int color = 0, x = 0, band = 0;
  for (i++; i < s.size(); i++) {
    const char c = s[i];
    if (c == '\x1b') break;
    if (c == '"') { while (i + 1 < s.size() && (std::isdigit(uint8_t(s[i + 1])) || s[i + 1] == ';')) i++; continue; }
    if (c == '#') {
      int v = 0;
      while (i + 1 < s.size() && std::isdigit(uint8_t(s[i + 1]))) v = v * 10 + (s[++i] - '0');
      color = v;
      if (i + 1 < s.size() && s[i + 1] == ';')  // a colour definition, not a selection
        while (i + 1 < s.size() && (std::isdigit(uint8_t(s[i + 1])) || s[i + 1] == ';')) i++;
      continue;
    }
    if (c == '$') { x = 0; continue; }
    if (c == '-') { x = 0; band += 6; continue; }
    int reps = 1;
    char ch = c;
    if (c == '!') {
      reps = 0;
      while (i + 1 < s.size() && std::isdigit(uint8_t(s[i + 1]))) reps = reps * 10 + (s[++i] - '0');
      ch = s[++i];
    }
    if (ch < 63 || ch > 126) return false;
    for (int r = 0; r < reps; r++, x++)
      for (int b = 0; b < 6; b++)
        if (((ch - 63) >> b) & 1) {
          if (x >= w || band + b >= h) return false;
          px[size_t(band + b) * size_t(w) + size_t(x)] = color;
        }
  }
  return true;
}

void check_str(std::string_view got, std::string_view want, const char* what) {
  if (got != want) {
    printf("  FAIL  %s\n        got  '%.*s'\n        want '%.*s'\n", what, int(got.size()),
           got.data(), int(want.size()), want.data());
    g_fail++;
  }
}

std::string unesc(std::string_view body) {
  std::string out;
  js::unescape_append(body, out);
  return out;
}

// Finds a top-level member by key.
js::Value member(std::string_view obj, std::string_view key) {
  js::Value found{};
  js::scan_object(obj, [&](std::string_view k, const js::Value& v) {
    if (k != key) return true;
    found = v;
    return false;
  });
  return found;
}

}  // namespace

// The JSON reader is hand-written and every transcript flows through it, so the
// awkward cases get pinned down here rather than discovered as corrupted chat.
int run_selftest() {
  // Every rendering part on, whatever this machine's Settings say.
  set_render_settings(RenderSettings{}, false);
  printf("mico selftest\n\n");
  // Copying now owns the desktop's X clipboard. A test that copies must not
  // take the real one from whoever runs the tests.
  unsetenv("DISPLAY");
  unsetenv("WAYLAND_DISPLAY");

  // Escaped quotes must not terminate a string early, which would truncate the
  // value and desynchronise the rest of the scan.
  check_str(member(R"({"a":"say \"hi\" now","b":"after"})", "a").body(),
            R"(say \"hi\" now)", "escaped quotes in value");
  check_str(member(R"({"a":"say \"hi\" now","b":"after"})", "b").body(), "after",
            "member after escaped quotes");

  // A backslash immediately before the closing quote is itself escaped.
  check_str(member(R"({"a":"ends with backslash \\","b":"next"})", "b").body(), "next",
            "trailing escaped backslash");

  // Braces and brackets inside strings must not affect nesting depth.
  check_str(member(R"({"a":{"x":"} ] { ["},"b":"safe"})", "b").body(), "safe",
            "braces inside a nested string");

  check(member(R"({"n":42,"s":"x"})", "n").type == js::Type::Number, "number type");
  check(member(R"({"t":true})", "t").is_true(), "true value");
  check(!member(R"({"t":false})", "t").is_true(), "false value");
  check(member(R"({"o":{"k":1}})", "o").is_object(), "object type");
  check(member(R"({"a":[1,2,3]})", "a").is_array(), "array type");
  check(member(R"({"a":"x"})", "zz").type == js::Type::Null, "missing key");

  // Early exit: scanning must stop without touching later members.
  int seen = 0;
  js::scan_object(R"({"a":1,"b":2,"c":3})", [&](std::string_view, const js::Value&) {
    seen++;
    return seen < 2;
  });
  check(seen == 2, "early exit stops the scan");

  int elems = 0;
  js::scan_array(R"([{"a":1},{"b":2},"str",7])", [&](const js::Value&) {
    elems++;
    return true;
  });
  check(elems == 4, "array element count");

  // Unescaping, including the surrogate pairs agents emit for emoji.
  check_str(unesc(R"(a\nb)"), "a\nb", "newline escape");
  check_str(unesc(R"(tab\there)"), "tab\there", "tab escape");
  check_str(unesc(R"(quote\"in)"), "quote\"in", "quote escape");
  check_str(unesc(R"(back\\slash)"), "back\\slash", "backslash escape");
  check_str(unesc(R"(…)"), "\xE2\x80\xA6", "BMP unicode escape");
  check_str(unesc(R"(😀)"), "\xF0\x9F\x98\x80", "surrogate pair");
  check_str(unesc("plain ascii, no escapes"), "plain ascii, no escapes", "literal fast path");
  check(js::Value{R"("no escapes")", js::Type::String}.literal(), "literal() true");
  check(!js::Value{R"("has \n escape")", js::Type::String}.literal(), "literal() false");

  // Malformed input must terminate rather than spin or read out of bounds.
  js::scan_object(R"({"a":"unterminated)", [](std::string_view, const js::Value&) { return true; });
  js::scan_object("{", [](std::string_view, const js::Value&) { return true; });
  js::scan_object("", [](std::string_view, const js::Value&) { return true; });
  js::scan_array("[{", [](const js::Value&) { return true; });
  printf("  malformed input handled without hanging\n");

  // Wrapping must cover every byte of the input exactly once, in order.
  {
    std::string_view text = "the quick brown fox jumps over the lazy dog\nsecond line here";
    std::vector<text::Span> spans;
    text::wrap_spans(text, 12, spans);
    size_t covered = 0;
    bool ordered = true;
    uint32_t last = 0;
    for (auto& sp : spans) {
      if (sp.off < last) ordered = false;
      last = sp.off;
      covered += sp.len;
      check(sp.off + sp.len <= text.size(), "wrap span in bounds");
    }
    check(ordered, "wrap spans are ordered");
    check(covered > 0 && covered <= text.size(), "wrap spans cover a sane range");
    for (auto& sp : spans)
      check(text::str_width(text.substr(sp.off, sp.len)) <= 12, "wrap respects the column limit");
  }

  // Markdown: block classification, marker stripping, inline inks.
  {
    Arena scratch;
    std::vector<md::Seg> segs, inl;
    std::vector<md::Line> lines;
    std::vector<text::Span> spans;
    md::Out out{size_t(-1), &scratch, &segs, &lines, &spans, &inl};

    std::string_view doc =
        "# Title\n"
        "plain **bold** and `code` and *it*\n"
        "- first item\n"
        "> quoted\n"
        "---\n"
        "```\nraw **not bold**\n```\n"
        "[label](http://example.com)\n"
        "while not_loss_detected: _lean_ and __strong__ keep a_b_c\n";

    Arena src;
    Str t = src.add(doc);
    md::render(src.view(t), t.off, false, 60, out);

    auto text_of = [&](const md::Seg& sg) -> std::string_view {
      return (sg.off & md::kScratchBit) ? scratch.view(Str{sg.off & ~md::kScratchBit, sg.len})
                                        : src.view(Str{sg.off, sg.len});
    };
    auto has = [&](md::Ink ink, std::string_view want) {
      for (const auto& sg : segs)
        if (sg.ink == ink && text_of(sg) == want) return true;
      return false;
    };

    check(has(md::Ink::Heading, "Title"), "heading text without its hashes");
    check(has(md::Ink::Bold, "bold"), "bold run without asterisks");
    check(has(md::Ink::Code, "code"), "code span without backticks");
    check(has(md::Ink::Italic, "it"), "italic run");
    check(has(md::Ink::Quote, "quoted"), "blockquote");
    check(has(md::Ink::Link, "label"), "link shows its label, not the url");
    check(has(md::Ink::CodeText, "raw **not bold**"), "fenced code is verbatim");
    check(has(md::Ink::Italic, "lean") && has(md::Ink::Bold, "strong"), "underscores emphasise a word");
    bool snake = false, short_snake = false;
    for (const auto& sg : segs) {
      snake |= text_of(sg).find("not_loss_detected") != std::string_view::npos;
      short_snake |= text_of(sg).find("a_b_c") != std::string_view::npos;
    }
    check(snake && short_snake, "underscores inside a word are the word's");
    bool rule = false, bullet = false;
    for (const auto& sg : segs) {
      if (sg.ink == md::Ink::Rule) rule = true;
      if (sg.ink == md::Ink::Bullet) bullet = true;
    }
    check(rule, "horizontal rule");
    check(bullet, "list items get a bullet");

    // A wrapped list item must continue, not repeat: the bullet is spliced in
    // ahead of the first line, which shifts every following segment.
    {
      lines.clear();
      segs.clear();
      Arena lsrc;
      const std::string item =
          "one two three four five six seven eight nine ten eleven twelve";
      Str lt = lsrc.add("- " + item + "\n");
      md::Out lout{size_t(-1), &scratch, &segs, &lines, &spans, &inl};
      md::render(lsrc.view(lt), lt.off, false, 24, lout);
      check(lines.size() >= 3, "a long bullet wraps to several lines");

      std::string joined;
      for (const auto& L : lines) {
        for (int k = 0; k < L.seg_count; k++) {
          const md::Seg& sg = segs[L.seg_first + k];
          std::string_view v = (sg.off & md::kScratchBit)
                                   ? scratch.view(Str{sg.off & ~md::kScratchBit, sg.len})
                                   : lsrc.view(Str{sg.off, sg.len});
          if (v == "\xE2\x80\xA2 ") continue;  // the bullet itself
          if (!joined.empty()) joined += ' ';
          joined.append(v);
        }
      }
      check(joined == item, "a wrapped bullet reads back as its original text");
      if (joined != item) printf("        got  '%s'\n        want '%s'\n", joined.c_str(), item.c_str());
    }

    // LaTeX: a terminal cannot typeset equations, so rendering means turning
    // the symbols an agent actually writes into the Unicode that already
    // means the same thing.
    {
      auto tex = [](std::string_view in) { std::string s; latex::to_unicode(in, s); return s; };
      check(tex("\\alpha + \\beta") == "α + β", "greek letters");
      check(tex("x^2 + y^2") == "x² + y²", "digit superscripts");
      check(tex("a_{ij}") == "aᵢⱼ", "braced subscript");
      check(tex("x_0") == "x₀", "bare subscript");
      check(tex("e^{i\\pi} + 1 = 0").find("π") != std::string::npos,
            "superscript group with a greek letter inside is still readable");
      check(tex("\\frac{1}{2}") == "1⁄2", "simple fraction uses the fraction slash");
      check(tex("\\sqrt{x}") == "√x", "square root");
      check(tex("\\sum_{i=1}^{n} x_i") == "∑ᵢ₌₁ⁿ xᵢ", "sum with bounds");
      check(tex("\\sin(x) \\cdot \\cos(x)") == "sin(x) · cos(x)", "function names and cdot");
      check(tex("\\unknownthing") == "unknownthing",
            "an unrecognised command still shows its name, not nothing");
      check(tex("\\%") == "%", "escaped literal percent");

      // Wired through the markdown renderer: $...$ inline, $$...$$ display.
      lines.clear();
      segs.clear();
      Arena msrc;
      Str mt = msrc.add("Einstein wrote $E = mc^2$.\n\n$$\n\\alpha + \\beta = \\gamma\n$$\n");
      md::Out mout{size_t(-1), &scratch, &segs, &lines, &spans, &inl};
      md::render(msrc.view(mt), mt.off, false, 60, mout);
      bool inline_math = false, display_math = false;
      for (const auto& sg : segs) {
        if (sg.ink != md::Ink::Math) continue;
        std::string_view v = text_of(sg);
        if (v.find("mc²") != std::string_view::npos) inline_math = true;
        if (v.find("α + β = γ") != std::string_view::npos) display_math = true;
      }
      check(inline_math, "$...$ renders inline as math, not literal dollar signs");
      check(display_math, "a $$ ... $$ block renders as math");

      // Equations as their source: the LaTeX as written, dollars and all.
      {
        const RenderSettings saved = render_settings();
        RenderSettings src = saved;
        src.way[kEquations] = uint8_t(Equations::Source);
        set_render_settings(src, false);
        lines.clear();
        segs.clear();
        md::render(msrc.view(mt), mt.off, false, 60, mout);
        std::string all;
        bool math_ink = false;
        for (const auto& sg : segs) {
          // This text's own spans are in msrc, not in the earlier test's src.
          all += (sg.off & md::kScratchBit) ? text_of(sg) : msrc.view(Str{sg.off, sg.len});
          math_ink |= sg.ink == md::Ink::Math;
        }
        set_render_settings(saved, false);
        check(!math_ink && all.find("$E = mc^2$") != std::string::npos && all.find("\\alpha + \\beta") != std::string::npos,
              "equations as source: $...$ and $$ blocks shown as written");
      }
    }

    // Tables. The invariant that matters is that the dividers line up: rows
    // are deliberately not padded to equal length, because trailing space at
    // the end of a line is dead weight.
    {
      auto render_table = [&](std::string_view md_src, int cols) {
        lines.clear();
        segs.clear();
        Arena tsrc;
        Str tt = tsrc.add(md_src);
        md::Out tout{size_t(-1), &scratch, &segs, &lines, &spans, &inl};
        md::render(tsrc.view(tt), tt.off, false, cols, tout);
        std::vector<std::string> rows;
        for (const auto& L : lines) {
          std::string row;
          for (int k = 0; k < L.seg_count; k++) {
            const md::Seg& sg = segs[L.seg_first + k];
            row.append((sg.off & md::kScratchBit)
                           ? scratch.view(Str{sg.off & ~md::kScratchBit, sg.len})
                           : tsrc.view(Str{sg.off, sg.len}));
          }
          rows.push_back(row);
        }
        return rows;
      };
      // Display columns at which a vertical divider sits.
      auto divider_cols = [](std::string_view row) {
        std::vector<int> at;
        int col = 0;
        for (size_t i = 0; i < row.size();) {
          char32_t cp = text::decode(row, i);
          if (cp == 0x2502 || cp == 0x253C) at.push_back(col);
          col += text::cp_width(cp);
        }
        return at;
      };

      auto rows = render_table(
          "| metric | before | after |\n"
          "|--------|-------:|------:|\n"
          "| scan | 135 ms | 13 ms |\n",
          60);
      check(rows.size() == 3, "a three-row table renders three lines");
      if (rows.size() == 3) {
        for (const auto& r : rows) check(r.find('|') == std::string::npos, "no raw pipes survive");
        auto a = divider_cols(rows[0]), b = divider_cols(rows[1]), c = divider_cols(rows[2]);
        check(a.size() == 2 && a == b && b == c, "dividers line up across every row");
        check(rows[2].find(" 13 ms") != std::string::npos, "right-aligned column pads on the left");
      }

      // A stray extra pipe must not invent a column the table does not have.
      auto ragged = render_table("| a | b |\n|---|---|\n| 1 | 2 | 3 |\n", 40);
      check(ragged.size() == 3 && divider_cols(ragged[0]).size() == 1,
            "ragged rows do not add a column");

      // An escaped pipe is content.
      auto esc = render_table("| expr | means |\n|---|---|\n| a \\| b | or |\n", 40);
      check(esc.size() == 3 && esc[2].find("a | b") != std::string::npos,
            "an escaped pipe stays inside its cell");

      // Wide glyphs must be measured in columns, not bytes.
      auto cjk = render_table("| name | v |\n|---|---|\n| \xE4\xB8\xAD\xE6\x96\x87 | 1 |\n", 40);
      check(cjk.size() == 3 && divider_cols(cjk[0]) == divider_cols(cjk[2]),
            "wide characters keep the columns aligned");

      // No trailing whitespace anywhere.
      for (const auto& r : rows)
        check(r.empty() || r.back() != ' ', "no trailing space on a table row");
    }

    for (const auto& sg : segs)
      check(text_of(sg).find("**") == std::string_view::npos ||
                text_of(sg).find("not bold") != std::string_view::npos,
            "no stray markers survive into output");

    // A capped render must stop early rather than lay out everything.
    lines.clear();
    segs.clear();
    std::string big;
    for (int i = 0; i < 5000; i++) big += "line of text here\n";
    Arena b;
    Str bt = b.add(big);
    md::Out capped{3, &scratch, &segs, &lines, &spans, &inl};
    md::render(b.view(bt), bt.off, false, 60, capped);
    check(lines.size() <= 3, "max_lines honoured");
  }

  // Attachments: a long paste or an image held as one token in the box and
  // put back only when the message is sent.
  {
    PromptEditor e;
    for (char c : std::string("see ")) e.insert_char(char32_t(c));
    e.insert_attachment({false, "line1\nline2", "[Pasted 11 characters]"});
    e.insert_char(U' ');
    e.insert_attachment({true, "/tmp/shot.png", "[Image #1]"});
    for (char c : std::string(" ok")) e.insert_char(char32_t(c));
    const auto parts = e.parts();
    check(parts.size() == 3 && !parts[0].image && parts[0].text == "see line1\nline2 " &&
              parts[1].image && parts[1].text == "/tmp/shot.png" && !parts[2].image &&
              parts[2].text == " ok",
          "a paste expands in place and an image is its own part, in order");
    check(e.image_count() == 1, "images are counted for numbering");
    // One Backspace from just after the image token removes the whole token.
    e.move_left(false); e.move_left(false); e.move_left(false);  // before " ok"
    e.backspace();
    const auto after = e.parts();
    check(after.size() == 1 && after[0].text == "see line1\nline2  ok",
          "one backspace removes an attachment whole");
    // Wrapping measures a token as its label.
    PromptEditor w;
    w.insert_attachment({false, std::string(5000, 'x'), "[Pasted 5,000 characters]"});
    check(w.wrap(80).size() == 1, "a collapsed paste occupies one row, not five thousand characters");
    w.clear();
    check(w.empty() && w.parts().empty(), "clear drops attachments too");
  }

  // Pasted file paths that name images become image attachments.
  {
    char dir[] = "/tmp/mico_clip_XXXXXX";
    if (mkdtemp(dir)) {
      const std::string a = std::string(dir) + "/a shot.png", b = std::string(dir) + "/b.JPG";
      for (const auto& f : {a, b}) { FILE* fp = fopen(f.c_str(), "w"); if (fp) { fputs("x", fp); fclose(fp); } }
      check(clip::image_paths("'" + a + "'").size() == 1, "a quoted image path is an image");
      check(clip::image_paths("file://" + std::string(dir) + "/a%20shot.png\n").size() == 1,
            "a file:// URI with escapes is an image");
      check(clip::image_paths("'" + a + "' " + b).size() == 2, "several pasted image paths");
      check(clip::image_paths("look at " + b).empty(), "text around a path stays text");
      check(clip::image_paths(std::string(dir) + "/missing.png").empty(), "a path to nothing stays text");
      check(clip::image_paths("/etc/hostname").empty(), "a non-image file stays text");
      unlink(a.c_str()); unlink(b.c_str()); rmdir(dir);
    }
  }

  // PromptEditor history: typing is undone a word at a time, deleting
  // likewise, and anything else is a step of its own. Esc's clear() is
  // undoable; reset(), for text that was sent, is not.
  {
    PromptEditor e;
    for (char c : std::string("hello big world")) e.insert_char(char32_t(c));
    check(e.undo(), "undo after typing");
    check_str(e.text(), "hello big ", "undo takes back the last word typed");
    check(e.undo() && e.undo(), "two more undos");
    check_str(e.text(), "", "undo walks back word by word to empty");
    check(!e.undo(), "nothing left to undo");
    check(e.redo() && e.redo(), "redo");
    check_str(e.text(), "hello big ", "redo replays in order");
    e.insert_char('x');
    check(!e.redo(), "a new edit drops the redo history");

    e.delete_word_before();
    check_str(e.text(), "hello big ", "a word deleted");
    check(e.undo(), "undo the word delete");
    check_str(e.text(), "hello big x", "the word comes back");

    e.backspace();
    e.backspace();
    e.backspace();
    check_str(e.text(), "hello bi", "three backspaces");
    e.undo();
    check_str(e.text(), "hello big x", "a run of backspaces undoes as one");

    e.select_all();
    check(e.has_selection() && e.selected_text() == "hello big x", "select all");
    check_str(e.cut_selection(), "hello big x", "cut returns the selection");
    check(e.empty(), "cut empties the box");
    e.undo();
    check_str(e.text(), "hello big x", "cut is undoable");

    e.move_doc_start(false);
    check(e.cursor() == 0, "Ctrl+Home goes to the start");
    e.move_doc_end(true);
    check(e.selected_text() == "hello big x", "Shift+Ctrl+End selects to the end");

    e.clear();
    check(e.empty() && e.undo() && e.text() == "hello big x", "an Esc clear can be undone");
    e.reset();
    check(e.empty() && !e.can_undo(), "reset forgets the history of a sent message");

    PromptEditor a;
    a.insert_char('<');
    a.insert_attachment(PromptEditor::Attachment{false, "PASTED BODY", "[Pasted 11 characters]"});
    a.insert_char('>');
    a.select_all();
    check_str(a.selected_text(), "<PASTED BODY>", "copying a paste copies its contents");
    a.undo();
    a.undo();
    check(a.text() == "<" && !a.attachment(PromptEditor::kTokenBase + 1),
          "undo takes back a paste");
  }

  // PromptEditor: cursor movement, selection, and the multi-line box Ctrl+J
  // opens up. Pulled out of SessionPane specifically so this — the part most
  // likely to hide an off-by-one — can be checked without a live pty.
  {
    PromptEditor e;
    check(e.empty() && e.cursor() == 0 && !e.has_selection(), "starts empty, no selection");

    e.insert_char('h');
    e.insert_char('i');
    check_str(e.text(), "hi", "typing inserts at the cursor");
    check(e.cursor() == 2, "cursor follows what was typed");

    e.move_left(false);
    check(e.cursor() == 1, "left moves the cursor back one codepoint");
    e.insert_char('X');
    check_str(e.text(), "hXi", "typing inserts, not appends, once the cursor has moved");

    e.move_right(false);
    e.move_right(false);
    check(e.cursor() == e.text().size(), "right walks forward to the end");
    e.move_right(false);
    check(e.cursor() == e.text().size(), "right at the end does not overrun it");

    // A multi-byte codepoint must move as one unit, not one UTF-8 byte at a
    // time — "café" + backspace should drop the é, not mangle it.
    e.clear();
    for (char32_t c : {U'c', U'a', U'f', U'é'}) e.insert_char(c);
    check_str(e.text(), "caf\xC3\xA9", "multi-byte codepoint typed");
    e.backspace();
    check_str(e.text(), "caf", "backspace removes one codepoint, not one byte");

    // Selection: shift-left twice from the end selects the last two chars;
    // typing over a selection replaces it.
    e.clear();
    for (char c : std::string("hello")) e.insert_char(char32_t(c));
    e.move_left(true);
    e.move_left(true);
    check(e.has_selection() && e.sel_lo() == 3 && e.sel_hi() == 5, "shift-left extends a selection");
    e.insert_char('!');
    check_str(e.text(), "hel!", "typing over a selection replaces it");
    check(!e.has_selection(), "the selection is gone after replacing it");

    // An arrow without shift collapses a selection to the edge it moved
    // toward, rather than moving from wherever the cursor happened to be.
    e.clear();
    for (char c : std::string("hello")) e.insert_char(char32_t(c));
    e.move_home(true);  // select the whole line, cursor at 0, anchor at 5
    check(e.has_selection() && e.sel_lo() == 0 && e.sel_hi() == 5, "shift-home selects to the start");
    e.move_right(false);
    check(!e.has_selection() && e.cursor() == 5, "a bare arrow collapses to the far edge and stops there");

    // Ctrl+J / multi-line: Home and End stay within the current line, and Up
    // /Down cross lines while roughly preserving the column.
    e.clear();
    for (char c : std::string("ab")) e.insert_char(char32_t(c));
    e.insert_newline();
    for (char c : std::string("cd")) e.insert_char(char32_t(c));
    check_str(e.text(), "ab\ncd", "Ctrl+J inserts a real newline");
    check(e.lines().size() == 2, "the box now has two lines");

    e.move_home(false);
    check(e.cursor() == 3, "home on the second line stops at its own start, not the text's");
    check(e.move_line(-1, false), "up from a lower line succeeds");
    check(e.cursor() == 0, "up lands on the same column on the line above");
    check(!e.move_line(-1, false), "up from the first line reports it cannot move further");

    e.move_end(false);
    check(e.cursor() == 2, "end on the first line stops at its own end, not the text's");
    check(e.move_line(1, false), "down from an upper line succeeds");
    check(e.cursor() == 5, "down lands on the same column on the line below");
    check(!e.move_line(1, false), "down from the last line reports it cannot move further");

    // Delete (forward) and backspace both respect an active selection first.
    e.clear();
    for (char c : std::string("abcdef")) e.insert_char(char32_t(c));
    e.move_home(false);
    e.move_right(true);
    e.move_right(true);
    e.del();
    check_str(e.text(), "cdef", "delete removes the selection instead of one char forward");

    // Classical Ctrl word motion, word deletion and line kills.
    e.clear();
    for (char c : std::string("one two three")) e.insert_char(char32_t(c));
    e.move_word_left(false);
    check(e.cursor() == 8, "ctrl-left jumps to the start of the word before");
    e.move_word_left(false);
    check(e.cursor() == 4, "ctrl-left crosses whitespace and the previous word");
    e.move_word_right(false);
    check(e.cursor() == 7, "ctrl-right jumps past the word");
    e.move_word_left(false);
    e.delete_word_after();
    check_str(e.text(), "one  three", "delete-word-forward removes the next word");
    e.move_end(false);
    e.delete_word_before();
    check_str(e.text(), "one  ", "delete-word-back removes the word before");

    e.clear();
    for (char c : std::string("keep this")) e.insert_char(char32_t(c));
    e.move_word_left(false);
    e.kill_to_end();
    check_str(e.text(), "keep ", "ctrl-k kills to the end of the line");
    e.move_end(false);
    e.kill_to_start();
    check_str(e.text(), "", "ctrl-u kills to the start of the line");

    // Soft wrap: a long line becomes display rows, and the spaces a prose
    // wrapper would drop are kept so the cursor can sit on them.
    e.clear();
    for (char c : std::string("abcdefghij")) e.insert_char(char32_t(c));
    check(e.wrap(4).size() == 3, "a long line wraps to three rows");
    check(e.wrap(4)[1].first == 4 && e.wrap(4)[1].second == 8, "rows tile the bytes");
    e.clear();
    for (char c : std::string("a b c")) e.insert_char(char32_t(c));
    check(e.wrap(2).size() == 3, "wrap keeps the trailing spaces");

    // Up/Down move across display rows, not logical lines.
    e.clear();
    for (char c : std::string("abcdefghij")) e.insert_char(char32_t(c));
    e.move_home(false);
    check(e.move_display_line(1, 4, false) && e.cursor() == 4, "down crosses a wrapped row");
    check(e.move_display_line(1, 4, false) && e.cursor() == 8, "down again");
    check(e.move_display_line(1, 4, false) && e.cursor() == 10, "down to the short last row");
    check(!e.move_display_line(1, 4, false), "down at the last row reports no move");
    check(e.move_display_line(-1, 4, false) && e.cursor() == 6,
          "up keeps the visual column on the row above");
  }

  // Diff classification.
  {
    Arena scratch, src;
    std::vector<md::Seg> segs, inl;
    std::vector<md::Line> lines;
    std::vector<text::Span> spans;
    md::Out out{size_t(-1), &scratch, &segs, &lines, &spans, &inl};
    Str t = src.add("@@ hunk\n-old line\n+new line\n context\n");
    md::render_diff(src.view(t), t.off, false, 60, out);
    check(lines.size() == 4, "one row per diff line");
    const auto text_of = [&](const md::Seg& sg) { return std::string(src.view(Str{sg.off, sg.len})); };
    if (lines.size() == 4) {
      check(segs[lines[0].seg_first].ink == md::Ink::Hunk, "hunk header");
      check(lines[1].flags & md::kDelRow, "removed line, tinted");
      check(lines[2].flags & md::kAddRow, "added line, tinted");
      check(!(lines[3].flags & (md::kAddRow | md::kDelRow)), "context line, untinted");
      std::string strong_old, strong_new;
      for (uint16_t k = 0; k < lines[1].seg_count; k++)
        if (segs[lines[1].seg_first + k].attr & md::kAttrStrong) strong_old += text_of(segs[lines[1].seg_first + k]);
      for (uint16_t k = 0; k < lines[2].seg_count; k++)
        if (segs[lines[2].seg_first + k].attr & md::kAttrStrong) strong_new += text_of(segs[lines[2].seg_first + k]);
      check(strong_old == "old" && strong_new == "new", "the changed words of a paired line stand out");
    }
    // In a known language, the code keeps its colours inside the diff.
    segs.clear();
    lines.clear();
    Str t2 = src.add("-def f(x):\n+def f(x, y):\n");
    md::render_diff(src.view(t2), t2.off, false, 60, out, code::lang_of("py"));
    bool kw = false;
    for (const auto& sg : segs) kw |= sg.ink == md::Ink::CodeKeyword && text_of(sg) == "def";
    check(kw, "a diff of a known language is syntax coloured");
  }

  // Adapters: the translation from wire format to Event, on records shaped
  // exactly like the ones on disk.
  {
    Arena a;
    std::vector<Event> ev;

    // Claude: an assistant record holding prose plus an Edit tool call.
    claude_adapter().parse(
        R"({"parentUuid":"p","type":"assistant","message":{"role":"assistant","content":[)"
        R"({"type":"text","text":"doing it"},)"
        R"({"type":"tool_use","id":"tu_1","name":"Edit","input":{"file_path":"/x.c",)"
        R"("old_string":"int a;","new_string":"int b;"}}]}})",
        a, ev);
    check(ev.size() == 2, "assistant record yields prose + tool call");
    if (ev.size() == 2) {
      check(ev[0].kind == EventKind::Assistant, "first block is prose");
      check_str(a.view(ev[0].text), "doing it", "prose text");
      check(ev[1].kind == EventKind::ToolCall, "second block is a tool call");
      check_str(a.view(ev[1].name), "Edit", "tool name");
      check_str(a.view(ev[1].summary), "/x.c", "tool summary prefers file_path");
      check_str(a.view(ev[1].detail), "-int a;\n+int b;\n", "edit renders as a diff");
      check(ev[1].tool_id != 0, "tool id hashed");
    }

    // Claude: a user record carrying a tool_result is output, not a human turn.
    ev.clear();
    claude_adapter().parse(
        R"({"type":"user","message":{"role":"user","content":[)"
        R"({"type":"tool_result","tool_use_id":"tu_1","is_error":true,"content":"boom"}]}})",
        a, ev);
    check(ev.size() == 1 && ev[0].kind == EventKind::ToolResult, "tool_result is a result");
    if (ev.size() == 1) {
      check(!ev[0].ok, "is_error propagates");
      check_str(a.view(ev[0].text), "boom", "result text");
    }

    // Claude: a plain user turn, with escapes.
    ev.clear();
    claude_adapter().parse(
        R"({"type":"user","message":{"role":"user","content":"hi\nthere \"quoted\""}})", a, ev);
    check(ev.size() == 1 && ev[0].kind == EventKind::User, "plain user turn");
    if (ev.size() == 1) check_str(a.view(ev[0].text), "hi\nthere \"quoted\"", "user text unescaped");

      // Session state: each agent reports a different set, and the strip is
    // built from whatever it actually said.
    {
      SessionState st;
      claude_adapter().observe(R"({"type":"mode","mode":"normal"})", st);
      claude_adapter().observe(R"({"type":"permission-mode","permissionMode":"auto"})", st);
      claude_adapter().observe(
          R"({"parentUuid":"p","message":{"model":"claude-opus-5","role":"assistant"},)"
          R"("effort":"high","type":"assistant"})",
          st);
      const std::string* m = st.find("model");
      const std::string* e = st.find("effort");
      check(m && *m == "claude-opus-5", "claude model observed");
      check(e && *e == "high", "claude effort observed");
      check(st.find("mode") && *st.find("mode") == "normal", "claude mode observed");
      check(st.find("perm") && *st.find("perm") == "auto", "claude permission mode observed");

      // Records that say nothing about the session must not disturb it.
      claude_adapter().observe(R"({"type":"user","message":{"content":"hi"}})", st);
      check(st.find("model") && *st.find("model") == "claude-opus-5", "a user turn leaves state alone");

      SessionState cx;
      codex_adapter().observe(
          R"({"type":"turn_context","payload":{"model":"gpt-6-astra","effort":"medium",)"
          R"("approval_policy":"on-request","personality":"pragmatic","summary":"auto",)"
          R"("sandbox_policy":{"type":"workspace-write"}}})",
          cx);
      check(cx.fields.size() == 6, "codex reports six fields");
      check(cx.find("model") && *cx.find("model") == "gpt-6-astra", "codex model observed");
      check(cx.find("sandbox") && *cx.find("sandbox") == "workspace-write", "codex sandbox observed");
      check(cx.find("approval") && *cx.find("approval") == "on-request", "codex approvals observed");
      check(!cx.find("mode"), "codex does not invent fields it never reported");

      // The menus built from that state.
      check(!chip_command("model").empty(), "model is actionable");
      check(!chip_command("effort").empty(), "effort is actionable");

      // With nothing probed yet, the model chip must not invent a list; it
      // hands the question to claude's own picker.
      set_known_models("claude", {});
      {
        auto cold = chip_menu(st, "model", true, "claude");
        bool picker = false;
        for (const auto& it : cold)
          if (it.action == "chipcmd:/model") picker = it.enabled;
        check(picker, "an unprobed model chip defers to the agent");
      }

      // A probed picker is parsed into entries the chip can set directly.
      {
        Vt pv;
        pv.resize(110, 14);
        pv.write(
            "Select model\r\n"
            "    1. Default (recommended)  Opus 5.5 \xC2\xB7 Best for everyday, complex tasks\r\n"
            "    2. Opus                   Opus 5.5 \xC2\xB7 Best for everyday \xC2\xB7 ~2x usage\r\n"
            "    3. Fable                  Fable 5.1 \xC2\xB7 Most capable for your hardest and\r\n"
            "                              longest-running tasks\r\n"
            "    4. Sonnet                 Sonnet 5 \xC2\xB7 Efficient for routine tasks\r\n"
            "    5. Haiku                  Haiku 4.5 \xC2\xB7 Fastest for quick answers\r\n"
            "  \xE2\x9D\xAF 6. Opus 5 \xE2\x9C\x94             Newer version available \xC2\xB7 select Opus\r\n");
        auto found = parse_model_picker(pv);
        check(found.size() == 6, "every picker entry is read");
        check(found[0].value == "default", "the default entry keeps its alias");
        check(found[1].value == "opus", "a bare family name goes over as an alias");
        check(found[2].value == "fable", "a newly shipped model needs no code change");
        check(found[5].value == "claude-opus-5", "a versioned entry goes over in full");
        check(found[5].current, "the checked entry is the current model");
        check(found[1].label.find("Opus 5.5") != std::string::npos,
              "the concrete model shows in the label");
        check(found[5].label == "Opus 5", "prose is not mistaken for a model name");
        set_known_models("claude", found);
      }

      // The claude model chip lists those values, each a set command.
      auto mm = chip_menu(st, "model", true, "claude");
      int direct = 0, checked = 0;
      bool has_fable = false;
      for (const auto& it : mm) {
        if (it.action.rfind("chipset:/model ", 0) == 0) direct++;
        if (it.action == "chipset:/model fable") has_fable = true;
        if (it.checked) checked++;
      }
      check(direct == 6, "the model chip lists every probed model");
      check(has_fable, "a probed model is offered by its own alias");
      check(checked <= 1, "at most the current model is marked");

      // The mode chip exposes Claude's permission ring directly, plan mode
      // included, and marks the one currently in effect.
      {
        SessionState ms;
        ms.set("mode", "mode", "acceptEdits");
        auto menu = chip_menu(ms, "mode", true, "claude");
        bool plan = false, cur_checked = false;
        for (const auto& it : menu) {
          if (it.action.rfind("chipmode:mode|plan|", 0) == 0) plan = it.enabled;
          if (it.checked && it.action.find("|acceptEdits|") != std::string::npos)
            cur_checked = true;
        }
        check(plan, "the mode chip offers plan mode");
        check(cur_checked, "the mode chip marks the active mode");
      }

      // The picker a chip click opens: the probe's wording as the label, the
      // exact id as the detail, the cursor on the value in effect.
      {
        SessionState ps;
        ps.set("model", "model", "fable");
        ps.set("effort", "effort", "medium");
        int cursor = -1;
        std::string title;
        auto picks = chip_pick_items(ps, "model", true, "claude", &cursor, &title);
        check(title == "model \xC2\xB7 fable", "chip picker: the title names the chip and its value");
        bool fable_checked = false, id_detail = false, copy_pinned = false;
        for (size_t i = 0; i < picks.size(); i++) {
          const PickItem& it = picks[i];
          if (it.id == "chipset:/model fable") {
            fable_checked = it.checked && cursor == int(i);
            id_detail = it.label.find("Fable 5.1") != std::string::npos && it.hint == "fable";
          }
          if (it.id == "chipcopy:model") copy_pinned = it.pinned && it.detail == "fable";
        }
        check(fable_checked, "chip picker: the cursor starts on the current model");
        check(id_detail, "chip picker: the probe's wording, the value as the hint");

        // Claude's initialize answer names the models with descriptions and
        // effort levels; the chips use both.
        const std::string minit =
            R"({"type":"control_response","response":{"subtype":"success","response":{"commands":[],"models":[)"
            R"({"value":"opus","resolvedModel":"claude-opus-5-5","displayName":"Opus 5.5","description":"For complex work",)"
            R"("supportedEffortLevels":["low","medium","high","xhigh","max"]},)"
            R"({"value":"haiku","resolvedModel":"claude-haiku-4-5","displayName":"Haiku 4.5","description":"Fastest"}]}}})";
        std::vector<std::string> efforts;
        auto opts = parse_claude_models(minit, &efforts);
        check(opts.size() == 2 && opts[0].value == "opus" && opts[0].label == "Opus 5.5" &&
                  opts[0].detail == "For complex work" && opts[0].resolved == "claude-opus-5-5",
              "initialize models: value, name, description, resolved id");
        check(efforts == std::vector<std::string>({"low", "medium", "high", "xhigh", "max"}),
              "initialize models: effort levels in claude's order");
        set_known_models("claude", opts);
        set_known_efforts("claude", efforts);

        // The transcript may write the model as the resolved id, "claude-"
        // shed; the picker still marks it and shows the description.
        SessionState rs;
        rs.set("model", "model", "opus-5-5");
        cursor = -1;
        picks = chip_pick_items(rs, "model", true, "claude", &cursor, &title);
        bool desc = false, resolved_checked = false;
        for (size_t i = 0; i < picks.size(); i++) {
          if (picks[i].id != "chipset:/model opus") continue;
          desc = picks[i].detail == "For complex work" && picks[i].hint == "opus";
          resolved_checked = picks[i].checked && cursor == int(i);
        }
        check(desc, "chip picker: the description under the name");
        check(resolved_checked, "chip picker: a resolved id still marks its alias");

        // And the effort chip lists what the models reported.
        picks = chip_pick_items(rs, "effort", true, "claude", &cursor, &title);
        int levels = 0;
        bool xhigh = false;
        for (const auto& it : picks)
          if (it.id.rfind("chipset:/effort ", 0) == 0) {
            levels++;
            xhigh |= it.id == "chipset:/effort xhigh";
          }
        check(levels == 5 && xhigh, "chip picker: effort lists the reported levels");
        set_known_efforts("claude", {});
        check(copy_pinned, "chip picker: copy stays offered whatever is typed");

        set_known_efforts("claude", {});
        cursor = -1;
        picks = chip_pick_items(ps, "effort", true, "claude", &cursor, &title);
        check(picks.size() == 4 && picks[1].id == "chipset:/effort medium" && picks[1].checked &&
                  cursor == 1,
              "chip picker: effort falls back to low/medium/high unprobed");

        // The mode ring: each entry carries exactly the presses to reach it.
        SessionState ms;
        ms.set("mode", "mode", "acceptEdits");
        picks = chip_pick_items(ms, "mode", true, "claude", &cursor, &title);
        std::string plan_keys, default_keys;
        for (const auto& it : picks) {
          if (it.id.rfind("chipmode:mode|plan|", 0) == 0) plan_keys = it.id.substr(19);
          if (it.id.rfind("chipmode:mode|default|", 0) == 0) default_keys = it.id.substr(22);
        }
        check(plan_keys == "[Z" && default_keys == "[Z[Z" && cursor == 1,
              "chip picker: the ring sends just enough Shift+Tabs");
      }

      auto live = chip_menu(st, "model", true);
      bool change = false, copy = false;
      for (const auto& it : live) {
        if (it.action == "chipcmd:/model") change = it.enabled;
        if (it.action == "chipcopy:model") copy = it.enabled;
      }
      check(change, "a live agent can be asked to change its model");
      check(copy, "the value can be copied");

      auto stored = chip_menu(st, "model", false);
      for (const auto& it : stored)
        if (it.action == "chipcmd:/model")
          check(!it.enabled, "a stored transcript cannot be commanded");

      // Before any turn the chips still appear, with "?" values, so model and
      // effort can be set on a fresh session.
      ChatRenderer fresh;
      fresh.seed_agent(&claude_adapter());
      Surface strip;
      strip.resize(100, 1);
      std::vector<ChatRenderer::Chip> hits;
      {
        Painter p(strip, Rect{0, 0, 100, 1});
        fresh.render_chips(p, Theme{}, hits);
      }
      check(hits.size() == 4, "a fresh claude session shows four chips");
      std::string row;
      for (int x = 0; x < 100; x++) {
        const Cell& c2 = strip.at(x, 0);
        if (c2.width == 0) continue;
        text::encode(c2.cp ? c2.cp : U' ', row);
      }
      check(row.find("model ?") != std::string::npos, "an unset chip shows a placeholder");
    }

  // Claude records slash-commands and their output as user messages. Those
    // must not read as things the user said.
    auto user_kind = [&](const char* content, Arena& ar) {
      ev.clear();
      std::string rec = std::string(R"({"type":"user","message":{"role":"user","content":")") +
                        content + R"("}})";
      claude_adapter().parse(rec, ar, ev);
      return ev;
    };
    {
      Arena ar;
      auto r = user_kind("<command-name>/model</command-name>\\n<command-args></command-args>", ar);
      check(r.size() == 1 && r[0].kind == EventKind::Meta,
            "a slash command the user ran is hidden, not a chat line");
      r = user_kind("<command-name>/loop</command-name>\\n<command-args>5m go</command-args>", ar);
      check(r.size() == 1 && r[0].kind == EventKind::Meta, "slash command with args is hidden too");

      r = user_kind("<local-command-stdout>Set model to Opus</local-command-stdout>", ar);
      check(r.size() == 1 && r[0].kind == EventKind::Meta, "command output is not a user turn");

      r = user_kind("<local-command-caveat>Caveat: ...</local-command-caveat>", ar);
      check(r.size() == 1 && r[0].kind == EventKind::Meta, "command caveat is not a user turn");

      r = user_kind("real question<system-reminder>ignore me</system-reminder> here", ar);
      check(r.size() == 1 && r[0].kind == EventKind::User, "a turn with a reminder is still a turn");
      if (r.size() == 1)
        check_str(ar.view(r[0].text), "real question here", "injected reminders are stripped");

      // Nothing survives stripping, so nothing is emitted at all.
      r = user_kind("<system-reminder>only machinery</system-reminder>", ar);
      check(r.empty(), "a turn that is only a reminder yields no event");

      r = user_kind("just a normal question", ar);
      check(r.size() == 1 && r[0].kind == EventKind::User, "an ordinary turn is untouched");
      if (r.size() == 1)
        check_str(ar.view(r[0].text), "just a normal question", "ordinary text unchanged");
    }

    // Claude: metadata records produce nothing.
    ev.clear();
    claude_adapter().parse(R"({"type":"mode","mode":"normal"})", a, ev);
    claude_adapter().parse(R"({"type":"ai-title","aiTitle":"x"})", a, ev);
    check(ev.empty(), "metadata records yield no events");

    // Codex: apply_patch keeps its patch body as the expandable detail.
    ev.clear();
    codex_adapter().parse(
        R"J({"timestamp":"t","type":"response_item","payload":{"type":"custom_tool_call",)J"
        R"J("call_id":"c1","name":"exec","input":"tools.apply_patch(\"*** Begin Patch\n)J"
        R"J(+added\n*** End Patch\")"}})J",
        a, ev);
    check(ev.size() == 1 && ev[0].kind == EventKind::ToolCall, "codex tool call");
    if (ev.size() == 1)
      check(a.view(ev[0].detail).find("*** Begin Patch") != std::string_view::npos,
            "apply_patch body kept as diff detail");

    // Codex: exec narrows to the command inside the JS wrapper.
    ev.clear();
    codex_adapter().parse(
        R"J({"type":"response_item","payload":{"type":"custom_tool_call","call_id":"c2",)J"
        R"J("name":"exec","input":"text(await tools.exec_command({cmd:\"ls -la\"}))"}})J",
        a, ev);
    if (ev.size() == 1) check_str(a.view(ev[0].summary), "ls -la", "exec narrows to the command");

    // Codex: everything that is not a response_item is rejected.
    ev.clear();
    codex_adapter().parse(R"({"type":"event_msg","payload":{"type":"token_count"}})", a, ev);
    codex_adapter().parse(R"({"type":"token_usage_record","payload":{}})", a, ev);
    check(ev.empty(), "non response_item records are rejected");

    // Codex: the preamble replayed as a user message is demoted to Meta.
    ev.clear();
    codex_adapter().parse(
        R"({"type":"response_item","payload":{"type":"message","role":"user",)"
        R"("content":[{"type":"input_text","text":"# AGENTS.md instructions for /x"}]}})",
        a, ev);
    check(ev.size() == 1 && ev[0].kind == EventKind::Meta, "preamble demoted to meta");

    // pi / omp: a user turn, an assistant turn with thinking + a tool call,
    // and the matching tool result — the shared schema both agents write.
    ev.clear();
    pi_adapter().parse(
        R"({"type":"message","id":"m1","message":{"role":"user",)"
        R"("content":[{"type":"text","text":"hi there"}]}})",
        a, ev);
    check(ev.size() == 1 && ev[0].kind == EventKind::User, "pi/omp user turn");
    if (ev.size() == 1) check_str(a.view(ev[0].text), "hi there", "pi/omp user text");

    ev.clear();
    pi_adapter().parse(
        R"({"type":"message","id":"m2","message":{"role":"assistant","content":[)"
        R"({"type":"thinking","thinking":"let me check"},)"
        R"({"type":"toolCall","id":"call_1","name":"bash","arguments":{"command":"ls -la"}}]}})",
        a, ev);
    check(ev.size() == 2 && ev[0].kind == EventKind::Thinking && ev[1].kind == EventKind::ToolCall,
          "pi/omp thinking + tool call");
    if (ev.size() == 2) check_str(a.view(ev[1].summary), "ls -la", "pi/omp tool call summary");

    ev.clear();
    pi_adapter().parse(
        R"({"type":"message","id":"m3","message":{"role":"toolResult","toolCallId":"call_1",)"
        R"("toolName":"bash","content":[{"type":"text","text":"file.txt"}],"isError":false}})",
        a, ev);
    check(ev.size() == 1 && ev[0].kind == EventKind::ToolResult && ev[0].ok,
          "pi/omp tool result links to its call");
    {
      ev.clear();
      std::vector<Event> call_ev;
      pi_adapter().parse(
          R"({"type":"message","message":{"role":"assistant","content":[)"
          R"({"type":"toolCall","id":"call_2","name":"edit","arguments":{"input":)"
          R"("*** Begin Patch\n+x\n*** End Patch"}}]}})",
          a, call_ev);
      std::vector<Event> result_ev;
      pi_adapter().parse(
          R"({"type":"message","message":{"role":"toolResult","toolCallId":"call_2",)"
          R"("content":[{"type":"text","text":"ok"}],"isError":false}})",
          a, result_ev);
      check(call_ev.size() == 1 && result_ev.size() == 1 &&
                call_ev[0].tool_id == result_ev[0].tool_id,
            "pi/omp tool call and result share the same hashed id");
      if (call_ev.size() == 1)
        check(a.view(call_ev[0].detail).find("*** Begin Patch") != std::string_view::npos,
              "omp's edit argument is already an apply_patch body");
    }

    // omp's "custom"/"custom_message" telemetry outnumbers the conversation in
    // a real session; none of it should produce an event.
    ev.clear();
    pi_adapter().parse(R"({"type":"custom","customType":"ui_state","data":{}})", a, ev);
    pi_adapter().parse(R"({"type":"title","title":"some title"})", a, ev);
    check(ev.empty(), "pi/omp telemetry records yield no events");

    // The model an agent is actually answering with is picked up from a plain
    // message record, not only from a model_change event.
    {
      SessionState st;
      omp_adapter().observe(
          R"({"type":"message","message":{"role":"assistant","provider":"openai-codex",)"
          R"("model":"gpt-6-astra"}})",
          st);
      omp_adapter().observe(R"({"type":"thinking_level_change","thinkingLevel":"high"})", st);
      check(st.find("model") && *st.find("model") == "gpt-6-astra", "omp model observed");
      check(st.find("effort") && *st.find("effort") == "high", "omp thinking level observed");
    }

    // omp's modes and service tier are chips; leaving one is "default".
    {
      SessionState st;
      omp_adapter().observe(R"({"type":"mode_change","id":"a","mode":"vibe"})", st);
      check(st.find("mode") && *st.find("mode") == "vibe", "omp mode observed");
      omp_adapter().observe(R"({"type":"mode_change","id":"b","mode":"none"})", st);
      check(st.find("mode") && *st.find("mode") == "default", "omp leaving a mode");
      omp_adapter().observe(R"({"type":"service_tier_change","serviceTier":{"openai":"priority"}})", st);
      check(st.find("tier") && *st.find("tier") == "priority", "omp service tier observed");
      omp_adapter().observe(R"({"type":"service_tier_change","serviceTier":null})", st);
      check(st.find("tier") && *st.find("tier") == "default", "omp service tier cleared");
    }

    // omp states an intent ("i") on every call; eval's argument is its code.
    {
      ev.clear();
      omp_adapter().parse(
          R"({"type":"message","message":{"role":"assistant","content":[)"
          R"({"type":"toolCall","id":"e1","name":"eval","arguments":{"language":"py","code":"print 1"}},)"
          R"({"type":"toolCall","id":"h1","name":"hub","arguments":{"i":"Awaiting workers","op":"wait"}}]}})",
          a, ev);
      check(ev.size() == 2, "omp eval + hub calls");
      if (ev.size() == 2) {
        check_str(a.view(ev[0].summary), "print 1", "omp eval shows its code");
        check_str(a.view(ev[1].summary), "Awaiting workers", "omp call without a ranked key shows its intent");
      }
    }

    // omp's own edit language: the summary names the files; the result's
    // numbered diff is what the result shows.
    {
      ev.clear();
      omp_adapter().parse(
          R"({"type":"message","message":{"role":"assistant","content":[)"
          R"({"type":"toolCall","id":"ed1","name":"edit","arguments":{"input":)"
          R"("*** Begin Patch\n[src/a.py#B766]\nINS.POST 40:\n+x\n[src/b.cpp#00FE]\nDEL 3\n[src/a.py#C001]\nCUT 1.=2\n*** End Patch\n"}}]}})",
          a, ev);
      check(ev.size() == 1 && ev[0].kind == EventKind::ToolCall, "omp hashline edit call");
      if (ev.size() == 1) {
        check_str(a.view(ev[0].summary), "src/a.py, src/b.cpp", "omp edit summary names its files");
        check(a.view(ev[0].detail).starts_with("*** Begin Patch\n[src/a.py#B766]"), "omp edit script kept as detail");
      }
      ev.clear();
      omp_adapter().parse(
          R"({"type":"message","message":{"role":"toolResult","toolCallId":"ed1","toolName":"edit",)"
          R"("content":[{"type":"text","text":"[src/a.py#AE82]\n40:x"}],)"
          R"("details":{"diff":" 39|a\n+40|x","op":"update","path":"src/a.py"},"isError":false}})",
          a, ev);
      check(ev.size() == 1 && ev[0].kind == EventKind::ToolResult, "omp edit result");
      if (ev.size() == 1) {
        check_str(a.view(ev[0].detail), " 39|a\n+40|x", "omp edit result carries its diff");
        check_str(a.view(ev[0].text), "[src/a.py#AE82]\n40:x", "omp edit result keeps its text");
      }
      ev.clear();
      omp_adapter().parse(
          R"({"type":"message","message":{"role":"toolResult","toolCallId":"b1","toolName":"bash",)"
          R"("content":[{"type":"text","text":"ok"}],"details":{"diff":"+x"},"isError":false}})",
          a, ev);
      check(ev.size() == 1 && ev[0].detail.empty(), "only an edit result's diff is shown");
      check_str(edit_script_paths("*** Begin Patch\n*** Update File: x.c\n@@\n-a\n+b\n*** End Patch"), "x.c",
                "apply_patch headers name the files too");
    }

    // What omp shows of its own messages: a compaction, finished background
    // jobs, other agents' messages, a collab guest's prompt — not reminders.
    {
      ev.clear();
      omp_adapter().parse(R"({"type":"compaction","id":"c","summary":"long summary"})", a, ev);
      check(ev.size() == 1 && ev[0].kind == EventKind::Notice, "omp compaction is a notice");
      ev.clear();
      omp_adapter().parse(
          R"({"type":"custom_message","customType":"async-result","content":"<system-notice>\nBackground job )"
          R"(Review has completed. Resume your work using the result below.\n<task-result id=\"Review\">)",
          a, ev);
      omp_adapter().parse(
          R"({"type":"custom_message","customType":"async-result","content":"<system-notice>\nBackground job )"
          R"(Review has completed. Resume your work using the result below.\n<task-result id=\"Review\">",)"
          R"("display":true,"attribution":"agent"})",
          a, ev);
      check(ev.size() == 1 && ev[0].kind == EventKind::TaskStatus && ev[0].ok, "omp background job finished");
      if (ev.size() == 1) check_str(a.view(ev[0].text), "Background job Review has completed.", "omp job notice text");
      ev.clear();
      omp_adapter().parse(
          R"({"type":"custom_message","customType":"irc:incoming","content":"<irc>\n…</irc>","display":true,)"
          R"("details":{"id":"1","from":"Fixer","message":"Done.\nNo tests run."},"attribution":"agent"})",
          a, ev);
      check(ev.size() == 1 && ev[0].kind == EventKind::TaskStatus, "omp agent message");
      if (ev.size() == 1) check_str(a.view(ev[0].text), "Fixer: Done.\nNo tests run.", "omp agent message text");
      ev.clear();
      omp_adapter().parse(
          R"({"type":"custom_message","customType":"collab-prompt","content":"How is training going?",)"
          R"("display":true,"details":{"from":"guest"},"attribution":"user"})",
          a, ev);
      check(ev.size() == 1 && ev[0].kind == EventKind::User, "omp collab guest prompt is a user turn");
      ev.clear();
      omp_adapter().parse(
          R"({"type":"custom_message","customType":"mid-run-todo-nudge","content":"<system-reminder>x</system-reminder>",)"
          R"("display":false,"attribution":"agent"})",
          a, ev);
      check(ev.empty(), "omp hidden reminders stay hidden");
    }

    // Where pi and omp keep things, as their environments say, and omp's
    // subagent runs listed as chats of their own.
    {
      const char* vars[] = {"HOME", "PI_CODING_AGENT_DIR", "PI_CODING_AGENT_SESSION_DIR", "OMP_PROFILE",
                            "PI_PROFILE", "PI_CONFIG_DIR", "XDG_DATA_HOME"};
      std::vector<std::pair<std::string, std::string>> saved;
      for (const char* v : vars) {
        const char* had = getenv(v);
        saved.push_back({v, had ? std::string("=") + had : std::string()});
        if (std::string_view(v) != "HOME") unsetenv(v);
      }
      const std::string root = "/tmp/mico-selftest-omp-" + std::to_string(getpid());
      setenv("HOME", root.c_str(), 1);
      const auto& pi = static_cast<const PiFamilyAdapter&>(pi_adapter());
      const auto& omp = static_cast<const PiFamilyAdapter&>(omp_adapter());
      check_str(omp.agent_dir(root), root + "/.omp/agent", "omp agent dir by default");
      check_str(omp.sessions_dir(), root + "/.omp/agent/sessions", "omp sessions by default");
      setenv("PI_CODING_AGENT_DIR", "~/elsewhere", 1);
      check_str(pi.agent_dir(root), root + "/elsewhere", "pi honours PI_CODING_AGENT_DIR");
      check_str(omp.agent_dir(root), root + "/elsewhere", "omp honours PI_CODING_AGENT_DIR");
      setenv("PI_PROFILE", "work", 1);
      check_str(omp.agent_dir(root), root + "/.omp/profiles/work/agent", "an omp profile wins");
      setenv("OMP_PROFILE", "default", 1);
      check_str(omp.agent_dir(root), root + "/elsewhere", "OMP_PROFILE=default is no profile, over PI_PROFILE");
      unsetenv("OMP_PROFILE");
      unsetenv("PI_PROFILE");
      unsetenv("PI_CODING_AGENT_DIR");
      fs::make_dirs(root + "/xdg/omp");
      setenv("XDG_DATA_HOME", (root + "/xdg").c_str(), 1);
      check_str(omp.sessions_dir(), root + "/xdg/omp/sessions", "omp sessions under an existing XDG data dir");
      unsetenv("XDG_DATA_HOME");

      const std::string slug = root + "/.omp/agent/sessions/-work";
      const std::string parent = slug + "/2026-09-30T17-21-31-812Z_p1.jsonl";
      fs::make_dirs(slug + "/2026-09-30T17-21-31-812Z_p1");
      put_file(parent, R"({"type":"title","v":1,"title":"Review the strategy"})" "\n"
                       R"({"type":"session","version":3,"id":"p1","cwd":"/work"})" "\n");
      put_file(slug + "/2026-09-30T17-21-31-812Z_p1/MarketReview.jsonl",
               R"({"type":"title","v":1,"title":""})" "\n"
               R"({"type":"session","version":3,"id":"s1","cwd":"/work","parentSession":")" + parent + "\"}\n" +
               R"({"type":"message","message":{"role":"user","content":[{"type":"text","text":"Complete assignment"}]}})" "\n");
      std::map<std::string, std::string> titles;
      omp.list_sessions([&](SessionRef&& r) { titles[r.id] = r.title; });
      check(titles.size() == 2, "omp lists a session and its subagent run");
      check_str(titles["p1"], "Review the strategy", "omp session title");
      check_str(titles["s1"], "↳ MarketReview · Review the strategy", "omp subagent run named for its agent and parent");
      Launch l;
      omp.continue_session(l, "s1", false, nullptr);
      check(l.argv.size() == 3 && l.argv[2] == slug + "/2026-09-30T17-21-31-812Z_p1/MarketReview.jsonl",
            "a subagent run resumes by its path");
      omp.continue_session(l, "p1", false, nullptr);
      check(l.argv.size() == 3 && l.argv[2] == "p1", "a session resumes by its id");

      // A flat session-dir override, from the command line.
      fs::make_dirs(root + "/flat");
      put_file(root + "/flat/x_f1.jsonl", R"({"type":"session","version":3,"id":"f1","cwd":"/work"})" "\n");
      std::vector<std::string> seen;
      omp_adapter().snapshot_transcripts({"omp", "--session-dir", "flat"}, root, seen);
      check(std::find(seen.begin(), seen.end(), root + "/flat/x_f1.jsonl") != seen.end() &&
                std::find(seen.begin(), seen.end(), parent) != seen.end() && seen.size() == 2,
            "omp --session-dir is looked in, subagent runs are not new sessions");

      if (system(("rm -rf '" + root + "'").c_str()) != 0) {}
      for (auto& [k, v] : saved) {
        if (v.empty()) unsetenv(k.c_str());
        else setenv(k.c_str(), v.c_str() + 1, 1);
      }
    }
  }

  // pi and omp: mico's plot tool through its extension, and what each agent
  // starts in the background, read from its transcript.
  {
    Arena a;
    std::vector<Event> ev;
    pi_adapter().parse(
        R"({"type":"message","message":{"role":"assistant","content":[)"
        R"({"type":"toolCall","id":"p1","name":"mico_plot","arguments":{"type":"line","x":[0,1],"y":[1,2]}}]}})",
        a, ev);
    check(ev.size() == 1 && ev[0].kind == EventKind::Chart &&
              a.view(ev[0].text) == "```chart\n{\"type\":\"line\",\"x\":[0,1],\"y\":[1,2]}\n```",
          "pi: mico_plot is a chart");
    ev.clear();
    omp_adapter().parse(
        R"({"type":"message","message":{"role":"assistant","content":[)"
        R"({"type":"toolCall","id":"p2","name":"write","arguments":{"path":"xd://mico_plot",)"
        R"("content":"{\"type\":\"bar\",\"labels\":[\"a\"],\"series\":[{\"y\":[1]}]}"}}]}})",
        a, ev);
    check(ev.size() == 1 && ev[0].kind == EventKind::Chart &&
              a.view(ev[0].text) == "```chart\n{\"type\":\"bar\",\"labels\":[\"a\"],\"series\":[{\"y\":[1]}]}\n```",
          "omp: a write to xd://mico_plot is a chart");
    std::vector<LineChanges> lc;
    omp_adapter().read_changes(
        R"({"type":"message","message":{"role":"assistant","content":[)"
        R"({"type":"toolCall","id":"p2","name":"write","arguments":{"path":"xd://mico_plot","content":"{}"}}]}})",
        "/w", true, lc);
    check(lc.empty(), "omp: a write to xd:// changes no file");

    BackgroundTasks t;
    const auto run = [&](std::string_view line) { omp_adapter().read_background(line, 0, t); };
    const auto ids = [&] {
      std::string s;
      for (const auto& r : t.running) s += (s.empty() ? "" : ",") + r.id + ":" + r.kind;
      return s;
    };
    run(R"({"type":"message","message":{"role":"assistant","content":[{"type":"toolCall","id":"c1","name":"bash",)"
        R"("arguments":{"i":"Building the probes","command":"make","async":true}}]}})");
    run(R"({"type":"message","message":{"role":"toolResult","toolCallId":"c1","toolName":"bash",)"
        R"("content":[{"type":"text","text":"Backgrounded as job bg_1"}],)"
        R"("details":{"async":{"state":"running","jobId":"bg_1","type":"bash"}}}})");
    check(ids() == "bg_1:shell" && t.running[0].what == "Building the probes", "omp: an async bash job runs");
    run(R"({"type":"message","message":{"role":"toolResult","toolCallId":"c2","toolName":"task","content":[],)"
        R"("details":{"async":{"state":"running","jobId":"Review","type":"task"},)"
        R"("progress":[{"id":"Review","agent":"reviewer"},{"id":"Audit","agent":"task"}]}}})");
    check(ids() == "bg_1:shell,Review:agent,Audit:agent", "omp: each of a task's subagents runs");
    run(R"({"type":"custom_message","customType":"async-result","content":"…","display":true,)"
        R"("details":{"jobs":[{"jobId":"bg_1","type":"bash"}]}})");
    run(R"({"type":"message","message":{"role":"toolResult","toolCallId":"c3","toolName":"wait","content":[],)"
        R"("details":{"op":"wait","jobs":[{"id":"Review","status":"completed"},{"id":"Audit","status":"running"}]}}})");
    check(ids() == "Audit:agent", "omp: a delivery and a jobs snapshot end what finished");
    run(R"({"type":"custom","customType":"vibe-session-lifecycle","data":{"id":"v1","action":"turn-started"}})");
    run(R"({"type":"message","message":{"role":"toolResult","toolCallId":"c4","toolName":"hub","content":[],)"
        R"("details":{"op":"start","daemon":{"name":"server","state":"running"}}}})");
    check(ids() == "Audit:agent,v1:agent,server:shell", "omp: vibe turns and hub services run");
    run(R"({"type":"custom","customType":"vibe-session-lifecycle","data":{"id":"v1","action":"turn-settled"}})");
    run(R"({"type":"message","message":{"role":"toolResult","toolCallId":"c5","toolName":"hub","content":[],)"
        R"("details":{"op":"stop","daemon":{"name":"server","state":"stopped"}}}})");
    check(ids() == "Audit:agent", "omp: a settled turn and a stopped service end");
  }

  // Codex: a yielded code cell until a wait sees it finish, and a command
  // still running when its call returned until its item completes, even when
  // codex writes the completion first.
  {
    BackgroundTasks t;
    const auto run = [&](std::string_view line) { codex_adapter().read_background(line, 0, t); };
    const auto ids = [&] {
      std::string s;
      for (const auto& r : t.running) s += (s.empty() ? "" : ",") + r.id;
      return s;
    };
    run(R"({"timestamp":"2026-10-01T10:00:00Z","type":"response_item","payload":{"type":"custom_tool_call",)"
        R"j("call_id":"x1","name":"exec","input":"text(await tools.exec_command({cmd:\"make test\"}))"}})j");
    run(R"({"timestamp":"2026-10-01T10:00:31Z","type":"response_item","payload":{"type":"custom_tool_call_output",)"
        R"("call_id":"x1","output":"Script running with cell ID 3\nWall time 31.0 seconds\nOutput:\n"}})");
    check(ids() == "cell 3" && t.running[0].what == "make test", "codex: a yielded cell runs, named by its command");
    run(R"({"type":"response_item","payload":{"type":"function_call","call_id":"w1","name":"wait",)"
        R"("arguments":"{\"cell_id\":\"3\",\"yield_time_ms\":1000}"}})");
    run(R"({"type":"response_item","payload":{"type":"function_call_output","call_id":"w1",)"
        R"("output":"Script completed\nWall time 0.0 seconds\nOutput:\n"}})");
    check(ids().empty(), "codex: a wait that sees the cell complete ends it");
    run(R"({"type":"response_item","payload":{"type":"custom_tool_call","call_id":"x2","name":"exec",)"
        R"j("input":"text(await tools.exec_command({cmd:\"npm run dev\"}))"}})j");
    run(R"({"type":"response_item","payload":{"type":"custom_tool_call_output","call_id":"x2","output":[)"
        R"({"type":"input_text","text":"Script completed\nOutput:\n"},)"
        R"({"type":"input_text","text":"{\"chunk_id\":\"a\",\"session_id\":4242,\"output\":\"\"}"}]}})");
    check(ids() == "process 4242", "codex: a command still running after its call");
    run(R"({"type":"event_msg","payload":{"type":"item_completed","item":{"type":"CommandExecution",)"
        R"("process_id":"4242","status":"completed"}}})");
    check(ids().empty(), "codex: its completed item ends it");
    run(R"({"type":"event_msg","payload":{"type":"item_completed","item":{"type":"CommandExecution",)"
        R"("process_id":"77","status":"completed"}}})");
    run(R"({"type":"response_item","payload":{"type":"custom_tool_call","call_id":"x3","name":"exec","input":"x"}})");
    run(R"({"type":"response_item","payload":{"type":"custom_tool_call_output","call_id":"x3","output":[)"
        R"({"type":"input_text","text":"{\"chunk_id\":\"b\",\"session_id\":77,\"output\":\"\"}"}]}})");
    check(ids().empty(), "codex: a completion written before the output still ends it");
  }

  // Codex's folder trust dialog, answered as claude's is; and walking its
  // numbered menus by name.
  {
    Vt vt;
    vt.resize(80, 14);
    vt.write("  Folder access\r\n  /work\r\n\r\n  Trust this folder? Codex can read, edit, and run files here.\r\n\r\n"
             "  1. Trust and continue\r\n\xE2\x80\xBA 2. Back to Agent Command Center\r\n\r\n  enter continue \xC2\xB7 esc back");
    bool confirms = true;
    check(codex_adapter().startup_prompt(vt), "codex: its trust dialog is a startup prompt");
    check(codex_adapter().startup_answer(vt, &confirms) == "\x1b[A" && !confirms,
          "codex: the cursor is moved to Trust first");
    Vt on;
    on.resize(80, 14);
    on.write("  Trust this folder?\r\n\xE2\x80\xBA 1. Trust and continue\r\n  2. Back\r\n\r\n  enter continue \xC2\xB7 esc back");
    check(codex_adapter().startup_answer(on, &confirms) == "\r" && confirms, "codex: Enter on Trust and continue");
    Vt quoted;
    quoted.resize(80, 8);
    quoted.write("\xE2\x80\xA2 It asked: Trust this folder?\r\n\r\n\xE2\x80\xBA Ask Codex to do anything");
    check(!codex_adapter().startup_prompt(quoted), "codex: a reply quoting the dialog is not it");

    Vt menu;
    menu.resize(100, 12);
    menu.write("  Select Model and Effort\r\n\r\n  1. GPT-6.1-Sol (default)  Latest workhorse.\r\n"
               "\xE2\x80\xBA 4. GPT-6-Luna (current)   Fast and affordable.\r\n  5. More reasoning\xE2\x80\xA6   Max and Ultra.\r\n");
    bool again = false;
    check(pick_key(menu, "GPT-6-Luna", &again) == "4" && !again, "pick: a row by its name, marks aside");
    check(pick_key(menu, "GPT-6", &again).empty(), "pick: a name is whole, not a prefix");
    check(pick_key(menu, "Max|More reasoning", &again) == "5" && again, "pick: a submenu first, then again");
    check(pick_key(menu, "Ultra", &again).empty(), "pick: nothing to press when the row is not there");
  }

  // The reply pi and omp are writing, read off their screens: plain text one
  // column in, under a message in its own background or grey italic thinking.
  {
    const std::string rule = [] {
      std::string r;
      for (int i = 0; i < 30; i++) r += "\xE2\x94\x80";
      return r;
    }();
    const std::string user = "\x1b[48;2;52;53;65m Say hi                       \x1b[49m\r\n";
    const std::string think = "\x1b[3;38;2;128;128;128m Thinking about it\x1b[0m\r\n";
    const std::string reply = " Hello \x1b[1mthere\x1b[22m, this is a reply\r\n that wraps.\r\n";
    Vt pi;
    pi.resize(30, 14);
    pi.write(user + "\r\n" + think + "\r\n" + reply + "\r\n\xE2\x94\x80\xE2\x94\x80 \xE2\xA0\xB9 Working " + rule.substr(0, 45) +
             "\r\n\r\n" + rule + "\r\n~/work");
    check_str(pi_adapter().screen_reply(pi), "Hello **there**, this is a reply that wraps.", "pi: the reply off its screen");
    Vt thinking;
    thinking.resize(30, 10);
    thinking.write(user + "\r\n" + think + "\r\n" + rule + "\r\n\r\n" + rule + "\r\n~/work");
    check(pi_adapter().screen_reply(thinking).empty(), "pi: thinking is not a reply");
    Vt omp;
    omp.resize(30, 14);
    omp.write(user + "\r\n" + reply + "\r\n  \xE2\x8E\x8B Working\xE2\x80\xA6\r\n\x1b[48;2;15;18;22m \xE2\xA0\xB4 4s > model          \x1b[49m\r\n"
              "\xE2\x95\xB0\xE2\x94\x80");
    check_str(omp_adapter().screen_reply(omp), "Hello **there**, this is a reply that wraps.", "omp: the reply off its screen");
  }

  // The models each agent lists on the command line, and the chips that set
  // them: through a completion menu for pi and omp, through codex's menus.
  {
    CommandProbeAnswer got;
    check(!pi_adapter().read_command_probe("provider  model\n", false, got), "probe: read only once it has ended");
    check(pi_adapter().read_command_probe(
              "provider      model                context  max-out  thinking  images\n"
              "deepseek      deepseek-flash       1M       384K     yes       yes\n"
              "openai-codex  gpt-5.5              272K     128K     yes       yes\n",
              true, got) &&
              got.models.size() == 2 && got.models[1].value == "openai-codex/gpt-5.5" && got.models[1].resolved == "gpt-5.5",
          "pi: --list-models, as provider/model");
    check(omp_adapter().read_command_probe(
              R"({"models":[{"provider":"deepseek","kind":"chat","id":"deepseek-flash","selector":"deepseek/deepseek-flash",)"
              R"("name":"DeepSeek Flash","thinking":["low","high","max"]},{"provider":"x","kind":"embedding","id":"e","selector":"x/e"}]})",
              true, got) &&
              got.models.size() == 1 && got.models[0].label == "DeepSeek Flash" &&
              got.models[0].efforts == std::vector<std::string>({"off", "low", "high", "max"}),
          "omp: models --json, chat models with their levels");
    check(codex_adapter().read_command_probe(
              R"({"models":[{"slug":"gpt-6-sol","display_name":"GPT-6-Sol","description":"Workhorse","visibility":"list",)"
              R"("supported_reasoning_levels":[{"effort":"low"},{"effort":"max"}]},{"slug":"hidden","visibility":"hide"}]})",
              true, got) &&
              got.models.size() == 1 && got.models[0].label == "GPT-6-Sol" &&
              got.models[0].efforts == std::vector<std::string>({"low", "max"}),
          "codex: debug models, those its picker lists");
    set_known_models("codex", got.models);

    SessionState st;
    st.set("model", "model", "gpt-6-sol");
    st.set("effort", "effort", "max");
    int cursor = -1;
    std::string title;
    auto picks = chip_pick_items(st, "effort", true, "codex", &cursor, &title);
    std::string max_steps;
    for (const auto& it : picks)
      if (it.id.starts_with("chipsteps:effort|max|")) max_steps = it.id.substr(21);
    check(picks.size() >= 2 && picks[0].id.starts_with("chipsteps:effort|low|") && picks[0].label == "Low",
          "codex: the effort chip offers the model's own levels, by codex's names");
    check(max_steps == "/model\x1f\r\x1f\x01pick:GPT-6-Sol\x1f\x01pick:Max|More reasoning",
          "codex: an effort is set through /model's menus, never as a message");
    picks = chip_pick_items(st, "model", true, "codex", &cursor, &title);
    check(!picks.empty() && picks[0].id == "chipsteps:model|gpt-6-sol|/model\x1f\r\x1f\x01pick:GPT-6-Sol\x1f\x01pick:Max|More reasoning",
          "codex: a model keeps the effort in use");

    SessionState os;
    os.set("model", "model", "deepseek-flash");
    picks = chip_pick_items(os, "effort", true, "omp", &cursor, &title);
    check(picks.size() == 1 && !picks[0].enabled, "omp: no thinking level without the provider to /switch with");
    os.set("provider", "provider", "deepseek");
    picks = chip_pick_items(os, "effort", true, "omp", &cursor, &title);
    check(!picks.empty() && picks[0].id == "chipsteps:effort|off|/switch deepseek/deepseek-flash:off\x1f\r\x1f\r",
          "omp: a thinking level through /switch, past its completion menu");
    picks = chip_pick_items(os, "effort", true, "pi", &cursor, &title);
    check(!picks.empty() && picks[0].id == "chipsteps:effort|off|/thinking off\x1f\r\x1f\r", "pi: /thinking with its level");

    // What mico gives pi and omp at launch: its extension and its hints.
    Launch l;
    LaunchExtras x;
    x.hints = "HINTS.";
    x.tool_extension = "/state/mico-tools.js";
    omp_adapter().prepare(l, x);
    check(l.argv.size() == 5 && l.argv[0] == "omp" && l.argv[1] == "--append-system-prompt" &&
              l.argv[2].starts_with("HINTS. You also have mico's `mico_plot` tool") && l.argv[3] == "-e" &&
              l.argv[4] == "/state/mico-tools.js",
          "omp: launched with mico's extension and hints");
    Launch own;
    own.argv = {"omp", "--append-system-prompt", "mine"};
    omp_adapter().prepare(own, x);
    check(own.argv.size() == 5 && own.argv[4] == "mine", "omp: a system prompt of the user's own is left alone");
    check(tool_extension_source("/bin/mico").find("const MICO = \"/bin/mico\";") != std::string::npos,
          "the extension runs this mico");
  }

  // The bulk-ASCII path in the emulator must produce exactly what the
  // per-character path produces. Feeding the same bytes one at a time forces
  // the slow path, so the two can be compared directly. The corpus is built
  // here rather than read from a captured file: a corpus that can go missing
  // makes this pass by comparing two empty screens.
  {
    std::string blob;
    blob += "\x1b[?1049h\x1b[2J";                       // alt screen, cleared
    blob += "\x1b(B\x0f";                                // charset designator
    blob += "\x1b[0;38;2;200;206;218;48;2;22;25;34m";    // truecolor SGR
    blob += "plain ascii line, long enough to exercise the bulk path\r\n";
    blob += "\x1b]8;id=1;https://example.com\x07link\x1b]8;;\x07\r\n";  // OSC, BEL
    blob += "\x1b]0;a title\x1b\\";                        // OSC, ST terminator
    blob += "\x1b[5;3Hplaced\x1b[1mbold\x1b[0m\ttab\rreturn\r\n";
    blob += "wide \xE4\xB8\xAD\xE6\x96\x87 then ascii again\r\n";
    blob += "\x1b[7mreverse\x1b[27m\r\n";
    blob += std::string(90, 'W') + "\r\n";                // wrapping
    blob += "\x1b[2A\x1b[3Cmoved\r\n";

    Vt bulk, slow;
    bulk.resize(80, 24);
    slow.resize(80, 24);
    bulk.write(blob);
    for (char c : blob) slow.write(std::string_view(&c, 1));

    int diff = 0, filled = 0;
    for (int y = 0; y < 24; y++) {
      const VtRow& a = bulk.row(bulk.total_rows() - 24 + y);
      const VtRow& b = slow.row(slow.total_rows() - 24 + y);
      for (size_t x = 0; x < a.size() && x < b.size(); x++) {
        if (a[x].cp != b[x].cp || a[x].width != b[x].width || !(a[x].st == b[x].st)) diff++;
        if (a[x].cp != U' ' && a[x].cp != 0) filled++;
      }
    }
    check(diff == 0, "bulk and per-character emulation agree");
    check(filled > 100, "the emulation corpus actually renders something");
    if (diff) printf("        %d differing cells\n", diff);
  }

  // scroll_up recycles row storage (evicted screen row -> scrollback, aged-out
  // scrollback buffer -> new blank bottom row). Every line has to survive that
  // shuffle intact: nothing duplicated, nothing dropped, order preserved.
  {
    Vt vt;
    vt.resize(20, 5);
    for (int i = 0; i < 400; i++) {
      char ln[24];
      int n = snprintf(ln, sizeof ln, "line %d\r\n", i);
      vt.write(std::string_view(ln, size_t(n)));
    }
    auto row_num = [](const VtRow& r) {
      std::string s;
      for (const Cell& c : r)
        if (c.cp >= '0' && c.cp <= '9') s.push_back(char(c.cp));
        else if (!s.empty()) break;
      return s.empty() ? -1 : std::stoi(s);
    };
    // 400 lines written, one trailing blank from the last \n: rows 0..399 are
    // all above the cursor, so the last visible line is 399 minus the blank.
    int last = -1, contiguous = 1;
    for (int y = 0; y < vt.total_rows(); y++) {
      int v = row_num(vt.row(y));
      if (v < 0) continue;
      if (last >= 0 && v != last + 1) contiguous = 0;
      last = v;
    }
    check(contiguous, "scrolled lines stay in order with no gaps");
    check(last == 399, "the most recent line is retained after scrolling");
    check(vt.total_rows() > 5, "scrolled-off lines land in scrollback");
  }

  // Dragging the scrollbar seeks by byte offset into a file the index has only
  // partly covered. Asking for a position and reading the thumb back has to
  // agree, or the thumb walks away from the pointer.
  {
    Store store;
    store.add_folder("/home/bamedeo/Desktop/kaggle/kaggriculture", false);
    store.scan();
    const SessionRef* big = nullptr;
    for (const auto& pr : store.projects())
      for (const auto& se : pr.sessions)
        if (!big || se.bytes > big->bytes) big = &se;

    if (big && Store::adapter_for(*big)) {
      ChatRenderer c;
      c.open(big->path, Store::adapter_for(*big));
      Theme theme;
      Filters filters;
      Surface sf;
      sf.resize(100, 24);
      auto frame = [&] {
        Painter p(sf, Rect{0, 0, 100, 24});
        c.render(p, theme, filters);
      };
      auto rows_with_text = [&] {
        int n = 0;
        for (int y = 0; y < 24; y++)
          for (int x = 0; x < 100; x++) {
            const Cell& cell = sf.at(x, y);
            if (cell.width && cell.cp != U' ' && cell.cp != 0) { n++; break; }
          }
        return n;
      };
      frame();
      for (double want : {0.0, 0.25, 0.5, 0.9}) {
        c.seek_fraction(want);
        frame();
        const double got = c.thumb_fraction();
        check(std::abs(got - want) < 0.05, "seeking lands where it was asked to");
        // Seeking to the very start used to leave an empty screen: the opening
        // lines of a rollout carry no conversation and the window only grew
        // backwards.
        check(rows_with_text() > 3, "a seek fills the screen");
        if (std::abs(got - want) >= 0.05 || rows_with_text() <= 3)
          printf("        asked %.2f, landed %.3f, %d rows\n", want, got, rows_with_text());
      }
    }
  }

  // A model question renders as a card and, when live and single-question, is
  // answerable from the chat with the keyboard.
  {
    const std::string path = "/tmp/mico_question_test.jsonl";
    const std::string line =
        R"({"type":"message","message":{"role":"assistant","content":[{"type":"toolCall","name":"ask","id":"call_1","arguments":{"questions":[{"header":"Library","question":"Which library?","multi":false,"recommended":1,"options":[{"label":"date-fns","description":"small"},{"label":"luxon","description":"rich"}]}]}}]}})"
        "\n";
    if (FILE* f = fopen(path.c_str(), "wb")) {
      fwrite(line.data(), 1, line.size(), f);
      fclose(f);
    }

    ChatRenderer c;
    check(c.open(path, &pi_adapter()), "a question transcript opens");
    c.set_questions_interactive(true);
    Theme theme;
    Filters filters;
    Surface sf;
    sf.resize(60, 12);
    auto frame = [&] {
      Painter p(sf, Rect{0, 0, 60, 12});
      c.render(p, theme, filters);
    };
    frame();

    std::string screen;
    for (int y = 0; y < 12; y++) {
      for (int x = 0; x < 60; x++) {
        const Cell& cell = sf.at(x, y);
        if (cell.width == 0) continue;
        text::encode(cell.cp ? cell.cp : U' ', screen);
      }
      screen.push_back('\n');
    }
    check(screen.find("Which library?") != std::string::npos,
          "the question text is rendered");
    check(screen.find("date-fns") != std::string::npos &&
              screen.find("luxon") != std::string::npos,
          "the options are rendered");
    check(screen.find("\xE2\x9D\xAF") != std::string::npos,
          "a live card draws its option cursor");
    check(screen.find("\xE2\x97\x8B") != std::string::npos,
          "a live card draws its option markers");
    check(c.has_pending_question(), "an unanswered question is pending");
    check(c.question_active(), "a single-question card is answerable here");

    ChatRenderer::Answer a;
    check(!c.take_answer(a), "nothing is answered before a choice");
    check(c.question_key(KeyEvent{Key::Enter}), "enter answers the focused option");
    check(c.take_answer(a) && a.chosen.size() == 1 && a.chosen[0].size() == 2 &&
              a.chosen[0][1] == 1,
          "the recommended option starts focused and is chosen");
    check(!c.question_active(), "an answered card stops taking keys");

    // The keystrokes that drive the agent's own menu: walk the cursor to the
    // chosen option, then confirm, including each agent's Submit behaviour.
    check_str(question_answer_keys(*adapter_for("codex"), {false}, {0}, {3}, {{0, 0, 1}}),
              "\x1b[B\x1b[B\r", "a single-select walks down then confirms");
    check_str(question_answer_keys(*adapter_for("codex"), {false}, {1}, {3}, {{0, 1, 0}}), "\r",
              "the recommended option needs no movement");
    check_str(question_answer_keys(*adapter_for("claude"), {false}, {0}, {2}, {{1, 0}}), "\r",
              "claude submits a single choice without leaking an extra Enter");
    check_str(question_answer_keys(*adapter_for("claude"), {true}, {0}, {3}, {{1, 0, 1}}),
              " \x1b[B\x1b[B \x1b[B\x1b[B\r\r",
              "claude multi-select reaches its Submit button after Other before confirming review");
    check_str(question_answer_keys(*adapter_for("claude"), {false, false}, {1, 1}, {2, 2}, {{0, 1}, {1, 0}}),
              "\x1b[B\r\r\r", "claude starts each question on its first option, then confirms review");
    check_str(question_answer_keys(*adapter_for("omp"), {true}, {0}, {3}, {{0, 1, 1}}),
              "\x1b[B \x1b[B \r", "a multi-select toggles each chosen option");
    check_str(question_answer_keys(*adapter_for("codex"), {false, false}, {0, 1}, {2, 2},
                                   {{1, 0}, {0, 1}}),
              "\r\r\r", "two questions confirm in turn then submit");
    check_str(question_answer_keys(*adapter_for("codex"), {false, false}, {0, 1}, {2, 2},
                                   {{0, 1}, {1, 0}}),
              "\x1b[B\r\x1b[A\r\r", "each question walks its own cursor");
  }

  // mico's MCP server (`mico --mcp`): the handshake, the plot tool, and its
  // answers, one JSON message per line.
  {
    const auto call = [](std::string_view msg) { return mcp_handle(msg, "/tmp"); };
    std::string r = call(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05"}})");
    check(r.find("\"protocolVersion\":\"2024-11-05\"") != std::string::npos && r.find("\"tools\"") != std::string::npos,
          "mcp: initialize answers in the client's protocol version, with tools");
    check(call(R"({"jsonrpc":"2.0","method":"notifications/initialized"})").empty(), "mcp: notifications get no reply");
    r = call(R"({"jsonrpc":"2.0","id":"a","method":"tools/list"})");
    check(r.find("\"name\":\"plot\"") != std::string::npos && r.find('\n') == std::string::npos &&
              r.find("\"id\":\"a\"") != std::string::npos,
          "mcp: tools/list offers plot, on one line, under the caller's id");
    r = call(R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"plot","arguments":{"title":"sq","y":[0,1,4]}}})");
    check(r.find("\"isError\":false") != std::string::npos && r.find("Drawn") != std::string::npos,
          "mcp: a good plot is drawn");
    r = call(R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"plot","arguments":{"type":"line"}}})");
    check(r.find("\"isError\":true") != std::string::npos && r.find("needs data") != std::string::npos,
          "mcp: a bad plot says why, as a tool error the agent can fix");
    r = call(R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"plot","arguments":{"file":"no-such.csv","y":"a"}}})");
    check(r.find("\"isError\":true") != std::string::npos && r.find("no such file") != std::string::npos,
          "mcp: a file chart checks its file");
    check(call(R"({"jsonrpc":"2.0","id":6,"method":"nope"})").find("-32601") != std::string::npos,
          "mcp: unknown methods are errors");
    r = call(R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"plot","arguments":{"subplots":[{"y":[1,2]},{"type":"bar","labels":["a"],"series":[{"y":[3]}]}]}}})");
    check(r.find("\"isError\":false") != std::string::npos && r.find("figure of 2 charts (line, bar)") != std::string::npos,
          "mcp: a figure of subplots is drawn");
    r = call(R"({"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"plot","arguments":{"subplots":[{"y":[1]},{"type":"line"}]}}})");
    check(r.find("\"isError\":true") != std::string::npos && r.find("subplot 2") != std::string::npos,
          "mcp: a bad subplot is named");

    // A plot call is drawn as its chart, in either agent's transcript, and a
    // successful result is not shown under it.
    Arena ar;
    std::vector<Event> ev;
    claude_adapter().parse(
        R"({"type":"assistant","message":{"role":"assistant","content":[{"type":"tool_use","id":"p1","name":"mcp__mico__plot","input":{"title":"Sq","y":[0,1,4,9]}}]}})",
        ar, ev);
    check(ev.size() == 1 && ev[0].kind == EventKind::Chart && ar.view(ev[0].text).starts_with("```chart\n{"),
          "mcp: claude's plot call is a chart");
    ev.clear();
    codex_adapter().parse(
        R"({"timestamp":"x","type":"event_msg","payload":{"type":"item_completed","item":{"type":"McpToolCall","id":"m1","server":"mico","tool":"plot","arguments":{"y":[1,2]},"status":"completed"}}})",
        ar, ev);
    check(ev.size() == 1 && ev[0].kind == EventKind::Chart, "mcp: codex's plot call is a chart");
    ev.clear();
    codex_adapter().parse(
        R"({"timestamp":"x","type":"event_msg","payload":{"type":"item_completed","item":{"type":"McpToolCall","id":"m2","server":"other","tool":"plot","arguments":{"y":[1,2]}}}})",
        ar, ev);
    check(ev.empty(), "mcp: another server's plot is not ours");

    const std::string path = "/tmp/mico_mcp_chart.jsonl";
    put_file(path,
             R"({"type":"assistant","message":{"role":"assistant","content":[{"type":"tool_use","id":"p1","name":"mcp__mico__plot","input":{"title":"Sq","y":[0,1,4,9],"height":5}}]}})" "\n"
             R"({"type":"user","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"p1","content":"Drawn for the user: DRAWNNOTE"}]}})" "\n"
             R"({"type":"assistant","message":{"role":"assistant","content":[{"type":"tool_use","id":"p2","name":"mcp__mico__plot","input":{"type":"line"}}]}})" "\n"
             R"({"type":"user","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"p2","is_error":true,"content":"Not drawn: BADNOTE"}]}})" "\n");
    ChatRenderer c;
    c.open(path, &claude_adapter());
    Surface sf;
    sf.resize(60, 20);
    Theme theme;
    Filters filters;
    filters.density = Density::Full;  // results shown: only a drawn chart's is still hidden
    Painter p(sf, Rect{0, 0, 60, 20});
    c.render(p, theme, filters);
    std::string shown;
    int braille = 0;  // the line's marks
    for (int y = 0; y < 20; y++)
      for (int x = 0; x < 60; x++) {
        const char32_t cp = sf.at(x, y).cp;
        braille += cp > 0x2800 && cp <= 0x28FF;
        if (sf.at(x, y).width) text::encode(cp ? cp : U' ', shown);
      }
    check(braille > 3 && shown.find("Sq") != std::string::npos, "mcp: the chat draws the chart");
    check(shown.find("DRAWNNOTE") == std::string::npos && shown.find("BADNOTE") != std::string::npos,
          "mcp: a drawn chart's result is hidden, a failed one's is shown");
    unlink(path.c_str());
  }

  // Tool calls sorted by what they were for, from commands as agents write them.
  {
    const auto kind = [](std::string_view tool, std::string_view cmd, std::string* g = nullptr,
                         size_t* from = nullptr) { return classify_tool(tool, cmd, g, from); };
    std::string g;
    size_t from = 0;
    check(kind("Bash", "cd /x && cmake --build build -j8 2>&1 | tail -3", &g) == ToolKind::Build &&
              g == "cmake --build", "tools: cd && cmake --build is a build");
    check(kind("Bash", "S=/tmp/a/scratchpad; cp x $S/y.bak; sed -i 's/a/b/' f; cmake --build build; ./build/mico --selftest",
               &g, &from) == ToolKind::Build && g == "cmake --build",
          "tools: the heaviest step names a chained command line");
    check(std::string_view("S=/tmp/a/scratchpad; cp x $S/y.bak; sed -i 's/a/b/' f; cmake --build build; ./build/mico --selftest")
              .substr(from).starts_with("cmake --build"), "tools: shown from the step that decided");
    check(kind("Bash", "./build/mico --selftest 2>&1 | grep -E 'FAIL|all checks'", &g) == ToolKind::Test &&
              g == "mico --selftest", "tools: a --selftest run is a test");
    check(kind("Bash", "python3 - <<'PY'\nimport os\nos.system('make')\nPY", &g) == ToolKind::Run &&
              g == "python3 (inline)", "tools: a heredoc's text is not more commands");
    check(kind("Bash", "objs=$(find build -name '*.o' | grep -v main); c++ -O2 x.cpp $objs -o x", &g) == ToolKind::Build &&
              g == "c++", "tools: $(...) holds together");
    check(kind("Bash", "pkill -f x; sleep 1; ./run.sh --fast", &g) == ToolKind::Run && g == "run.sh --fast",
          "tools: housekeeping never names a command line");
    check(kind("Bash", "git status --short", &g) == ToolKind::Git && g == "git status", "tools: git by subcommand");
    check(kind("Bash", "pytest -q tests/", &g) == ToolKind::Test, "tools: pytest");
    check(kind("Bash", "pip install -r requirements.txt", &g) == ToolKind::Install && g == "pip install",
          "tools: installs");
    check(kind("Bash", "grep -rn foo src | head", &g) == ToolKind::Read, "tools: reading and searching");
    check(kind("Bash", "sed -i 's/a/b/' f", &g) == ToolKind::Edit && kind("Bash", "sed -n 1,5p f") == ToolKind::Read,
          "tools: sed edits only with -i");
    check(kind("Bash", "timeout 600 valgrind --error-exitcode=99 ./t", &g) == ToolKind::Run &&
              g == "valgrind --error-exitcode=99", "tools: wrappers are looked through");
    check(kind("Bash", "for f in a b; do cat $f; done", &g) == ToolKind::Read, "tools: a loop is named by its body");
    check(kind("Read", "/x/y.cpp") == ToolKind::Read && kind("Edit", "/x") == ToolKind::Edit &&
              kind("AskUserQuestion", "") == ToolKind::Wait && kind("Task", "x") == ToolKind::Agent,
          "tools: tools by name");

    // Each agent's transcript: durations, failures, and where each call is.
    const std::string cl = "/tmp/mico_activity_claude.jsonl";
    const std::string cl_body =
        R"({"type":"assistant","timestamp":"2026-09-20T10:00:00.000Z","message":{"role":"assistant","content":[{"type":"tool_use","id":"t1","name":"Bash","input":{"command":"make -j8"}}]}})" "\n"
        R"({"type":"assistant","timestamp":"2026-09-20T10:00:00.500Z","message":{"role":"assistant","content":[{"type":"tool_use","id":"t2","name":"Read","input":{"file_path":"/x/a.png"}}]}})" "\n"
        R"({"type":"user","timestamp":"2026-09-20T10:00:12.000Z","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"t1","is_error":true,"content":"boom"}]}})" "\n"
        R"({"type":"user","timestamp":"2026-09-20T10:01:00.500Z","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"t2","content":"ok"}]}})" "\n";
    put_file(cl, cl_body);
    ChatActivity a = ActivityIndex::read_file(cl, "claude");
    check(a.runs.size() == 2 && a.runs[0].dur_ms == 12000 && a.runs[0].failed && a.runs[0].kind == ToolKind::Build &&
              a.runs[0].offset == 0, "tools: claude calls timed call to result, errors kept");
    check(a.runs.size() == 2 && a.runs[1].waited && a.runs[1].dur_ms == 60000,
          "tools: a file tool that sat for a minute was waiting on a permission prompt");

    const std::string cx = "/tmp/mico_activity_codex.jsonl";
    put_file(cx,
             R"({"timestamp":"2026-09-20T10:00:05.000Z","type":"event_msg","payload":{"type":"item_completed","item":{"type":"CommandExecution","command":["/bin/bash","-lc","cargo test"],"exit_code":101,"duration":{"secs":4,"nanos":500000000},"status":"completed"}}})" "\n"
             R"({"timestamp":"2026-09-20T10:01:00.000Z","type":"response_item","payload":{"type":"function_call","name":"shell","arguments":"{\"command\":[\"bash\",\"-lc\",\"npm run build\"]}","call_id":"c1"}})" "\n"
             R"({"timestamp":"2026-09-20T10:03:00.000Z","type":"response_item","payload":{"type":"function_call_output","call_id":"c1","output":"Exit code: 0\nWall time: 2.5 seconds\nOutput:\nok"}})" "\n");
    a = ActivityIndex::read_file(cx, "codex");
    check(a.runs.size() == 2 && a.runs[0].dur_ms == 4500 && a.runs[0].failed && a.runs[0].kind == ToolKind::Test,
          "tools: codex commands carry their own duration and exit code");
    check(a.runs.size() == 2 && a.runs[1].dur_ms == 2500 && !a.runs[1].failed && a.runs[1].kind == ToolKind::Build &&
              a.runs[1].group == "npm run build", "tools: older codex: Wall time is the duration");
    put_file(cx,
             R"({"timestamp":"2026-09-20T10:00:05.000Z","type":"event_msg","payload":{"type":"item_completed","item":{"type":"CommandExecution","command":["/bin/bash","-lc","rg -n foo src"],"parsed_cmd":[{"type":"search"}],"exit_code":0,"duration":{"secs":0,"nanos":2000000},"status":"completed"}}})" "\n");
    a = ActivityIndex::read_file(cx, "codex");
    check(a.runs.size() == 1 && a.runs[0].kind == ToolKind::Read && a.runs[0].group == "rg",
          "tools: a command codex says only searched is a read, and keeps its group");

    const std::string pi = "/tmp/mico_activity_pi.jsonl";
    put_file(pi,
             R"({"type":"message","id":"1","timestamp":"2026-09-20T10:00:00.000Z","message":{"role":"assistant","content":[{"type":"toolCall","id":"k1","name":"bash","arguments":{"command":"go test ./..."}}]}})" "\n"
             R"({"type":"message","id":"2","timestamp":"2026-09-20T10:00:03.000Z","message":{"role":"toolResult","toolCallId":"k1","isError":false,"content":[]}})" "\n");
    a = ActivityIndex::read_file(pi, "pi");
    check(a.runs.size() == 1 && a.runs[0].dur_ms == 3000 && a.runs[0].kind == ToolKind::Test, "tools: pi calls");

    // A call whose result lands in a later pass is still matched.
    std::vector<Project> projects(1);
    projects[0].name = "p";
    projects[0].sessions.push_back(SessionRef{"c", cl, "Chat", "claude", "/p", 1, 0});
    put_file(cl, cl_body.substr(0, cl_body.find("{\"type\":\"user\"")));
    projects[0].sessions[0].bytes = cl_body.find("{\"type\":\"user\"");
    Store store;
    ActivityIndex idx;
    idx.start(projects, store);
    while (!idx.step(50)) {}
    const size_t first_pass = idx.chats().empty() ? 99 : idx.chats()[0]->runs.size();
    put_file(cl, cl_body);
    projects[0].sessions[0].bytes = cl_body.size();
    projects[0].sessions[0].mtime = 2;
    idx.start(projects, store);
    while (!idx.step(50)) {}
    check(first_pass == 0 && idx.chats().size() == 1 && idx.chats()[0]->runs.size() == 2 &&
              idx.chats()[0]->runs[0].dur_ms == 12000,
          "tools: a call's result read in a later pass completes it");
    unlink(cl.c_str());
    unlink(cx.c_str());
    unlink(pi.c_str());
  }

  {
    // File changes, from each agent's own record of them.
    const auto one = [](std::string_view agent, std::string_view raw, std::string_view cwd = "/w") {
      std::vector<LineChanges> v = read_changes(agent, raw, cwd, true);
      return v.empty() ? LineChanges{} : v[0];
    };
    // claude: the result's structuredPatch, numbered; a created file's content.
    LineChanges lc = one("claude",
        R"({"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"t9"}]},"toolUseResult":{"filePath":"/w/a.cpp","structuredPatch":[{"oldStart":7,"oldLines":2,"newStart":7,"newLines":3,"lines":[" keep","-old","+new\tx","+more"]}]}})");
    check(lc.result_of == "t9" && lc.changes.size() == 1 && lc.changes[0].file == "/w/a.cpp" &&
              lc.changes[0].added == 2 && lc.changes[0].removed == 1 && lc.changes[0].op == EditOp::Edit,
          "edits: claude's structuredPatch counted");
    check(lc.changes.size() == 1 && lc.changes[0].lines.size() == 5 && lc.changes[0].lines[0].kind == '@' &&
              lc.changes[0].lines[1].new_no == 7 && lc.changes[0].lines[2].old_no == 8 &&
              lc.changes[0].lines[3].new_no == 8 && lc.changes[0].lines[3].text == "new    x",
          "edits: lines numbered from the hunk, tabs made spaces");
    lc = one("claude", R"({"toolUseResult":{"type":"create","filePath":"/w/n.md","content":"a\nb\n","structuredPatch":[]}})");
    check(lc.changes.size() == 1 && lc.changes[0].op == EditOp::Create && lc.changes[0].added == 2,
          "edits: a file claude created is every line added");
    check(one("claude", R"({"toolUseResult":{"type":"text","file":{"filePath":"/w/r.cpp","content":"x"}}})").changes.empty(),
          "edits: a Read is no change");

    // codex: FileChange items; an older rollout's apply_patch and its result.
    lc = one("codex",
        R"({"timestamp":"2026-09-20T10:00:00.000Z","type":"event_msg","payload":{"type":"item_completed","item":{"type":"FileChange","id":"c1","status":"completed","changes":{"/w/b.py":{"type":"update","unified_diff":"@@ -3,2 +3,2 @@\n ctx\n-x = 1\n+x = 2\n"},"rel/c.py":{"type":"add","content":"print(1)\n"},"/w/d.py":{"type":"delete","content":"a\nb\n"}}}}})");
    check(lc.result_of == "c1" && lc.changes.size() == 3 && lc.changes[0].added == 1 && lc.changes[0].removed == 1 &&
              lc.changes[1].file == "/w/rel/c.py" && lc.changes[1].op == EditOp::Create &&
              lc.changes[2].op == EditOp::Delete && lc.changes[2].removed == 2,
          "edits: codex FileChange updates, adds and deletes");
    lc = one("codex",
        R"({"type":"response_item","payload":{"type":"custom_tool_call","call_id":"p1","name":"apply_patch","input":"*** Begin Patch\n*** Update File: src/e.rs\n*** Move to: src/f.rs\n@@ fn main\n-a\n+b\n+c\n*** Add File: g.txt\n+hi\n*** End Patch"}})",
        "/w/./proj/");
    check(lc.call_id == "p1" && lc.changes.size() == 2 && lc.changes[0].file == "/w/proj/src/e.rs" &&
              lc.changes[0].moved_to == "/w/proj/src/f.rs" && lc.changes[0].added == 2 && lc.changes[0].removed == 1 &&
              lc.changes[1].op == EditOp::Create,
          "edits: an apply_patch envelope, file by file, paths from the chat's folder");

    // pi: the result's patch; omp: its numbered diff and relative path.
    lc = one("pi",
        R"({"type":"message","message":{"role":"toolResult","toolCallId":"k1","toolName":"edit","isError":false,"details":{"diff":"x","patch":"--- /w/h.go\n+++ /w/h.go\n@@ -1,1 +1,1 @@\n-a\n+b\n"}}})");
    check(lc.result_of == "k1" && lc.changes.size() == 1 && lc.changes[0].file == "/w/h.go" &&
              lc.changes[0].added == 1 && lc.changes[0].removed == 1,
          "edits: pi's patch, the file from its header");
    lc = one("omp",
        R"({"type":"message","message":{"role":"toolResult","toolCallId":"k2","toolName":"edit","details":{"path":"src/i.c","op":"update","diff":" 20|keep\n\n 43|};\n-44|gone\n+44|here\n+45|also"}}})");
    check(lc.changes.size() == 1 && lc.changes[0].file == "/w/src/i.c" && lc.changes[0].added == 2 &&
              lc.changes[0].removed == 1 && lc.changes[0].lines.size() == 6 && lc.changes[0].lines[1].kind == '@' &&
              lc.changes[0].lines[3].old_no == 44 && lc.changes[0].lines[5].new_no == 45,
          "edits: omp's numbered diff, a gap where lines were left out");

    // Through the index: a call's changes count only once its result says
    // they went through, and codex's second record of a patch replaces it.
    const std::string f = "/tmp/mico_edits_codex.jsonl";
    put_file(f,
        R"({"timestamp":"2026-09-20T10:00:00.000Z","type":"response_item","payload":{"type":"custom_tool_call","call_id":"p1","name":"apply_patch","input":"*** Begin Patch\n*** Update File: a.txt\n@@\n-a\n+b\n*** End Patch"}})" "\n"
        R"({"timestamp":"2026-09-20T10:00:01.000Z","type":"response_item","payload":{"type":"custom_tool_call_output","call_id":"p1","output":"apply_patch verification failed: no match"}})" "\n"
        R"({"timestamp":"2026-09-20T10:00:02.000Z","type":"response_item","payload":{"type":"custom_tool_call","call_id":"p2","name":"apply_patch","input":"*** Begin Patch\n*** Update File: a.txt\n@@\n-x\n+y\n*** End Patch"}})" "\n"
        R"({"timestamp":"2026-09-20T10:00:03.000Z","type":"event_msg","payload":{"type":"item_completed","item":{"type":"FileChange","id":"p2","status":"completed","changes":{"/w/a.txt":{"type":"update","unified_diff":"@@ -9,1 +9,1 @@\n-x\n+y\n"}}}}})" "\n"
        R"({"timestamp":"2026-09-20T10:00:04.000Z","type":"response_item","payload":{"type":"custom_tool_call_output","call_id":"p2","output":"Success."}})" "\n");
    ChatActivity a = ActivityIndex::read_file(f, "codex", "/w");
    check(a.edits.size() == 1 && a.edits[0].file == "/w/a.txt" && a.edits[0].call_offset == 0 + a.edits[0].call_offset &&
              a.edits[0].offset > a.edits[0].call_offset,
          "edits: a failed patch is dropped, and a patch recorded twice counts once");
    FileChange fc;
    check(!a.edits.empty() && load_change(f, "codex", "/w", a.edits[0].offset, a.edits[0].file, fc) &&
              fc.lines.size() == 3 && fc.lines[0].old_no == 9 && fc.lines[2].text == "y",
          "edits: a change's lines read again from its line");
    unlink(f.c_str());

    const std::string g = "/tmp/mico_edits_pi.jsonl";
    put_file(g,
        R"({"type":"message","timestamp":"2026-09-20T10:00:00.000Z","message":{"role":"assistant","content":[{"type":"toolCall","id":"w1","name":"write","arguments":{"path":"out.txt","content":"1\n2\n3\n"}},{"type":"toolCall","id":"w2","name":"write","arguments":{"path":"bad.txt","content":"x"}}]}})" "\n"
        R"({"type":"message","timestamp":"2026-09-20T10:00:01.000Z","message":{"role":"toolResult","toolCallId":"w1","toolName":"write","isError":false,"content":[]}})" "\n"
        R"({"type":"message","timestamp":"2026-09-20T10:00:01.000Z","message":{"role":"toolResult","toolCallId":"w2","toolName":"write","isError":true,"content":[]}})" "\n");
    a = ActivityIndex::read_file(g, "pi", "/w");
    check(a.edits.size() == 1 && a.edits[0].file == "/w/out.txt" && a.edits[0].added == 3 && a.edits[0].op == EditOp::Write,
          "edits: pi's writes, the failed one left out");
    unlink(g.c_str());
    check(resolve_path("/a/b", "../c/./d//e") == "/a/c/d/e" && resolve_path("/x", "/y/z") == "/y/z",
          "edits: paths made absolute");
  }

  // Charts: a ```chart block's JSON, drawn in braille or bars.
  {
    chart::Spec sp;
    std::string err;
    check(chart::parse(R"({"type":"line","title":"t","x":[0,1,2],"series":[{"name":"a","y":[1,2,3]},{"name":"b","y":[3,null,1]}]})", sp, &err) &&
              sp.series.size() == 2 && sp.series[1].x.size() == 3 && std::isnan(sp.series[1].y[1]),
          "chart: series with shared x, null as a gap");
    check(chart::parse(R"({"y":[5,4,3]})", sp, &err) && sp.kind == chart::Spec::Kind::Line &&
              sp.series.size() == 1, "chart: a bare y is one line");
    check(chart::parse(R"({"type":"bar","x":["a","b"],"y":[1,2]})", sp, &err) && sp.labels.size() == 2 &&
              sp.series[0].y.size() == 2, "chart: string x are bar labels");
    check(!chart::parse(R"({"type":"line"})", sp, &err) && err.find("data") != std::string::npos,
          "chart: no data is an error that says so");
    check(!chart::parse("not json", sp, &err), "chart: not JSON is not a chart");
    check(chart::parse(R"({"file":"m.csv","x":"step","y":["loss","val"]})", sp, &err) &&
              sp.file == "m.csv" && sp.xcol == "step" && sp.ycols.size() == 2,
          "chart: a file chart names its columns");

    chart::Canvas cv;
    chart::parse(R"({"title":"sq","x":[0,1,2,3],"y":[0,1,4,9],"height":6})", sp, &err);
    chart::draw(sp, 40, cv);
    const auto count_in = [&](std::u32string_view set) {
      int n = 0;
      for (char32_t c : cv.cp) n += set.find(c) != std::u32string_view::npos;
      return n;
    };
    int braille = 0;
    for (char32_t c : cv.cp) braille += c > 0x2800 && c <= 0x28FF;
    check(cv.w == 40 && cv.h == 1 + 6 + 2 && braille > 5, "chart: a line is drawn in braille by default, 40 wide");
    // "line": box drawing, which the terminal draws itself, whole in any font.
    // One value per column and nothing skipped: every plot column has a mark.
    chart::parse(R"({"title":"sq","x":[0,1,2,3],"y":[0,1,4,9],"height":6,"marker":"line"})", sp, &err);
    chart::draw(sp, 40, cv);
    check(count_in(U"\u2500\u256D\u256E\u2570\u256F\u2502") > 20 && count_in(U"\u2801\u2802\u2804\u2808\u2840\u2880") == 0,
          "chart: \"marker\": \"line\" draws with box-drawing characters");
    {
      int lw = 0;
      while (lw < cv.w && cv.at(lw, 1) != U'\u2524') lw++;  // ┤ on the top row
      bool whole = lw < cv.w;
      for (int x = lw + 1; x < cv.w && whole; x++) {
        bool any = false;
        for (int y = 1; y < 1 + 6; y++) any |= cv.at(x, y) != U' ';
        whole = any;
      }
      check(whole, "chart: a box-drawn line is whole, a mark in every column");
    }
    chart::parse(R"({"type":"scatter","x":[0,1,2],"y":[0,1,4],"marker":"block"})", sp, &err);
    chart::draw(sp, 40, cv);
    check(count_in(U"\u2598\u259D\u2596\u2597\u2580\u2584\u258C\u2590\u259A\u259E\u2588") >= 3,
          "chart: \"marker\": \"block\" draws in block quadrants");
    chart::parse(R"({"title":"sq","x":[0,1,2,3],"y":[0,1,4,9],"height":6})", sp, &err);
    chart::draw(sp, 40, cv);
    std::string rows;
    for (int y = 0; y < cv.h; y++) {
      for (int x = 0; x < cv.w; x++) text::encode(cv.at(x, y), rows);
      rows += '\n';
    }
    check(rows.find("9\xE2\x94\xA4") != std::string::npos && rows.find("0\xE2\x94\xA4") != std::string::npos,
          "chart: the y axis is labelled at its top and bottom");
    chart::parse(R"({"type":"bar","labels":["a","bb"],"y":[10,5]})", sp, &err);
    chart::draw(sp, 30, cv);
    rows.clear();
    for (int y = 0; y < cv.h; y++) {
      for (int x = 0; x < cv.w; x++) text::encode(cv.at(x, y), rows);
      rows += '\n';
    }
    check(cv.h == 2 && rows.find(" a\xE2\x94\x82\xE2\x96\x88") != std::string::npos &&
              rows.find("10") != std::string::npos,
          "chart: bars are labelled rows with their values");
    check(chart::format_number(1234567) == "1.23M" && chart::format_number(0.00002) == "2.0e-05" &&
              chart::format_number(12.5) == "12.5", "chart: compact axis numbers");

    // Data from files, re-read when they change.
    const std::string csv = "/tmp/mico_chart_test.csv", jl = "/tmp/mico_chart_test.jsonl";
    put_file(csv, "step,loss\n0,3\n1,2\n2,1.5\n");
    put_file(jl, "{\"step\":0,\"acc\":0.1}\n{\"step\":1,\"acc\":0.5}\n");
    chart::parse(R"({"file":"/tmp/mico_chart_test.csv","x":"step","y":"loss"})", sp, &err);
    int64_t mt = 0;
    check(chart::load_file(sp, "", &err, &mt, nullptr) && sp.series.size() == 1 &&
              sp.series[0].y.size() == 3 && sp.series[0].y[2] == 1.5 && sp.series[0].x[1] == 1 && mt > 0,
          "chart: CSV columns by header name");
    chart::parse(R"({"file":"mico_chart_test.jsonl","y":["acc"]})", sp, &err);
    check(chart::load_file(sp, "/tmp", &err, nullptr, nullptr) && sp.series[0].y.size() == 2 &&
              sp.series[0].y[1] == 0.5, "chart: JSONL fields, relative to the chat's folder");
    chart::parse(R"({"file":"/tmp/mico_chart_test.csv","y":"nope"})", sp, &err);
    check(!chart::load_file(sp, "", &err, nullptr, nullptr) && err.find("nope") != std::string::npos,
          "chart: a missing column is named");
    put_file(csv, "step,loss\n0,3\n1,2\n2,1.5\n3,0.25\n");
    chart::parse(R"({"file":"/tmp/mico_chart_test.csv","x":"step","y":"loss"})", sp, &err);
    check(chart::load_file(sp, "", &err, nullptr, nullptr) && sp.series[0].y.size() == 4,
          "chart: a changed file is read again, not served from the cache");

    // In a message: coloured rows; a broken spec shows as code with the
    // reason; a block still streaming (no closing fence) stays code.
    Arena sc;
    std::vector<md::Seg> segs;
    std::vector<md::Line> lines;
    std::vector<text::Span> spans;
    std::vector<md::Seg> inl;
    md::Out o{size_t(-1), &sc, &segs, &lines, &spans, &inl};
    const std::string msg = "look:\n```chart\n{\"y\":[1,3,2],\"height\":4}\n```\ndone\n";
    md::render(msg, 0, false, 50, o);
    bool series_ink = false, axis_ink = false;
    for (const auto& sg : segs) {
      series_ink |= sg.ink == md::Ink::Series1;
      axis_ink |= sg.ink == md::Ink::ChartAxis;
    }
    check(lines.size() == 1 + 4 + 2 + 1 && series_ink && axis_ink,
          "chart: a ```chart block renders as chart rows, series in colour");
    segs.clear(); lines.clear();
    md::render("```chart\n{\"type\":\"line\"}\n```\n", 0, false, 50, o);
    bool reason = false, code = false;
    for (const auto& sg : segs) {
      reason |= sg.ink == md::Ink::ChartAxis;
      code |= sg.ink >= md::Ink::CodeText && sg.ink <= md::Ink::CodeMark;
    }
    check(reason && code, "chart: a broken spec shows as code, with the reason");
    segs.clear(); lines.clear();
    md::render("```chart\n{\"y\":[1,2", 0, false, 50, o);
    bool all_code = !segs.empty();
    for (const auto& sg : segs) all_code &= sg.ink >= md::Ink::CodeText && sg.ink <= md::Ink::CodeMark;
    check(all_code, "chart: a block still streaming in stays code");

    // A chat redraws a file chart when its file changes.
    const std::string tpath = "/tmp/mico_chart_chat.jsonl";
    put_file(tpath, R"({"type":"message","id":"a","message":{"role":"assistant","content":[{"type":"text","text":"```chart\n{\"file\":\"mico_chart_test.csv\",\"x\":\"step\",\"y\":\"loss\",\"height\":4}\n```"}]}})" "\n");
    ChatRenderer c;
    c.set_base_dir("/tmp");
    c.open(tpath, &pi_adapter());
    Surface sf;
    sf.resize(60, 12);
    Theme theme;
    Filters filters;
    const auto frame = [&] {
      Painter p(sf, Rect{0, 0, 60, 12});
      c.render(p, theme, filters);
      std::string out;
      for (int y = 0; y < 12; y++)
        for (int x = 0; x < 60; x++)
          if (sf.at(x, y).width) text::encode(sf.at(x, y).cp ? sf.at(x, y).cp : U' ', out);
      return out;
    };
    const std::string before = frame();
    usleep(20000);
    put_file(csv, "step,loss\n0,3\n1,2\n2,1.5\n3,0.25\n4,77\n");
    usleep(1100000);  // the chat looks at its chart files once a second
    const std::string after = frame();
    check(before.find("3\xE2\x94\xA4") != std::string::npos && after.find("77\xE2\x94\xA4") != std::string::npos,
          "chart: a file chart in a chat redraws when the file changes");
    unlink(csv.c_str());
    unlink(jl.c_str());
    unlink(tpath.c_str());
  }

  // Case-folded search: ASCII letters in either case, from an offset, and a
  // first byte common in one case and absent in the other stays linear.
  {
    check(text::find_folded("Hello World", text::fold("WORLD")) == 6, "find_folded ignores case");
    check(text::find_folded("abcabc", "bc", 2) == 4, "find_folded honours the start offset");
    check(text::find_folded("abc", "abcd") == std::string_view::npos, "find_folded: longer needle");
    check(text::find_folded("caf\xC3\xA9 ok", "\xC3\xA9 o") == 3, "find_folded: UTF-8 matched exactly");
    std::string big(8u << 20, 'a');
    big += "aB";
    const auto t0 = std::chrono::steady_clock::now();
    const size_t at = text::find_folded(big, "ab");
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    check(at == big.size() - 2 && ms < 500, "find_folded stays linear on a one-sided first byte");
  }
  // text::find and find_folded test sixteen positions at a time; checked here
  // against the obvious loops on strings made to hold many near misses.
  {
    uint32_t seed = 12345;
    const auto rnd = [&](uint32_t m) { seed = seed * 1103515245u + 12345u; return (seed >> 8) % m; };
    const char alphabet[] = {'a', 'b', 'A', 'B', '"', '_', '\xC3', '\xA9', ' ', '\n'};
    bool ok_find = true, ok_fold = true;
    for (int round = 0; round < 20000 && ok_find && ok_fold; round++) {
      std::string hay(rnd(80), ' '), needle(1 + rnd(6), ' ');
      for (char& c : hay) c = alphabet[rnd(sizeof alphabet)];
      for (char& c : needle) c = alphabet[rnd(sizeof alphabet)];
      const size_t from = rnd(uint32_t(hay.size() + 2));
      size_t want = std::string_view(hay).find(needle, from);
      ok_find = text::find(hay, needle, from) == want;
      const std::string lowered = text::fold(needle);
      want = std::string_view::npos;
      const std::string hay_lower = text::fold(hay);
      if (from <= hay.size()) want = std::string_view(hay_lower).find(lowered, from);
      ok_fold = text::find_folded(hay, lowered, from) == want;
    }
    check(ok_find, "text::find agrees with string_view::find");
    check(ok_fold, "text::find_folded agrees with searching the folded text");
  }

  // Find in one chat covers the whole file, counts only text the chat shows,
  // walks older then wraps, and leaves the match on screen.
  // Alt/Ctrl+↑ walks back one user message at a time.
  {
    const std::string path = "/tmp/mico_find_test.jsonl";
    std::string body;
    const auto user = [&](const std::string& t) {
      body += R"({"type":"message","id":"u","message":{"role":"user","content":[{"type":"text","text":")" + t + "\"}]}}\n";
    };
    const auto said = [&](const std::string& t) {
      body += R"({"type":"message","id":"a","message":{"role":"assistant","content":[{"type":"text","text":")" + t + "\"}]}}\n";
    };
    for (int i = 0; i < 150; i++) {
      user("question " + std::to_string(i));
      said(i == 10 ? "the Needle is here" : i == 120 ? "another NEEDLE" : "answer " + std::to_string(i));
    }
    // Only in an id: raw bytes match, the chat shows nothing of it.
    body += R"({"type":"message","id":"needle-id","message":{"role":"user","content":[{"type":"text","text":"plain"}]}})" "\n";
    for (int i = 150; i < 155; i++) { user("question " + std::to_string(i)); said("answer " + std::to_string(i)); }
    if (FILE* f = fopen(path.c_str(), "wb")) {
      fwrite(body.data(), 1, body.size(), f);
      fclose(f);
    }
    Theme theme;
    Filters filters;
    Surface sf;
    sf.resize(60, 14);
    const auto rows = [&] {
      std::vector<std::string> out;
      for (int y = 0; y < 14; y++) {
        std::string r;
        for (int x = 0; x < 60; x++) {
          const Cell& cell = sf.at(x, y);
          if (cell.width) text::encode(cell.cp ? cell.cp : U' ', r);
        }
        out.push_back(r);
      }
      return out;
    };
    const auto screen_has = [&](std::string_view t) {
      for (const auto& r : rows())
        if (r.find(t) != std::string::npos) return true;
      return false;
    };
    ChatRenderer c;
    check(c.open(path, &pi_adapter()), "the find transcript opens");
    const auto frame = [&](ChatRenderer& r) {
      Painter p(sf, Rect{0, 0, 60, 14});
      r.render(p, theme, filters);
    };
    frame(c);
    c.set_find_query("needle");
    c.find_next(-1);
    frame(c);
    check(c.find_count() == 2, "find counts shown text only, over the whole file");
    check(c.find_index() == 2 && screen_has("another NEEDLE"), "the first step goes to the nearest older match");
    c.find_next(-1);
    frame(c);
    check(c.find_index() == 1 && screen_has("the Needle is here"), "the next step goes further back");
    c.find_next(-1);
    frame(c);
    check(c.find_index() == 2, "stepping past the oldest wraps around");
    c.find_next(1);
    frame(c);
    check(c.find_index() == 1, "a newer step from the newest wraps to the oldest");

    // The user-message jump.
    ChatRenderer j;
    j.open(path, &pi_adapter());
    frame(j);
    const auto lowest_question = [&] {
      int best = -1;
      for (const auto& r : rows()) {
        const size_t at = r.find("question ");
        if (at == std::string::npos) continue;
        const int q = std::atoi(r.c_str() + at + 9);
        if (best < 0 || q < best) best = q;
      }
      return best;
    };
    const auto question_at_row = [&](int y) {
      const std::string r = rows()[size_t(y)];
      const size_t at = r.find("question ");
      return at == std::string::npos ? -1 : std::atoi(r.c_str() + at + 9);
    };
    const int q0 = lowest_question();
    j.jump_user(-1);
    frame(j);
    check(q0 > 0 && question_at_row(1) == q0 - 1, "Alt+\xE2\x86\x91 goes to the previous user message");
    j.jump_user(-1);
    frame(j);
    check(question_at_row(1) == q0 - 2, "and again to the one before");
    j.jump_user(1);
    frame(j);
    check(question_at_row(1) == q0 - 1, "Alt+\xE2\x86\x93 goes to the next");
    j.on_key(KeyEvent{Key::Up, 0, false, true});
    frame(j);
    check(question_at_row(1) == q0 - 2, "Alt+\xE2\x86\x91 is the key for it");
    unlink(path.c_str());
  }

  // Search across chats: every file, newest first, hits confirmed on text a
  // chat shows, a snippet with the match marked, and where the line starts.
  {
    const std::string a = "/tmp/mico_search_a.jsonl", b = "/tmp/mico_search_b.jsonl";
    const std::string la =
        R"({"type":"message","id":"needle-in-id","message":{"role":"user","content":[{"type":"text","text":"nothing here"}]}})" "\n"
        R"({"type":"message","id":"x","message":{"role":"user","content":[{"type":"text","text":"where is the Needle hiding"}]}})" "\n"
        R"({"type":"message","id":"y","message":{"role":"assistant","content":[{"type":"text","text":"a NEEDLE again"}]}})" "\n";
    const std::string lb =
        R"({"type":"message","id":"z","message":{"role":"assistant","content":[{"type":"text","text":"one needle"}]}})" "\n";
    for (const auto& [path, text] : {std::pair{a, la}, std::pair{b, lb}})
      if (FILE* f = fopen(path.c_str(), "wb")) {
        fwrite(text.data(), 1, text.size(), f);
        fclose(f);
      }
    std::vector<Project> projects(1);
    projects[0].name = "proj";
    projects[0].sessions.push_back(SessionRef{"sa", a, "Chat A", "pi", "/x", 100, la.size()});
    projects[0].sessions.push_back(SessionRef{"sb", b, "Chat B", "pi", "/x", 200, lb.size()});
    Store store;
    ChatSearch search;
    search.start(projects, store, "needle");
    for (int i = 0; i < 100 && !search.step(20); i++) {}
    const auto& h = search.hits();
    check(search.complete() && search.total_hits() == 3 && search.chats_with_hits() == 2,
          "search finds shown text in every chat, not ids");
    check(h.size() == 3 && h[0].title == "Chat B" && h[0].first_in_chat && h[1].title == "Chat A" &&
              h[1].first_in_chat && h[1].chat_hits == 2 && !h[2].first_in_chat,
          "hits are grouped by chat, newest chat first, with a count");
    if (h.size() == 3) {
      check(text::fold(h[1].snippet.substr(h[1].match_at, h[1].match_len)) == "needle",
            "the snippet marks the match");
      check(la.compare(h[1].offset, 12, R"({"type":"mes)") == 0 &&
                la.find("hiding", h[1].offset) < la.find('\n', h[1].offset),
            "the hit points at the start of its line");
    }
    unlink(a.c_str());
    unlink(b.c_str());
  }

  // A multi-select card toggles with Space and commits from its Submit row.
  {
    const std::string path = "/tmp/mico_question_multi.jsonl";
    const std::string line =
        R"({"type":"message","message":{"role":"assistant","content":[{"type":"toolCall","name":"ask","id":"call_m","arguments":{"questions":[{"header":"Extras","question":"Which extras?","multi":true,"recommended":0,"options":[{"label":"alpha","description":""},{"label":"beta","description":""},{"label":"gamma","description":""}]}]}}]}})"
        "\n";
    if (FILE* f = fopen(path.c_str(), "wb")) {
      fwrite(line.data(), 1, line.size(), f);
      fclose(f);
    }
    ChatRenderer c;
    check(c.open(path, &pi_adapter()), "a multi question transcript opens");
    c.set_questions_interactive(true);
    Theme theme;
    Filters filters;
    Surface sf;
    sf.resize(60, 12);
    auto frame = [&] {
      Painter p(sf, Rect{0, 0, 60, 12});
      c.render(p, theme, filters);
    };
    auto screen = [&] {
      std::string s;
      for (int y = 0; y < 12; y++) {
        for (int x = 0; x < 60; x++) {
          const Cell& cell = sf.at(x, y);
          if (cell.width == 0) continue;
          text::encode(cell.cp ? cell.cp : U' ', s);
        }
        s.push_back('\n');
      }
      return s;
    };
    frame();
    check(screen().find("Submit") != std::string::npos, "a multi-select shows a Submit row");
    check(c.question_key(KeyEvent{Key::Char, ' ', false, false, false}),
          "space toggles an option");
    frame();
    check(screen().find("\xE2\x98\x91") != std::string::npos,
          "the toggled option shows a checked box");
    c.question_key(KeyEvent{Key::Down});
    check(c.question_key(KeyEvent{Key::Enter}), "enter toggles a multi-select option like space");
    ChatRenderer::Answer early;
    check(!c.take_answer(early), "enter on an option does not skip ahead and commit");
    check(c.question_key(KeyEvent{Key::Tab}), "tab moves on to the Submit step");
    check(c.question_key(KeyEvent{Key::Enter}), "enter on the Submit step commits");
    ChatRenderer::Answer a;
    const std::vector<uint8_t> want{1, 1, 0};
    check(c.take_answer(a) && a.chosen.size() == 1 && a.chosen[0] == want,
          "the toggled set is the committed answer");
  }

  // A long option wraps instead of running off the card: its later rows carry
  // no marker of their own, and clicking one still chooses the option. A note
  // typed for a question is drawn on the card and goes out with the answer.
  {
    const std::string path = "/tmp/mico_question_long.jsonl";
    const std::string line =
        R"({"type":"message","message":{"role":"assistant","content":[{"type":"toolCall","name":"ask","id":"call_l","arguments":{"questions":[{"header":"Plan","question":"Which plan?","multi":false,"recommended":0,"options":[{"label":"short","description":""},{"label":"a very long option label that keeps going well past the width of this narrow card ENDLABEL","description":"and a description that is just as long and also has to wrap onto rows of its own ENDDESC"}]}]}}]}})"
        "\n";
    if (FILE* f = fopen(path.c_str(), "wb")) {
      fwrite(line.data(), 1, line.size(), f);
      fclose(f);
    }
    ChatRenderer c;
    check(c.open(path, &pi_adapter()), "a long-option transcript opens");
    c.set_questions_interactive(true);
    Theme theme;
    Filters filters;
    Surface sf;
    sf.resize(40, 30);
    auto frame = [&] {
      Painter p(sf, Rect{0, 0, 40, 30});
      c.render(p, theme, filters);
    };
    auto rows = [&] {
      std::vector<std::string> out;
      for (int y = 0; y < 30; y++) {
        std::string s;
        for (int x = 0; x < 40; x++) {
          const Cell& cell = sf.at(x, y);
          if (cell.width == 0) continue;
          text::encode(cell.cp ? cell.cp : U' ', s);
        }
        out.push_back(s);
      }
      return out;
    };
    frame();
    auto r = rows();
    int label_end = -1, desc_end = -1, markers = 0;
    for (int y = 0; y < 30; y++) {
      if (r[size_t(y)].find("ENDLABEL") != std::string::npos) label_end = y;
      if (r[size_t(y)].find("ENDDESC") != std::string::npos) desc_end = y;
      if (r[size_t(y)].find("\xE2\x97\x8B") != std::string::npos) markers++;  // ○
    }
    check(label_end >= 0 && desc_end > label_end,
          "a long option's label and description wrap onto further rows");
    check(markers == 2, "a wrapped option has one marker, on its first row");

    check(c.question_note_target() == 0, "a live card takes a note for its question");
    check(c.set_question_note(0, "because it is cheaper\n"), "a note is set");
    frame();
    r = rows();
    bool note_drawn = false;
    for (const auto& row : r) note_drawn |= row.find("because it is cheaper") != std::string::npos;
    check(note_drawn, "the note is drawn on the card");

    // Clicking the wrapped description's last row chooses that option.
    c.on_mouse(MouseEvent{MouseKind::Press, MouseButton::Left}, Point{10, desc_end});
    ChatRenderer::Answer a;
    check(c.take_answer(a) && a.chosen.size() == 1 && a.chosen[0].size() == 2 &&
              a.chosen[0][1] == 1,
          "a click on a wrapped row chooses its option");
    check(a.notes.size() == 1 && a.notes[0] == "because it is cheaper",
          "the note goes out with the answer, trimmed");
  }

  // A two-question card: answering the first advances to the second, the arrows
  // switch between them, and the Submit step commits both.
  {
    const std::string path = "/tmp/mico_question_two.jsonl";
    const std::string line =
        R"({"type":"message","message":{"role":"assistant","content":[{"type":"toolCall","name":"ask","id":"call_two","arguments":{"questions":[{"header":"First","question":"Pick one A?","multi":false,"options":[{"label":"a1","description":""},{"label":"a2","description":""}]},{"header":"Second","question":"Pick one B?","multi":false,"options":[{"label":"b1","description":""},{"label":"b2","description":""}]}]}}]}})"
        "\n";
    if (FILE* f = fopen(path.c_str(), "wb")) {
      fwrite(line.data(), 1, line.size(), f);
      fclose(f);
    }
    ChatRenderer c;
    check(c.open(path, &pi_adapter()), "a two-question transcript opens");
    c.set_questions_interactive(true);
    Theme theme;
    Filters filters;
    Surface sf;
    sf.resize(60, 18);
    auto frame = [&] {
      Painter p(sf, Rect{0, 0, 60, 18});
      c.render(p, theme, filters);
    };
    frame();
    auto screen = [&] {
      std::string s;
      for (int y = 0; y < 18; y++) {
        for (int x = 0; x < 60; x++) {
          const Cell& cell = sf.at(x, y);
          if (cell.width == 0) continue;
          text::encode(cell.cp ? cell.cp : U' ', s);
        }
        s.push_back('\n');
      }
      return s;
    };
    const std::string s0 = screen();
    check(s0.find("First") != std::string::npos && s0.find("Second") != std::string::npos,
          "both questions render");
    check(s0.find("Submit") != std::string::npos, "a two-question card shows Submit");
    check(s0.find("\xE2\x86\x90") != std::string::npos, "the navigation hint renders");
    check(c.question_active(), "a two-question card is answerable");
    ChatRenderer::Answer a;
    c.question_key(KeyEvent{Key::Right});
    c.question_key(KeyEvent{Key::Right});
    c.question_key(KeyEvent{Key::Enter});
    check(!c.take_answer(a), "Submit cannot silently choose defaults for unanswered questions");
    // Answer the first question: the card advances rather than committing.
    check(c.question_key(KeyEvent{Key::Enter}), "enter answers the first question");
    check(!c.take_answer(a), "answering the first question does not commit yet");
    check(c.question_key(KeyEvent{Key::Down}), "down moves the second question's cursor");
    check(c.question_key(KeyEvent{Key::Enter}), "enter answers the second question");
    check(c.question_key(KeyEvent{Key::Enter}), "enter on Submit commits");
    check(c.take_answer(a) && a.chosen.size() == 2 && a.chosen[0].size() == 2 &&
              a.chosen[0][0] == 1 && a.chosen[1].size() == 2 && a.chosen[1][1] == 1,
          "both answers are committed");

    // Clicking the second question while the first is focused must advance
    // from the clicked question, not from the previous keyboard focus.
    ChatRenderer clicked;
    clicked.open(path, &pi_adapter());
    clicked.set_questions_interactive(true);
    Painter painter(sf, Rect{0, 0, 60, 18});
    clicked.render(painter, theme, filters);
    const auto click_label = [&](std::string_view label) {
      for (int y = 0; y < sf.height(); ++y) {
        std::string row;
        for (int x = 0; x < sf.width(); ++x) text::encode(sf.at(x, y).cp, row);
        if (row.find(label) == std::string::npos) continue;
        clicked.on_mouse(MouseEvent{MouseKind::Press, MouseButton::Left}, Point{8, y});
        return true;
      }
      return false;
    };
    check(click_label("b2"), "the second question can be answered first by mouse");
    clicked.question_key(KeyEvent{Key::Enter});
    check(!clicked.take_answer(a), "an out-of-order answer still requires the first question");
    clicked.question_key(KeyEvent{Key::Enter});
    clicked.question_key(KeyEvent{Key::Right});
    check(click_label("[ Submit ]"), "mouse Submit is available");
    check(clicked.take_answer(a) && a.chosen.size() == 2 &&
              a.chosen[0] == std::vector<uint8_t>({1, 0}) &&
              a.chosen[1] == std::vector<uint8_t>({0, 1}),
          "mouse and keyboard answers preserve the intended choices in each question");
  }

  // A tool call with no result yet is the action in flight: the activity line
  // names it, and its row draws the spinner instead of the bullet.
  {
    const std::string path = "/tmp/mico_inflight.jsonl";
    const std::string line =
        R"({"type":"message","message":{"role":"assistant","content":[{"type":"toolCall","name":"read","id":"call_r","arguments":{"path":"src/foo.cpp"}}]}})"
        "\n";
    if (FILE* f = fopen(path.c_str(), "wb")) {
      fwrite(line.data(), 1, line.size(), f);
      fclose(f);
    }
    ChatRenderer c;
    check(c.open(path, &pi_adapter()), "an in-flight transcript opens");
    c.set_working(true, U'\u280B');
    Theme theme;
    Filters filters;  // Normal density: tool rows are visible
    Surface sf;
    sf.resize(60, 8);
    Painter p(sf, Rect{0, 0, 60, 8});
    c.render(p, theme, filters);

    std::string_view nm, sm;
    check(c.in_flight_tool(&nm, &sm), "an unanswered tool call is in flight");
    check_str(std::string(nm), "read", "the in-flight tool name");
    check_str(std::string(sm), "src/foo.cpp", "the in-flight tool summary");
    std::string screen;
    for (int y = 0; y < 8; y++) {
      for (int x = 0; x < 60; x++) {
        const Cell& cell = sf.at(x, y);
        if (cell.width == 0) continue;
        text::encode(cell.cp ? cell.cp : U' ', screen);
      }
      screen.push_back('\n');
    }
    check(screen.find("\xE2\xA0\x8B") != std::string::npos,
          "the executing tool row shows the spinner");

    // Once the result lands, nothing is in flight.
    const std::string done = path + ".done";
    const std::string both = line +
        R"({"type":"message","message":{"role":"toolResult","toolCallId":"call_r","content":[{"type":"text","text":"ok"}]}})"
        "\n";
    if (FILE* f = fopen(done.c_str(), "wb")) {
      fwrite(both.data(), 1, both.size(), f);
      fclose(f);
    }
    ChatRenderer c2;
    c2.open(done, &pi_adapter());
    c2.set_working(true, U'\u280B');
    Painter p2(sf, Rect{0, 0, 60, 8});
    c2.render(p2, theme, filters);
    check(!c2.in_flight_tool(&nm, &sm), "a tool with a result is no longer in flight");
  }

  // The view is kept across restarts: density, tab, the sidebar's filters,
  // the command history. Written only when it changed.
  {
    const std::string path = config_dir() + "/view";
    unlink(path.c_str());
    {
      App a;
      a.save_view_if_changed();
      check(access(path.c_str(), F_OK) != 0, "view: nothing changed, nothing written");
      a.filters().density = Density::Full;
      a.open_tab(4);
      a.set_all_folders(true);
      a.save_view_if_changed();
    }
    App b;
    std::string buf;
    const std::string_view kept = fs::read_prefix(path, 4096, buf);
    check(b.filters().density == Density::Full && b.filtering() && b.all_folders() &&
              kept.find("tab Diff\n") != std::string_view::npos,
          "view: density, tab and filters come back after a restart");
    unlink(path.c_str());
  }

  // A tracked folder can be gone (an unmounted drive); spawning there must not
  // just make a dead pane.
  {
    App app;
    check_str(app.usable_cwd("/tmp"), "/tmp", "an existing folder is used as-is");
    const std::string fallback = app.usable_cwd("/nonexistent-mico-test-dir");
    check(fallback != "/nonexistent-mico-test-dir", "a missing folder falls back");
    struct stat st{};
    check(stat(fallback.c_str(), &st) == 0 && S_ISDIR(st.st_mode),
          "the fallback folder actually exists");
  }

  // Anything the encoder emits has to reproduce the surface exactly. Replaying
  // it through our own emulator checks both halves at once, and is the safety
  // net for optimising the byte stream.
  {
    App app;
    const int W = 100, H = 30;
    Surface back, front;
    back.resize(W, H);
    Vt vt;
    vt.resize(W, H);

    int mismatches = 0;
    size_t bytes = 0;
    for (int i = 0; i < 2; i++)
      app.feed(InputEvent{InputEvent::Type::Key, KeyEvent{Key::Tab}, {}, {}});
    for (int step = 0; step < 12; step++) {
      if (step) app.feed(InputEvent{InputEvent::Type::Key, KeyEvent{Key::Up}, {}, {}});
      app.draw(back);
      std::string frame;
      encode_frame(back, front, frame, step == 0);
      bytes += frame.size();
      vt.write(frame);

      for (int y = 0; y < H && mismatches < 4; y++) {
        const VtRow& r = vt.row(vt.total_rows() - H + y);
        for (int x = 0; x < W && size_t(x) < r.size(); x++) {
          const Cell& a = back.at(x, y);
          const Cell& b = r[size_t(x)];
          if (a.width == 0) continue;
          if (a.cp != b.cp || !(a.st == b.st)) {
            if (++mismatches <= 2)
              printf("  FAIL  step %d cell (%d,%d): want U+%04X fg=%06X bg=%06X a=%d,"
                     " got U+%04X fg=%06X bg=%06X a=%d\n",
                     step, x, y, unsigned(a.cp), unsigned(a.st.fg), unsigned(a.st.bg), a.st.a,
                     unsigned(b.cp), unsigned(b.st.fg), unsigned(b.st.bg), b.st.a);
          }
        }
      }
    }
    if (mismatches) g_fail++;
    printf("  encoder round-trip: %d mismatching cells over 12 frames (%zu bytes)\n",
           mismatches, bytes);
  }

  // The live strip must not splice an agent's input box into the chat.
  {
    auto row = [](std::string_view utf8) {
      VtRow r;
      size_t i = 0;
      while (i < utf8.size()) {
        char32_t cp = text::decode(utf8, i);
        r.push_back(Cell{cp, Style{}, uint8_t(std::max(1, text::cp_width(cp)))});
      }
      return r;
    };
    check(row_is_blank(row("   ")), "spaces are blank");
    check(!row_is_blank(row(" x ")), "text is not blank");
    check(row_is_chrome(row("\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80")), "a rule is chrome");
    check(row_is_chrome(row("\xE2\x95\xAD\xE2\x94\x80\xE2\x95\xAE")), "a box top is chrome");
    check(row_is_chrome(row("\xE2\x96\x88\xE2\x96\x88")), "block shading is chrome");
    check(!row_is_chrome(row("\xE2\x94\x82 hello \xE2\x94\x82")), "a bordered line of text is not chrome");
    check(!row_is_chrome(row("   ")), "a blank row is not chrome");
    check(!row_is_chrome(row("\xE2\x9D\xAF ask me")), "a prompt line is not chrome");
  }

  // The live strip must show work in progress and nothing else. An agent's
  // screen ends with a bordered input box and a status line, and one that
  // renders inline can briefly hold two copies of that box mid-repaint.
  {
    std::string rule;
    for (int i = 0; i < 58; i++) rule += "\xE2\x94\x80";
    const std::string box = rule + "\r\n\xE2\x9D\xAF typed text\r\n" + rule +
                            "\r\n\xE2\x8F\xB5\xE2\x8F\xB5 auto mode on\r\n";

    auto strip = [&](const std::string& screen) {
      Vt vt;
      vt.resize(60, 14);
      vt.write(screen);
      std::vector<int> rows;
      live_rows(vt, rows, 4);
      std::string out;
      for (int y : rows) {
        for (const Cell& c : vt.row(y)) {
          if (c.width == 0) continue;
          text::encode(c.cp ? c.cp : U' ', out);
        }
        while (!out.empty() && out.back() == ' ') out.pop_back();
        out += "|";
      }
      return out;
    };

    // The strip is a two-line hint at the agent's current activity, taken from
    // the flush-left lines at the bottom of its screen. The input box, the
    // status line, anything at or above a completed-message bullet, and any
    // indented row (a wrapped continuation or nested tool detail the transcript
    // will capture) are all dropped.
    check_str(strip("thinking about it\r\ndoing something\r\n\r\n" + box),
              "thinking about it|doing something|", "the input box is dropped");
    check_str(strip("working\r\n" + box + box), "working|",
              "two stacked input boxes are both dropped");
    check(strip(box).empty(), "an agent showing only its box contributes no strip");
    check_str(strip("first\r\ntwo\r\nthree\r\n"), "two|three|",
              "at most two lines, the most recent");
    check(strip("\xE2\x97\x8F I'll take a look at the whole codebase and\r\n"
                "  report what I find in a moment.\r\n"
                "\xE2\x9C\xBB Thinking\xE2\x80\xA6 (8s)\r\n").empty(),
          "a wrapped completed message does not leak into the strip");
    check_str(strip("\xE2\x97\x8F done earlier\r\nListing project files\r\n"
                    "\xE2\x9C\xB3 Working\xE2\x80\xA6 (10s)\r\n"),
              "Listing project files|", "a flush-left activity line is kept");
    check_str(strip(rule + "\r\nalpha\r\nbeta\r\ngamma\r\ndelta\r\nepsilon\r\n"),
              "delta|epsilon|", "a bare rule is dropped, the text around it kept");
    // The strip only shows a flush-left line that reads like an action — it
    // starts with a letter, digit, path or shell token. A line led by any
    // symbol is the agent's own chrome, so spinner frames are rejected whatever
    // glyph they are on this second, and a warning or status line with them.
    check(strip("* Choreographing\xE2\x80\xA6 (49s)\r\n").empty(),
          "an asterisk spinner line is dropped");
    check(strip("\xE2\x9C\xB1 Pondering (12s)\r\n").empty(),
          "a heavy-star spinner frame is dropped");
    check(strip("\xE2\x9A\xA0 1 startup issue \xC2\xB7 ctrl + t for details\r\n"
                "\xE2\x80\xA2 Working (5s \xC2\xB7 esc to interrupt)\r\n").empty(),
          "a fresh agent showing only its status lines contributes no strip");
    check_str(strip("\xE2\x97\x8F earlier reply\r\n\r\nBash(cmake --build)\r\n"
                    "* Compiling (8s \xC2\xB7 1.2k tokens)\r\n"),
              "Bash(cmake --build)|", "an activity line above the spinner is kept");
  }

  // pi and omp render a chat of their own into the terminal and do not write
  // the in-flight turn to the transcript until it completes, so any tail of
  // their screen is their rendering of a turn mico will render itself. Their
  // chat views are transcript-only: no rows are spliced, whatever the screen
  // looks like.
  {
    std::string rule;
    for (int i = 0; i < 30; i++) rule += "\xE2\x94\x80";
    const std::string pi_screen =
        "settled reply\r\n\r\n"
        "\x1b[48;2;52;53;65mreply to me\x1b[49m\r\n"
        "\r\nhello\r\n\r\n" +
        rule + "\r\n\r\n" + rule + "\r\n/tmp\r\n$0.000 (sub)\r\n";
    const std::string omp_screen =
        "\xE2\x95\xAD\xE2\x94\x80\xE2\x94\x80\xE2\x95\xAE\r\n"
        "\xE2\x94\x82 hi \xE2\x94\x82\r\n"
        "\xE2\x95\xB0\xE2\x94\x80\xE2\x94\x80\xE2\x95\xAF\r\n"
        "\xF0\x9F\x90\x99 > /tmp > 6%\r\n";

    for (auto [agent, screen] : {std::pair{"pi", pi_screen}, std::pair{"omp", omp_screen}}) {
      Vt vt;
      vt.resize(60, 16);
      vt.write(screen);
      std::vector<int> rows;
      adapter_for(agent)->live_rows(vt, rows, 4);
      check(rows.empty(), "pi/omp contribute no live strip");
    }
    // Claude and Codex keep the generic scan.
    Vt vt;
    vt.resize(60, 16);
    vt.write("thinking about it\r\ndoing something\r\n\r\n" + rule + "\r\n" + rule + "\r\n");
    std::vector<int> rows;
    claude_adapter().live_rows(vt, rows, 4);
    check(!rows.empty(), "claude still gets a live strip");
  }

  // The picker behind every menu, palette and panel of choices.
  {
    std::vector<uint32_t> marks;
    check(fuzzy_match("Fork into a new chat", "fork", &marks) > fuzzy_match("Unfork it", "fork"),
          "fuzzy: a match at the start ranks first");
    check(marks.size() == 4 && marks[0] == 0, "fuzzy: marks the matched bytes");
    check(fuzzy_match("new chat", "nc", &marks) >= 0 && marks.size() == 2 && marks[1] == 4,
          "fuzzy: a subsequence takes the next word start");
    check(fuzzy_match("density", "xyz") < 0, "fuzzy: no match");
    check(fuzzy_match("/doctor", "/co") < 0, "fuzzy: letters scattered inside a word do not match");
    check(fuzzy_match("/remote-control", "/co") >= 0, "fuzzy: but a word they start does");
    check(fuzzy_match("Résumé", "rés") >= 0, "fuzzy: non-ASCII matches exactly");
    check(fuzzy_match("tool calls", "tc") > fuzzy_match("fetch", "tc"), "fuzzy: word starts beat a run inside a word");

    auto row = [](const Surface& s, int y) {
      std::string out;
      for (int x = 0; x < s.width(); x++)
        if (s.at(x, y).width) text::encode(s.at(x, y).cp, out);
      while (!out.empty() && out.back() == ' ') out.pop_back();
      return out;
    };
    auto items = [](std::initializer_list<const char*> labels) {
      std::vector<PickItem> v;
      for (const char* l : labels) {
        PickItem it;
        it.label = l;
        it.id = l;
        v.push_back(std::move(it));
      }
      return v;
    };
    auto key = [](Key k, char32_t ch = 0) {
      KeyEvent e;
      e.key = k;
      e.ch = ch;
      return e;
    };
    Theme th;
    using R = Picker::Result;

    // A menu: no cursor until a key; Down lands on the first entry and skips
    // a separator and a disabled entry; Enter picks.
    {
      Picker::Options o;
      o.start_unselected = true;
      o.filter = true;
      Picker pk(o);
      auto v = items({"Open", "Rename", "", "Archive", "Stop agent"});
      v[2].separator = true;
      v[3].enabled = false;
      pk.set_items(std::move(v));
      check(pk.cursor() == -1, "picker: a clicked menu starts with no cursor");
      check(pk.on_key(key(Key::Down)) == R::Moved && pk.cursor() == 0, "picker: Down lands on the first");
      pk.on_key(key(Key::Down));
      check(pk.on_key(key(Key::Down)) == R::Moved && pk.cursor() == 4, "picker: skips separators and disabled");
      check(pk.on_key(key(Key::Down)) == R::Moved && pk.cursor() == 0, "picker: wraps round");
      check(pk.on_key(key(Key::Up)) == R::Moved && pk.cursor() == 4, "picker: wraps back");
      check(pk.on_key(key(Key::Enter)) == R::Chosen && pk.index() == 4, "picker: Enter chooses");

      // Typing filters and ranks; the cursor goes to the best match.
      check(pk.on_key(key(Key::Char, 'a')) == R::QueryChanged, "picker: typing edits the query");
      pk.on_key(key(Key::Char, 'g'));
      check(pk.visible_count() == 1 && pk.cursor() == 4, "picker: 'ag' leaves Stop agent");
      Surface s;
      s.resize(40, 12);
      const int h = pk.rows(30, 12);
      check(h == 4, "picker: border, query row and one item");
      pk.render(Painter(s, Rect{0, 0, 30, h}), th);
      check(row(s, 1).find("ag") != std::string::npos && row(s, 1).find("1/4") != std::string::npos,
            "picker: the query row shows the query and the count");
      check(row(s, 2).find("Stop agent") != std::string::npos, "picker: draws the match");
      MouseEvent click;
      click.kind = MouseKind::Press;
      click.button = MouseButton::Left;
      check(pk.on_mouse(click, Point{5, 2}) == R::Chosen && pk.index() == 4, "picker: a click chooses");
      check(pk.on_key(key(Key::Escape)) == R::QueryChanged && pk.visible_count() == 4,
            "picker: Escape clears the query first");
      check(pk.on_key(key(Key::Escape)) == R::Cancelled, "picker: then cancels");
      pk.on_key(key(Key::Char, 'z'));
      pk.on_key(key(Key::Char, 'z'));
      check(pk.visible_count() == 0 && pk.on_key(key(Key::Enter)) == R::Handled, "picker: nothing to choose");
      pk.render(Painter(s, Rect{0, 0, 30, pk.rows(30, 12)}), th);
      check(row(s, 2).find("no matches") != std::string::npos, "picker: says when nothing matches");
    }

    // Words apart: each has to match the label or the detail.
    {
      Picker::Options o;
      o.filter = true;
      Picker pk(o);
      auto v = items({"Investigate bugs", "Question form", "Review"});
      v[0].detail = "mico \xC2\xB7 Codex";
      v[1].detail = "mico \xC2\xB7 Claude";
      v[2].detail = "gamedev \xC2\xB7 Codex";
      pk.set_items(std::move(v));
      pk.set_query("mico codex");
      check(pk.visible_count() == 1 && pk.cursor() == 0, "picker: each word may match the label or the detail");
      pk.set_query("codex rev");
      check(pk.visible_count() == 1 && pk.cursor() == 2, "picker: in any order");
    }

    // Multi-select with numbers and groups: Space and digits toggle, Enter
    // confirms, headings are not stops.
    {
      Picker::Options o;
      o.frame = Picker::Frame::Panel;
      o.title = "Pick tests";
      o.multi = true;
      o.numbers = true;
      Picker pk(o);
      auto v = items({"unit", "fuzz", "e2e"});
      v[0].group = "fast";
      v[1].group = "fast";
      v[2].group = "slow";
      pk.set_items(std::move(v));
      check(pk.cursor() == 0, "picker: a panel starts on its first choice");
      check(pk.on_key(key(Key::Char, ' ')) == R::Toggled && pk.items()[0].checked, "picker: Space marks");
      check(pk.on_key(key(Key::Char, '3')) == R::Toggled && pk.items()[2].checked, "picker: a digit marks the nth");
      pk.on_key(key(Key::Up));
      check(pk.cursor() == 1, "picker: Up skips the heading");
      check(pk.on_key(key(Key::Enter)) == R::Confirmed && pk.checked() == std::vector<int>({0, 2}),
            "picker: Enter confirms the marked set");
      KeyEvent all = key(Key::Char, 'a');
      all.ctrl = true;
      pk.on_key(all);
      check(pk.checked().size() == 3, "picker: Ctrl+A marks everything");
      pk.on_key(all);
      check(pk.checked().empty(), "picker: and again clears it");
      Surface s;
      s.resize(40, 10);
      const int h = pk.rows(40, 10);
      check(h == 7, "picker: title, two headings, three items, footer");
      pk.render(Painter(s, Rect{0, 0, 40, h}), th);
      check(row(s, 0).find("Pick tests") != std::string::npos, "picker: panel title");
      check(row(s, 1).find("fast") != std::string::npos && row(s, 4).find("slow") != std::string::npos,
            "picker: group headings");
      check(row(s, 3).find("\xE2\x9D\xAF") != std::string::npos && row(s, 3).find("2.") != std::string::npos,
            "picker: the cursor mark and the number");
      check(row(s, 6).find("space mark") != std::string::npos, "picker: the key hint");
    }

    // A long list scrolls to keep the cursor in view, and a refresh with the
    // same items keeps the cursor where it was.
    {
      Picker pk;
      std::vector<PickItem> v;
      for (int i = 0; i < 30; i++) {
        PickItem it;
        it.label = "item " + std::to_string(i);
        v.push_back(it);
      }
      pk.set_items(v);
      Surface s;
      s.resize(30, 8);
      pk.render(Painter(s, Rect{0, 0, 30, 8}), th);
      for (int i = 0; i < 20; i++) pk.on_key(key(Key::Down));
      pk.render(Painter(s, Rect{0, 0, 30, 8}), th);
      check(row(s, 6).find("item 20") != std::string::npos, "picker: scrolls to the cursor");
      pk.set_items(v);
      check(pk.cursor() == 20, "picker: a refresh keeps the cursor");
      pk.on_key(key(Key::PageUp));
      check(pk.cursor() == 15, "picker: PageUp moves a page");
    }
  }

  // The "/" and "@" menus of the prompt box.
  {
    Trigger t = find_trigger("/comp", 5);
    check(t.kind == '/' && t.from == 0 && t.end == 5 && t.query == "comp", "trigger: a command word");
    check(find_trigger("say /comp", 9).kind == 0, "trigger: a slash later in the message is not a command");
    t = find_trigger("look at @src/ui and", 15);
    check(t.kind == '@' && t.from == 8 && t.end == 15 && t.query == "src/ui", "trigger: a mention mid-message");
    t = find_trigger("look at @src/ui and", 11);
    check(t.kind == '@' && t.end == 15 && t.query == "sr", "trigger: the word runs past the cursor");
    check(find_trigger("mail a@b", 8).kind == 0, "trigger: an address is not a mention");
    check(find_trigger("", 0).kind == 0, "trigger: nothing in an empty box");

    const std::vector<std::string> paths = {"README.md", "src/", "src/ui/", "src/ui/picker.cpp",
                                            "src/ui/picker.h", "tests/", "tests/pick_test.py",
                                            "src/core/pty.cpp"};
    std::vector<int> r = rank_paths(paths, "", 10);
    check(r.size() == 3 && paths[size_t(r[0])] == "src/" && paths[size_t(r[2])] == "README.md",
          "rank_paths: an empty query lists the top level, folders first");
    r = rank_paths(paths, "picker", 10);
    check(r.size() >= 2 && paths[size_t(r[0])].find("picker") != std::string::npos, "rank_paths: the name matches first");
    r = rank_paths(paths, "pty", 10);
    check(!r.empty() && paths[size_t(r[0])] == "src/core/pty.cpp", "rank_paths: a short name");
    r = rank_paths(paths, "ui/pi", 10);
    check(!r.empty() && paths[size_t(r[0])].starts_with("src/ui/picker"), "rank_paths: a query with folders");

    const std::string answer =
        R"({"type":"control_response","response":{"subtype":"success","request_id":"mico-commands","response":{"commands":[)"
        R"({"name":"compact","description":"Free up context by summarizing\nthe conversation","argumentHint":"<optional instructions>","builtin":true},)"
        R"({"name":"__remote-workflow","description":"cloud only","argumentHint":""},)"
        R"({"name":"frontend:review","description":"Review \"the\" UI","argumentHint":""}],"models":[]}}})";
    const auto cmds = parse_claude_commands(answer);
    check(cmds.size() == 2, "claude commands: parsed, cloud-only ones dropped");
    check(cmds.size() == 2 && cmds[0].name == "compact" && cmds[0].description == "Free up context by summarizing" &&
              cmds[0].hint == "<optional instructions>",
          "claude commands: name, first line of the description, hint");
    check(cmds.size() == 2 && cmds[1].description == "Review \"the\" UI", "claude commands: unescaped");
    check(!builtin_commands("codex").empty() && !builtin_commands("pi").empty() &&
              !builtin_commands("omp").empty() && builtin_commands("gemini").empty(),
          "built-in command tables");

    {
      const std::string home = "/tmp/mico-selftest-cmds-" + std::to_string(getpid());
      mkdir(home.c_str(), 0700);
      mkdir((home + "/.pi").c_str(), 0700);
      mkdir((home + "/.pi/agent").c_str(), 0700);
      mkdir((home + "/.pi/agent/prompts").c_str(), 0700);
      mkdir((home + "/.pi/agent/skills").c_str(), 0700);
      mkdir((home + "/.pi/agent/skills/lint").c_str(), 0700);
      put_file(home + "/.pi/agent/prompts/fixup.md", "---\ndescription: Fix up the last commit\n---\nDo it.\n");
      put_file(home + "/.pi/agent/skills/lint/SKILL.md", "---\nname: lint\ndescription: \"Run the linters\"\n---\n");
      auto fc = file_commands("pi", "/nonexistent", home);
      bool prompt = false, skill = false;
      for (const auto& c : fc) {
        prompt |= c.name == "fixup" && c.description == "Fix up the last commit";
        skill |= c.name == "skill:lint" && c.description == "Run the linters";
      }
      check(prompt, "pi: a prompt template is a command");
      check(skill, "pi: a skill is /skill:name");
      check(file_commands("claude", "/nonexistent", home).empty(), "claude's own files come from claude");
      if (system(("rm -rf '" + home + "'").c_str()) != 0) {}
    }

    // The menu itself: built from the lists, narrowed by the word, and what
    // taking an item puts in the box.
    Completion c;
    std::vector<SlashCommand> list = {{"clear", "Start over", ""}, {"compact", "Summarize", "<instructions>"},
                                      {"model", "Pick a model", "[model]"}};
    c.set_commands(&list, 1);
    c.update("/co", 3, true);
    check(c.visible() && c.picker().visible_count() == 1, "completion: '/co' leaves /compact");
    Completion::Take take = c.take();
    check(take.text == "/compact " && !take.send, "completion: a required argument waits");
    c.update("/mo", 3, true);
    take = c.take();
    check(take.text == "/model " && take.send, "completion: an optional argument runs on Enter");
    c.update("/zz", 3, true);
    check(!c.visible(), "completion: no match, no menu");
    c.update("/c", 2, true);
    c.dismiss();
    check(!c.visible(), "completion: Escape closes it");
    c.update("/co", 3, true);
    check(!c.visible(), "completion: and it stays closed for that word");
    c.update("", 0, true);
    c.update("/", 1, true);
    check(c.visible() && c.picker().visible_count() == 3, "completion: a new word opens it again");
    c.update("/c", 2, false);
    check(!c.visible(), "completion: not while the box holds a note");

    c.set_files(&paths, 1);
    c.update("see @picker.c", 13, true);
    check(c.visible() && c.picker().items()[size_t(c.picker().cursor())].id == "src/ui/picker.cpp",
          "completion: '@picker.c' finds the file");
    c.update("see @src", 8, true);
    take = c.take();
    check(take.text == "@src/" && take.keep_open, "completion: a folder keeps the menu open");
    c.update("see @src/ui/picker.h", 20, true);
    take = c.take();
    check(take.text == "@src/ui/picker.h " && !take.send, "completion: a file ends the word");
  }

  // The outline of a chat (Ctrl+G): your messages, edits, failures, in
  // order, each with the byte offset of its line.
  {
    const std::string path = "/tmp/mico-selftest-outline-" + std::to_string(getpid()) + ".jsonl";
    std::string body;
    body += R"({"type":"user","message":{"role":"user","content":"make the arrows nicer"}})" "\n";
    body += R"({"type":"assistant","message":{"role":"assistant","content":[{"type":"tool_use","id":"t1","name":"Bash","input":{"command":"make test"}}]}})" "\n";
    body += R"({"type":"user","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"t1","is_error":true,"content":"boom"}]}})" "\n";
    body += R"({"type":"assistant","message":{"role":"assistant","content":[{"type":"tool_use","id":"t2","name":"Edit","input":{"file_path":"/src/arrow.cpp","old_string":"a","new_string":"b"}}]}})" "\n";
    const size_t second = body.size();
    body += R"({"type":"user","message":{"role":"user","content":"and the panel\nplus the grid"}})" "\n";
    put_file(path, body);
    ChatRenderer c;
    SessionRef r;
    r.agent = "claude";
    r.path = path;
    c.open(path, Store::adapter_for(r));
    std::vector<ChatRenderer::OutlineEntry> o;
    check(c.outline(o, 1000), "outline: a small chat is read whole");
    std::string kinds;
    for (const auto& e : o) kinds += e.kind;
    check_str(kinds, "uxeu", "outline: you, the failure, the edit, you");
    if (o.size() == 4) {
      check(o[0].label == "make the arrows nicer" && o[0].offset == 0, "outline: a message and where it is");
      check(o[1].label == "make test" && o[1].detail == "Bash failed", "outline: a failure names its call");
      check(o[2].label.find("arrow.cpp") != std::string::npos, "outline: an edit names its file");
      check(o[3].offset == second, "outline: offsets are byte offsets");
    }
    // The full text behind an outline entry, for ↑ history: the label is one
    // line, the recall is the message as written.
    {
      std::string full;
      check(c.user_text_at(o[0].offset, &full) && full == "make the arrows nicer",
            "user_text_at: a message by its offset");
      check(!c.user_text_at(o[1].offset, &full), "user_text_at: a tool failure is not a message");
    }

    // Only what is new is read on the next call.
    body += R"({"type":"user","message":{"role":"user","content":"one more"}})" "\n";
    put_file(path, body);
    c.poll_growth();
    c.outline(o, 1000);
    check(o.size() == 5 && o.back().label == "one more", "outline: grows with the chat");
    std::string full;
    check(c.user_text_at(o[3].offset, &full) && full == "and the panel\nplus the grid",
          "user_text_at: a multi-line message comes back whole");
    unlink(path.c_str());
  }

  // Claude's subscription limits: claude hands them to its status line, which
  // mico gives it as itself; the Usage tab reads what that kept.
  {
    Launch l;
    l.agent = "claude";
    LaunchExtras x;
    x.status_exe = "/opt/it's mico/mico";
    claude_adapter().prepare(l, x);
    const auto at = std::find(l.argv.begin(), l.argv.end(), "--settings");
    check(at != l.argv.end() && at + 1 != l.argv.end() &&
              at[1].find(R"("statusLine":{"type":"command","command":"'/opt/it'\\''s mico/mico' --claude-status"})") !=
                  std::string::npos &&
              at[1].find("PermissionRequest") != std::string::npos,
          "limits: claude's status line is mico, its path quoted for the shell, beside mico's hooks");
    Launch own;
    own.agent = "claude";
    own.argv = {"claude", "--settings", "mine.json"};
    claude_adapter().prepare(own, x);
    check(std::count(own.argv.begin(), own.argv.end(), "--settings") == 1, "limits: settings given by hand are left alone");

    char exe[4096];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    const std::string self = n > 0 ? std::string(exe, size_t(n)) : std::string();
    char tmpl[] = "/tmp/mico_limits_XXXXXX";
    const std::string dir = mkdtemp(tmpl) ? tmpl : "/tmp";
    const auto status = [&](const std::string& json, const std::string& out) {
      if (FILE* f = popen(("'" + self + "' --claude-status > '" + out + "'").c_str(), "w")) {
        fputs(json.c_str(), f);
        pclose(f);
      }
    };
    const int64_t now = int64_t(time(nullptr));
    const std::string reset5 = std::to_string(now + 3600), reset7 = std::to_string(now + 5 * 86400);
    status(R"({"model":{"id":"x"},"workspace":{"current_dir":"/nowhere","project_dir":"/nowhere"},)"
           R"("rate_limits":{"five_hour":{"used_percentage":23.5,"resets_at":)" + reset5 +
               R"(},"seven_day":{"used_percentage":61,"resets_at":)" + reset7 + "}}}",
           dir + "/out1");
    std::vector<PlanLimit> got;
    claude_adapter().plan_limits(got);
    std::string printed;
    {
      std::string buf;
      printed = std::string(fs::read_prefix(dir + "/out1", 4096, buf));
    }
    check(got.size() == 2 && got[0].window == "5 hours" && got[0].used_pct == 23.5 &&
              got[0].resets_at == now + 3600 && got[1].window == "week" && got[1].used_pct == 61 &&
              got[1].as_of >= now - 5 && printed.empty(),
          "limits: what claude's status line is handed is kept, and nothing is printed");

    // Without limits (no request yet) the last ones stay; the user's own
    // status line, set for the project, still runs and prints.
    mkdir((dir + "/.claude").c_str(), 0700);
    put_file(dir + "/.claude/settings.json", R"({"statusLine":{"type":"command","command":"cat > ')" + dir +
                                                 R"(/seen'; echo mine"}})");
    status(R"({"workspace":{"current_dir":")" + dir + R"(","project_dir":")" + dir + R"("}})", dir + "/out2");
    got.clear();
    claude_adapter().plan_limits(got);
    std::string buf2, buf3;
    const std::string mine(fs::read_prefix(dir + "/out2", 4096, buf2));
    const std::string seen(fs::read_prefix(dir + "/seen", 4096, buf3));
    check(got.size() == 2 && mine == "mine\n" && seen.find("project_dir") != std::string::npos,
          "limits: the user's own status line runs on the same input, and shows");
    for (const char* f : {"/out1", "/out2", "/seen", "/.claude/settings.json"}) unlink((dir + f).c_str());
    rmdir((dir + "/.claude").c_str());
    rmdir(dir.c_str());
  }

  // Claude's /btw panel, read off its screen: the questions, which one is
  // shown, the answer with its wrapping undone, and the answering state.
  {
    std::string rule;
    for (int i = 0; i < 60; i++) rule += "\xE2\x96\x94";
    const std::string dim = "\x1b[38;5;246m", bold = "\x1b[1m", off = "\x1b[0m";
    auto panel = [&](const std::string& body, const std::string& hint) {
      Vt vt;
      vt.resize(60, 20);
      vt.write("\xE2\x97\x8F PINEAPPLE\r\n\r\n\x1b[38;5;153m" + rule + "\x1b[39m\r\n\r\n" +
               "    " + dim + "/btw what fruit?" + off + "\r\n" +
               "    " + bold + "\x1b[38;5;220m/btw " + off + dim + "why is the sky blue" + off + "\r\n\r\n" +
               body + "\r\n    " + dim + hint + off + "\r\n");
      return vt;
    };
    BtwPanel b;
    // 57 columns, and "which" would not have fitted before 58: wrapped there.
    Vt vt = panel("      The sky looks blue because of " + bold + "Rayleigh" + off + " scattering,\r\n"
                  "      which favours short waves.\r\n"
                  "      - one\r\n      - two",
                  "\xE2\x87\xA7\xE2\x86\x90/\xE2\x86\x92 to browse \xC2\xB7 c to copy \xC2\xB7 Esc to close");
    check(parse_btw_panel(vt, b), "btw: the panel is found");
    check(b.questions.size() == 2 && b.questions[0] == "what fruit?" && b.current == 1 &&
              b.questions[1] == "why is the sky blue",
          "btw: every question, the bold one current");
    check_str(b.answer,
              "The sky looks blue because of **Rayleigh** scattering, which favours short waves.\n- one\n- two",
              "btw: the answer as markdown, its wrapping undone");
    check(!b.answering && b.hint.starts_with("\xE2\x87\xA7"), "btw: done, with its key line");

    check(!claude_adapter().awaits_input(vt), "btw: the panel is not a prompt waiting on you");
    vt = panel("      \xE2\x9C\xBD Answering\xE2\x80\xA6", "Esc to close");
    check(parse_btw_panel(vt, b) && b.answering && b.answer.empty(), "btw: answering");

    // In a chat with nothing in it yet, claude draws no rule above the panel.
    Vt bare;
    bare.resize(60, 12);
    bare.write(" tmux focus-events off\r\n\r\n    \x1b[1m/btw grass?\x1b[0m\r\n\r\n      Green.\r\n\r\n"
               "    \x1b[38;5;246mc to copy \xC2\xB7 Esc to close\x1b[39m\r\n");
    check(parse_btw_panel(bare, b) && b.questions.size() == 1 && b.questions[0] == "grass?" &&
              b.answer == "Green.",
          "btw: found without its rule");

    // The ordinary input box is not a panel.
    Vt plain;
    plain.resize(60, 10);
    plain.write("\xE2\x97\x8F done\r\n" + rule + "\r\n\xE2\x9D\xAF \r\n" + rule + "\r\n  esc to interrupt\r\n");
    check(!parse_btw_panel(plain, b), "btw: the input box is not the panel");
  }

  // The reply an agent is writing, read off its screen: both agents show it
  // as it streams, while their transcripts get it only once it is complete.
  {
    std::string rule;
    for (int i = 0; i < 60; i++) rule += "\xE2\x94\x80";
    const std::string white_dot = "\x1b[38;2;255;255;255m\xE2\x97\x8F\x1b[39m ";
    const std::string grey_dot = "\x1b[38;2;153;153;153m\xE2\x97\x8F\x1b[39m ";
    const std::string claude_box = rule + "\r\n\xE2\x9D\xAF \r\n" + rule +
                                   "\r\n  \xE2\x8F\xB8 manual mode on \xC2\xB7 esc to interrupt\r\n";
    auto reply = [](const std::string& screen, const char* agent) {
      Vt vt;
      vt.resize(60, 30);
      vt.write(screen);
      return adapter_for(agent)->screen_reply(vt);
    };
    // Rows 1 and 3 are full enough that the next row's first word could not
    // have fit: the agent wrapped them.
    const std::string claude_screen =
        "\xE2\x9D\xAF explain slow start\r\n\r\n" + white_dot +
        "\x1b[1mTCP Slow Start\x1b[22m\r\n\r\n"
        "  TCP slow start is a congestion control algorithm that\r\n"
        "  gradually increases the \x1b[1mrate\x1b[22m.\r\n"
        "  - Initial window: small, one to ten segments, then it\r\n"
        "    doubles each round trip.\r\n"
        "  - Loss ends it.\r\n\r\n" +
        grey_dot + "Bash(echo hi)\r\n  \xE2\x8E\xBF  hi\r\n"
        "\xE2\x9C\xBB Concocting\xE2\x80\xA6 (6s)\r\n\r\n" + claude_box;
    check_str(reply(claude_screen, "claude"),
              "## TCP Slow Start\n\n"
              "TCP slow start is a congestion control algorithm that gradually increases the **rate**.\n"
              "- Initial window: small, one to ten segments, then it doubles each round trip.\n"
              "- Loss ends it.",
              "claude's reply is read off the screen, unwrapped, with its bold, past the tool after it");
    check(reply("\xE2\x9D\xAF explain slow start\r\n\r\n\xE2\x9C\xBB Concocting\xE2\x80\xA6\r\n\r\n" +
                    claude_box, "claude").empty(),
          "nothing is read back before the agent writes");
    check(reply(white_dot + "an earlier reply\r\n\r\n\xE2\x9D\xAF new question\r\n\r\n" + rule +
                    "\r\n Bash command\r\n\r\n Do you want to proceed?\r\n \xE2\x9D\xAF 1. Yes\r\n   2. No\r\n",
                "claude").empty(),
          "under a dialog, the user's message is not taken for the input box");
    check(reply(claude_screen, "pi").empty(), "only claude and codex are read");
    {
      // Claude's diff of a file, drawn in a panel beside the conversation:
      // its rule and its italic notes are not the reply.
      const auto pad = [](std::string t, size_t to) {
        while (text::str_width(t) < int(to)) t += ' ';
        return t;
      };
      const std::string edge = "\xE2\x94\x82";  // │
      std::string screen = "\xE2\x9D\xAF write the sim\r\n" + pad("", 40) + edge + "\x1b[1mai.cpp\x1b[22m\r\n" +
                           white_dot + pad("Now sim tests, then I'll build the sim", 38) + edge +
                           "\x1b[2;3mNot staged.\x1b[22;23m\r\n" +
                           pad("  before writing the scene.", 40) + edge + "\x1b[2;3mRun `git add`.\x1b[22;23m\r\n";
      for (int i = 0; i < 3; i++) screen += pad("", 40) + edge + "\r\n";
      screen += claude_box;
      check_str(reply(screen, "claude"), "Now sim tests, then I'll build the sim before writing the scene.",
                "claude's reply beside a panel is read without the panel, wrapped at its edge");

      const std::string table = white_dot + "Counts:\r\n\r\n"
                                "  \xE2\x94\x8C\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\xAC\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x90\r\n"
                                "  " + edge + " a " + edge + " b " + edge + "\r\n"
                                "  " + edge + " 1 " + edge + " 2 " + edge + "\r\n"
                                "  \xE2\x94\x94\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\xB4\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x98\r\n\r\n" +
                                claude_box;
      const std::string got = reply(table, "claude");
      check(got.find(" b ") != std::string::npos && got.find(" 2 ") != std::string::npos,
            "a table's rules are not taken for a panel's edge");
    }
    // A running tool's bullet blinks: while it is off, its lines are indented
    // like the reply's, and only the colour left in the first cell tells.
    const std::string after_reply =
        "\xE2\x9D\xAF look at it\r\n\r\n" + white_dot + "Taking a screenshot:\r\n\r\n";
    const std::string tool_rows =
        "Reading /tmp/sand_view.png \xC2\xB7 41s\r\n"
        "  \xE2\x8E\xBF  /tmp/sand_view.png (40s)\r\n"
        "     (ctrl+b to run in background)\r\n\r\n" + claude_box;
    check_str(reply(after_reply + "\x1b[38;2;153;153;153m \x1b[39m " + tool_rows, "claude"),
              "Taking a screenshot:", "a tool whose bullet blinked off is not read as the reply");
    check_str(reply(after_reply + "  " + tool_rows, "claude"), "Taking a screenshot:",
              "nor is one known only by its output row");

    const std::string codex_screen =
        "\x1b[48;5;236m\xE2\x80\xBA explain slow start\x1b[49m\r\n\r\n"
        "\x1b[2m\xE2\x80\xA2\x1b[22m \x1b[1;4m# Slow start\x1b[22;24m\r\n\r\n"
        "  Slow start probes the path.\r\n"
        "  \xE2\x80\xA2 Send: transmit up to cwnd.\r\n\r\n"
        "\x1b[1;38;2;138;138;138m\xE2\x80\xA2\x1b[0m Running echo hi\r\n\r\n"
        "\x1b[1;38;2;242;242;242m\xE2\x80\xA2\x1b[0m Working (5s \xE2\x80\xA2 esc to interrupt)\r\n\r\n"
        "\xE2\x80\xBA Ask Codex to do anything\r\n  ? for shortcuts\r\n";
    check_str(reply(codex_screen, "codex"),
              "# Slow start\n\nSlow start probes the path.\n- Send: transmit up to cwnd.",
              "codex's reply is read off the screen, its bullets as markdown's");
    // Its tool cells wear the reply's dim bullet; a bold title or the "└"
    // under it says what they are.
    const std::string explored =
        "\x1b[2m\xE2\x80\xA2\x1b[22m \x1b[1mExplored\x1b[22m\r\n"
        "  \xE2\x94\x94 Read DATASET.md\r\n    Read README.md\r\n    List DeepDRiD\r\n\r\n";
    const std::string box = "\xE2\x80\xBA Ask Codex to do anything\r\n  ? for shortcuts\r\n";
    check_str(reply("\xE2\x80\xBA look at the data\r\n\r\n" + explored + box, "codex"), "",
              "codex: an Explored cell is not its reply");
    check_str(reply("\xE2\x80\xBA look at the data\r\n\r\n\x1b[2m\xE2\x80\xA2\x1b[22m I will read the docs first.\r\n\r\n" +
                        explored + box, "codex"),
              "I will read the docs first.", "codex: the reply above a tool cell is still read");
    check_str(reply("\xE2\x80\xBA go\r\n\r\n\x1b[2m\xE2\x80\xA2\x1b[22m \x1b[1mSomething new\x1b[22m\r\n"
                    "  \xE2\x94\x94 detail\r\n\r\n" + box, "codex"),
              "", "codex: an unknown cell with details under a \xE2\x94\x94 is a tool cell too");
    // Codex highlights a code block whole: it goes back in a fence, blank
    // line and all, and none of its lines is joined to another.
    const std::string code = "\x1b[38;2;205;214;244m";
    check_str(reply("\xE2\x80\xBA show code\r\n\r\n"
                    "\x1b[2m\xE2\x80\xA2\x1b[22m Like this:\r\n\r\n"
                    "  " + code + "item_count = 2\x1b[39m\r\n\r\n"
                    "  " + code + "print(item_count)\x1b[39m\r\n\r\n"
                    "  Done.\r\n\r\n"
                    "\xE2\x80\xBA Ask Codex to do anything\r\n", "codex"),
              "Like this:\n\n```\nitem_count = 2\n\nprint(item_count)\n```\nDone.",
              "codex's highlighted code comes back fenced");

    // Streaming, Claude drops its spinner line; the footer still says it works.
    Vt vt;
    vt.resize(60, 30);
    vt.write(white_dot + "streaming a reply\r\n\r\n" + claude_box);
    check(screen_shows_claude_activity(vt), "claude streaming a reply counts as working");
    Vt idle;
    idle.resize(60, 30);
    idle.write(white_dot + "done\r\n\r\n" + rule + "\r\n\xE2\x9D\xAF \r\n" + rule +
               "\r\n  \xE2\x8F\xB5\xE2\x8F\xB5 auto mode on (shift+tab to cycle)\r\n");
    check(!screen_shows_claude_activity(idle), "claude at its prompt is idle");
  }

  // Compaction: the summary handed to the model is not shown; one notice marks
  // where the conversation was compacted. The spinner's own words say when it
  // is compacting right now.
  {
    Arena arena;
    std::vector<Event> ev;
    claude_adapter().parse(
        R"({"parentUuid":"a","type":"system","subtype":"compact_boundary","content":"Conversation compacted","level":"info"})",
        arena, ev);
    claude_adapter().parse(
        R"({"parentUuid":"b","type":"user","message":{"role":"user","content":"This session is being continued from a previous conversation. Summary: ..."},"isVisibleInTranscriptOnly":true,"isCompactSummary":true})",
        arena, ev);
    claude_adapter().parse(
        R"({"parentUuid":"c","type":"system","subtype":"api_error","content":"retrying"})", arena, ev);
    claude_adapter().parse(
        R"({"parentUuid":"d","type":"user","message":{"role":"user","content":"a real message"}})", arena, ev);
    check(ev.size() == 2 && ev[0].kind == EventKind::Notice &&
              arena.view(ev[0].text) == "Conversation compacted" && ev[1].kind == EventKind::User,
          "claude compaction: a notice, no summary, other system records ignored");
    ev.clear();
    codex_adapter().parse(
        R"({"timestamp":"t","type":"compacted","payload":{"message":"","replacement_history":[{"type":"message","role":"user","content":[{"type":"input_text","text":"summary"}]}]}})",
        arena, ev);
    check(ev.size() == 1 && ev[0].kind == EventKind::Notice, "codex compaction: a notice, not its history");
  }
  // Codex 0.159 answers an optional question with an envelope naming it.
  {
    const std::string env = async_reply_envelope({AsyncReply{"call_9", 1, "Ship it\nnow?", "Yes, \"today\""}});
    check_str(env,
              "<send_user_message_question_reply>\n"
              R"([{"answer":"Yes, \"today\"","question":"Ship it now?","questionItemId":"[\"request_user_input_async\",\"call_9\",1]"}])"
              "\n</send_user_message_question_reply>",
              "async reply: written byte for byte as codex writes it");
    std::vector<AsyncReply> rs;
    check(parse_async_reply("\n" + env + "\n", rs) && rs.size() == 1 && rs[0].call_id == "call_9" && rs[0].index == 1 &&
              rs[0].question == "Ship it now?" && rs[0].answer == "Yes, \"today\"",
          "async reply: read back");
    check(!parse_async_reply("> Ship it now?\n\nYes", rs), "async reply: a quoted reply is not an envelope");
    check(parse_async_reply(async_reply_envelope({AsyncReply{"c", 0, "A?", "a"}, AsyncReply{"c", 1, "B?", "b"}}), rs) &&
              rs.size() == 2 && rs[1].index == 1 && rs[1].answer == "b",
          "async reply: several answers in one envelope");
    Arena a;
    std::vector<Event> ev;
    codex_adapter().parse(
        R"({"type":"response_item","payload":{"type":"message","role":"user","content":[{"type":"input_text","text":)" +
            js::quote(env) + "}]}}",
        a, ev);
    check(ev.size() == 1 && ev[0].kind == EventKind::User && ev[0].tool_id == hash_id("call_9") &&
              a.view(ev[0].text) == "> Ship it now?\n\nYes, \"today\"",
          "async reply: codex's envelope reads as the quoted reply, tied to its question");
    ev.clear();
    codex_adapter().parse(
        R"({"type":"response_item","payload":{"type":"function_call","name":"request_user_input_async","call_id":"call_9","arguments":"{\"questions\":[{\"title\":\"Ship it now?\"}]}"}})",
        a, ev);
    check(ev.size() == 1 && ev[0].kind == EventKind::Question && a.view(ev[0].summary) == "call_9",
          "async question: keeps the call id its answer names");

    Vt vt; vt.resize(80, 20);
    vt.write("● Earlier we discussed Compacting strategies\r\n✻ Compacting conversation… (esc to interrupt)\r\n❯ \r\n");
    check(screen_shows_claude_activity(vt) &&
              claude_activity_line(vt).find("Compacting conversation") != std::string::npos,
          "the spinner line says Claude is compacting");
    Vt thinking; thinking.resize(80, 20);
    thinking.write("● Earlier we discussed Compacting strategies\r\n✻ Thinking… (8s)\r\n❯ \r\n");
    check(claude_activity_line(thinking).find("Compacting") == std::string::npos,
          "conversation text mentioning compaction is not the spinner");
  }

  // Messages sent while Claude works: held in its queue until it takes them,
  // and shown as the user's once the model reads them mid-turn.
  {
    Arena arena;
    std::vector<Event> ev;
    const auto parse = [&](const char* line) { claude_adapter().parse(line, arena, ev); };
    parse(R"({"parentUuid":"p","attachment":{"type":"queued_command","prompt":"also check the tests","origin":"user"},"type":"attachment"})");
    parse(R"({"type":"attachment","attachment":{"type":"queued_command","prompt":"type first works too"}})");
    parse(R"({"parentUuid":"p","attachment":{"type":"queued_command","prompt":"<task-notification>\n<task-id>x</task-id>"},"type":"attachment"})");
    parse(R"({"parentUuid":"p","attachment":{"type":"total_tokens_reminder","text":"x"},"type":"attachment"})");
    check(ev.size() == 2 && ev[0].kind == EventKind::User && arena.view(ev[0].text) == "also check the tests" &&
              ev[1].kind == EventKind::User && arena.view(ev[1].text) == "type first works too",
          "a steered message is the user's; task notices and other attachments are not");
    ev.clear();
    parse(R"({"type":"queue-operation","operation":"enqueue","timestamp":"t","sessionId":"s","content":"first"})");
    parse(R"({"type":"queue-operation","operation":"enqueue","timestamp":"t","sessionId":"s","content":"second"})");
    parse(R"({"type":"queue-operation","operation":"enqueue","timestamp":"t","sessionId":"s","content":"<task-notification>x"})");
    parse(R"({"type":"queue-operation","operation":"remove","timestamp":"t","sessionId":"s","content":"second","reason":"absorbed_mid_turn"})");
    check(ev.size() == 3 && ev[0].kind == EventKind::QueueAdd && ev[1].kind == EventKind::QueueAdd &&
              ev[2].kind == EventKind::QueueTake && arena.view(ev[2].text) == "second",
          "claude queue operations are read, task notices skipped");

    char path[] = "/tmp/mico_queue_XXXXXX";
    const int fd = mkstemp(path);
    if (fd >= 0) {
      const std::string body =
          R"({"type":"user","message":{"role":"user","content":"go"}})" "\n"
          R"({"type":"queue-operation","operation":"enqueue","timestamp":"t","sessionId":"s","content":"first"})" "\n"
          R"({"type":"queue-operation","operation":"enqueue","timestamp":"t","sessionId":"s","content":"second"})" "\n"
          R"({"type":"queue-operation","operation":"remove","timestamp":"t","sessionId":"s","content":"second","reason":"absorbed_mid_turn"})" "\n";
      if (write(fd, body.data(), body.size()) != ssize_t(body.size())) check(false, "write queue fixture");
      close(fd);
      ChatRenderer chat;
      chat.open(path, &claude_adapter());
      Surface sf; sf.resize(60, 10);
      Painter painter(sf, {0, 0, 60, 10});
      chat.render(painter, Theme{}, Filters{});
      const auto held = chat.agent_queue();
      check(held.size() == 1 && held[0] == "first", "the renderer lists what the agent still holds");
      std::string shown;
      for (int y = 0; y < 10; y++) for (int x = 0; x < 60; x++) text::encode(sf.at(x, y).cp, shown);
      check(shown.find("first") == std::string::npos, "held messages are not drawn as chat rows");
      FILE* f = fopen(path, "a");
      if (f) {
        fputs(R"({"type":"queue-operation","operation":"dequeue","timestamp":"t","sessionId":"s"})" "\n", f);
        fclose(f);
      }
      chat.poll_growth();
      check(chat.agent_queue().empty(), "a dequeue with no text takes the oldest");
      unlink(path);
    }
  }

  // Claude's idle input box is a prompt, not a menu waiting for a pick.
  {
    const std::string rule = "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80";
    Vt idle; idle.resize(80, 12);
    idle.write("Claude Code\r\n" + rule + "\r\n\xE2\x9D\xAF \r\n" + rule + "\r\n  auto mode on\r\n");
    check(!screen_awaits_input(idle), "an idle Claude input box does not read as a menu");
    Vt menu; menu.resize(80, 12);
    menu.write("Select model\r\n  1. Default\r\n\xE2\x9D\xAF 2. Opus\r\n  3. Sonnet\r\n");
    check(screen_awaits_input(menu), "a selection cursor on an option still reads as a menu");
    Vt asks; asks.resize(80, 12);
    asks.write("\xE2\x97\x8F Done. Do you want to deploy it too? (y/n)\r\n\r\n" + rule + "\r\n\xE2\x9D\xAF \r\n" + rule +
               "\r\n  auto mode on\r\n");
    check(!screen_awaits_input(asks), "Claude's reply asking a question above its input box is not a dialog");
  }

  // Codex, as 0.159 draws it: its dialogs replace the input box, and its
  // status line sits just above the box while it works.
  {
    const std::string box = "\xE2\x80\xBA Ask Codex to do anything\r\n\r\n  gpt high \xC2\xB7 ~/p\r\n  ? for shortcuts\r\n";
    const std::string bullet = "\x1b[1m\xE2\x80\xA2\x1b[0m ";
    Vt approval; approval.resize(100, 20);
    approval.write("  Reason: Do you want to allow creating probe.txt?\r\n\r\n  $ touch probe.txt\r\n\r\n"
                   "\xE2\x80\xBA 1. Yes, proceed (y)\r\n  2. Yes, and don't ask again (p)\r\n"
                   "  3. No, and tell Codex what to do differently (esc)\r\n\r\n  Press enter to confirm or esc to cancel\r\n");
    check(screen_awaits_codex_input(approval), "codex: an approval dialog needs you");
    PermissionPrompt cp;
    check(!parse_codex_permission_prompt(approval, cp), "codex: no question heading, no permission panel");
    Vt asked; asked.resize(110, 24);
    asked.write("\xE2\x80\xBA touch probe.txt\r\n\r\n\xE2\x80\xA2 Running touch probe.txt\r\n\r\n"
                "  Would you like to run the following command?\r\n\r\n  Environment: local\r\n\r\n"
                "  Reason: Do you want to allow creating probe.txt?\r\n\r\n  $ touch probe.txt\r\n\r\n\r\n"
                "\xE2\x80\xBA 1. Yes, proceed (y)\r\n"
                "  2. Yes, and don't ask again for commands that start with `touch probe.txt` (p)\r\n"
                "  3. No, and tell Codex what to do differently (esc)\r\n\r\n"
                "  Press enter to confirm or esc to cancel\r\n");
    check(parse_codex_permission_prompt(asked, cp) &&
              cp.question == "Would you like to run the following command?" && cp.title.size() == 2 &&
              cp.title[0] == "Reason: Do you want to allow creating probe.txt?" && cp.title[1] == "$ touch probe.txt",
          "codex: the approval's question and what it is about are read");
    check(cp.options.size() == 3 && cp.options[0] == "Yes, proceed" &&
              cp.options[2] == "No, and tell Codex what to do differently" && cp.cursor == 0 && !cp.amend,
          "codex: choices are read without their keys");
    check(!parse_permission_prompt(asked, cp), "codex's approval is not read as claude's");

    // The plan's end, as Codex draws it: label and description per choice, a
    // long description wrapped under its column, a disabled choice marked.
    const std::string plan_foot = "\r\n  enter select \xC2\xB7 esc back\r\n";
    Vt plan; plan.resize(100, 20);
    plan.write("\xE2\x80\xA2 Here is the plan.\r\n\r\n  Implement this plan?\r\n"
               "\xE2\x80\xBA 1. Yes, implement this plan          Switch to Default and start coding\r\n"
               "  2. Yes, clear context and implement  Start a fresh thread (current context:\r\n"
               "                                       89% used)\r\n"
               "  3. No, stay in Plan mode             Continue planning with the model\r\n" + plan_foot);
    check(parse_codex_permission_prompt(plan, cp) && cp.plan && cp.question == "Implement this plan?" &&
              cp.title.empty() && cp.options.size() == 3 && cp.options[1] == "Yes, clear context and implement" &&
              cp.details[1] == "Start a fresh thread (current context: 89% used)" &&
              cp.options[2] == "No, stay in Plan mode" && cp.cursor == 0,
          "codex: the plan's choices are read with their descriptions");
    Vt off; off.resize(100, 20);
    off.write("  Implement this plan?\r\n"
              "  1. Yes, implement this plan          Switch to Default and start coding\r\n"
              "  2. Yes, clear context and implement (disabled)  Fresh thread with this plan (disabled: no plan)\r\n"
              "\xE2\x80\xBA 3. No, stay in Plan mode             Continue planning with the model\r\n"
              "\r\n  Press enter to confirm or esc to go back\r\n");
    check(parse_codex_permission_prompt(off, cp) && cp.cursor == 2 && cp.disabled.size() == 3 && cp.disabled[1] &&
              !cp.disabled[0] && cp.options[1] == "Yes, clear context and implement",
          "codex: a disabled plan choice is read as one, and the cursor where it is");

    Vt trust; trust.resize(100, 20);
    trust.write("  Trust this folder? Codex can read, edit, and run files here.\r\n\r\n"
                "\xE2\x80\xBA 1. Trust and continue\r\n  2. Quit\r\n\r\n  enter continue \xC2\xB7 esc quit\r\n");
    check(screen_awaits_codex_input(trust), "codex: the folder trust question needs you");
    check(!parse_codex_permission_prompt(trust, cp), "codex: the folder trust question is not an approval");
    Vt reply; reply.resize(100, 20);
    reply.write("\xE2\x80\xBA 1. fix the parser\r\n\r\n\xE2\x80\xA2 Fixed. Do you want to proceed with the release? (y/n)\r\n\r\n" + box);
    check(!screen_awaits_codex_input(reply) && !screen_shows_codex_activity(reply),
          "codex: at its input box, under a reply that asks, it is idle");
    Vt working; working.resize(100, 20);
    working.write("\xE2\x80\xA2 Looking at the parser\r\n\r\n" + bullet + "Working (12s \xE2\x80\xA2 esc to interrupt)\r\n\r\n" +
                  "  \xE2\x86\xB3 queued: and the tests\r\n\r\n" + box);
    check(screen_shows_codex_activity(working) && !screen_awaits_codex_input(working),
          "codex: its status line over the input box is work");
    Vt quoted; quoted.resize(100, 30);
    quoted.write("\xE2\x80\xA2 Codex shows Working (12s \xE2\x80\xA2 esc to interrupt) while busy.\r\n");
    for (int i = 0; i < 8; ++i) quoted.write("  more of the reply\r\n");
    quoted.write("\r\n" + box);
    check(!screen_shows_codex_activity(quoted), "codex: the words quoted higher up in a reply are not work");
  }

  // Work state follows Claude's visible footer, not general PTY traffic.
  {
    for (const char* footer : {"✻ Thinking… (8s)", "✢ Reading… (esc to interrupt)",
                               "· Noodling…", "* Working..."}) {
      Vt vt; vt.resize(80, 20);
      vt.write(std::string("● Previous reply\r\n") + footer + "\r\n❯ draft\r\n");
      check(screen_shows_claude_activity(vt), "Claude spinner frames indicate active work");
    }
    for (const char* footer : {"❯ draft", "✻ Worked for 8s", "1 startup issue · ctrl+t for details",
                               "The phrase esc to interrupt is documented here.", "Background task completed"}) {
      Vt vt; vt.resize(80, 20); vt.write(footer);
      check(!screen_shows_claude_activity(vt), "idle text and completion notices do not indicate work");
    }
    Vt vt; vt.resize(80, 5);
    vt.write("✻ Thinking… (8s)\r\n");
    for (int i = 0; i < 12; ++i) vt.write("ordinary output\r\n");
    check(!screen_shows_claude_activity(vt), "old spinner lines in scrollback cannot restart Thinking");
  }

  // A private-marker CSI is a different command: claude asks "CSI ? u" (the
  // kitty keyboard flags) right after its first frame. Run as a plain cursor
  // restore it moved the cursor to the top-left, so every relative redraw
  // after it landed on the wrong rows.
  {
    Vt v;
    v.resize(20, 6);
    v.write("row0\r\nrow1\r\nrow2\r\n\x1b[1C\x1b[2A");  // cursor on row 1, col 1
    v.write("\x1b[>0q\x1b[?u\x1b[?s\x1b[c");
    v.write("X");
    std::string r1;
    for (const Cell& c : v.row(1)) if (c.width) text::encode(c.cp ? c.cp : U' ', r1);
    check(r1.starts_with("rXw1"), "private CSI queries leave the cursor where it was");
  }

  // Claude's permission dialog, read off the screen: heading, question,
  // numbered choices (a wrapped one joined back, a path wrapped at its '-'
  // without a space), claude's cursor, and whether "Tab to amend" is on.
  {
    std::string top;
    for (int i = 0; i < 40; i++) top += "\xE2\x94\x80";  // ─
    Vt v;
    v.resize(60, 20);
    v.write("\xE2\x97\x8F Bash(touch x)\r\n" + top + "\r\n"
            " Bash command\r\n   touch x\r\n   Create x\r\n\r\n"
            " Do you want to proceed?\r\n"
            " \xE2\x9D\xAF 1. Yes\r\n"
            "   2. Yes, and always allow access to /tmp/some-long-\r\n"
            "      dir from this project\r\n"
            "   3. No\r\n\r\n"
            " Esc to cancel \xC2\xB7 Tab to amend\r\n");
    PermissionPrompt pp;
    check(parse_permission_prompt(v, pp), "a Bash permission dialog is recognised");
    check(pp.title.size() == 3 && pp.title[0] == "Bash command" && pp.title[1] == "touch x",
          "the dialog heading is read");
    check(pp.question == "Do you want to proceed?", "the permission question is read");
    check(pp.options.size() == 3 && pp.options[0] == "Yes" && pp.options[2] == "No" &&
              pp.options[1] == "Yes, and always allow access to /tmp/some-long-dir from this project",
          "choices are read, a wrapped one joined back");
    check(pp.cursor == 0 && pp.amend, "claude's cursor and Tab to amend are read");

    Vt f;
    f.resize(60, 24);
    f.write(top + "\r\n Create file\r\n notes.txt\r\n"
            "\xE2\x95\x8C\xE2\x95\x8C\xE2\x95\x8C\r\n  1 alpha\r\n  2 beta\r\n\xE2\x95\x8C\xE2\x95\x8C\xE2\x95\x8C\r\n"
            " Do you want to create notes.txt?\r\n"
            "   1. Yes\r\n   2. Yes, and switch to accept edits\r\n"
            " \xE2\x9D\xAF 3. No, and tell Claude what to do differently\r\n"
            " Esc to cancel\r\n");
    check(parse_permission_prompt(f, pp) && pp.title.size() == 2 && pp.title[1] == "notes.txt" &&
              pp.cursor == 2 && !pp.amend,
          "a file dialog's heading stops at its contents; the cursor may be on No");

    Vt m;
    m.resize(60, 12);
    m.write(" Select model\r\n \xE2\x9D\xAF 1. Default\r\n   2. Opus\r\n Enter to confirm \xC2\xB7 Esc to cancel\r\n");
    check(!parse_permission_prompt(m, pp), "another numbered menu is not a permission dialog");
    Vt idle;
    idle.resize(60, 8);
    idle.write("\xE2\x97\x8F Done.\r\n" + top + "\r\n\xE2\x9D\xAF \r\n" + top + "\r\n");
    check(!parse_permission_prompt(idle, pp), "an idle prompt is not a permission dialog");
  }

  // The first-run trust dialog is recognised so mico can answer it for a
  // folder the user explicitly tracked; a normal reply is not.
  {
    Vt yes, no;
    yes.resize(80, 20);
    yes.write("banner\r\n\r\nQuick safety check: Is this a project you created or one you "
              "trust?\r\n\xE2\x9D\xAF No, exit\r\n  Yes, I trust this folder\r\n"
              "  Enter to confirm  Esc to cancel\r\n");
    no.resize(80, 20);
    no.write("Hi Amedeo!\r\n\r\nWhat would you like to work on?\r\n");
    check(screen_is_trust_prompt(yes), "the trust dialog is recognised");
    check(!screen_is_trust_prompt(no), "an ordinary reply is not a trust dialog");
    {
      // A resumed chat reprints its messages; one quoting the dialog, with
      // the input box under it, is not the dialog.
      Vt quoted;
      quoted.resize(80, 20);
      quoted.write("\xE2\x97\x8F The log said: Quick safety check ... Yes, I trust this folder\r\n"
                   "  Enter to confirm \xC2\xB7 Esc to cancel\r\n\r\n"
                   "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\r\n\xE2\x9D\xAF \r\n\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\r\n"
                   "  ? for shortcuts\r\n");
      check(!screen_is_trust_prompt(quoted), "the dialog quoted in a resumed chat's messages is not the dialog");
    }
    check(screen_awaits_input(yes), "the trust dialog is awaiting input");
    check(!screen_awaits_input(no), "an ordinary reply is not awaiting input");
    // The cursor starts on "No, exit": a bare Enter would quit claude.
    check(trust_prompt_moves(yes) == 1, "the trust answer walks down to Yes before Enter");
    Vt first;
    first.resize(80, 20);
    first.write("Quick safety check: Is this a project you created or one you trust?\r\n"
                "\xE2\x9D\xAF Yes, I trust this folder\r\n  No, exit\r\n"
                "  Enter to confirm  Esc to cancel\r\n");
    check(trust_prompt_moves(first) == 0, "a trust dialog already on Yes needs no move");
    check(trust_prompt_moves(no) == kNoTrustMove, "no trust dialog, no move");
  }

  // Live tailing re-maps a growing file and extends the index over the new
  // bytes only. A partial last line is not a line: exposing one lets a reader
  // parse a half-written record and show a truncated message.
  {
    char tmpl[] = "/tmp/mico_tail_XXXXXX";
    const int fd = mkstemp(tmpl);
    if (fd >= 0) {
      close(fd);
      std::vector<std::string> want;
      auto append = [&](const std::string& text, bool newline) {
        FILE* f = fopen(tmpl, "ab");
        if (!f) return;
        fwrite(text.data(), 1, text.size(), f);
        if (newline) fputc('\n', f);
        fclose(f);
      };
      auto matches = [&](Jsonl& j) {
        while (!j.complete()) j.extend_back();
        if (j.line_count() != want.size()) return false;
        for (size_t i = 0; i < want.size(); i++)
          if (j.line(i) != want[i]) return false;
        return true;
      };

      for (int i = 0; i < 5; i++) {
        append("line " + std::to_string(i), true);
        want.push_back("line " + std::to_string(i));
      }
      Jsonl j;
      check(j.open(tmpl), "a growing file opens");
      check(matches(j), "the initial index matches");

      for (int i = 5; i < 9; i++) {
        append("appended " + std::to_string(i), true);
        want.push_back("appended " + std::to_string(i));
      }
      check(j.refresh() && matches(j), "the index extends over appended lines");

      append("half", false);  // a line the writer has not finished
      j.refresh();
      check(matches(j), "a partial trailing line is not counted");

      append("-done", true);
      want.push_back("half-done");
      check(j.refresh() && matches(j), "the partial line appears once it is complete");

      unlink(tmpl);
    }
  }

  // Input arrives in whatever chunks a read produced, so the events decoded
  // must not depend on where the boundaries fall. A multi-byte character split
  // across a read used to come out as three replacement characters.
  {
    std::string stream;
    stream += "hello";
    stream += "\x1b[A\x1b[B\x1bOC\x1bOD\x1b[1;5A";
    stream += "\x1b[5~\x1b[6~\x1b[3~\x1b[15~\x1b[24~";
    stream += "\x1b[<0;12;34M\x1b[<0;12;34m\x1b[<64;5;6M\x1b[<32;9;9M";
    stream += "\x1b[200~pasted text\x1b[201~";
    stream += "\x01\x1bz";
    stream += "\xE4\xB8\xAD\xC3\xA9\xF0\x9F\x98\x80";  // 3-, 2- and 4-byte characters
    stream += "\x1b";                                    // a trailing bare Escape

    auto decode_split = [&](size_t chunk) {
      InputDecoder d;
      std::vector<std::string> out;
      for (size_t i = 0; i < stream.size(); i += chunk) {
        d.feed(std::string_view(stream).substr(i, chunk));
        while (auto e = d.next()) {
          char b[96];
          if (e->type == InputEvent::Type::Key)
            snprintf(b, sizeof b, "K%d/%u/%d%d", int(e->key.key), unsigned(e->key.ch),
                     e->key.ctrl, e->key.alt);
          else if (e->type == InputEvent::Type::Mouse)
            snprintf(b, sizeof b, "M%d/%d/%d,%d", int(e->mouse.kind), int(e->mouse.button),
                     e->mouse.pos.x, e->mouse.pos.y);
          else
            snprintf(b, sizeof b, "P:%s", e->paste.c_str());
          out.push_back(b);
        }
      }
      if (auto e = d.flush()) out.push_back("ESC");
      return out;
    };

    const auto whole = decode_split(stream.size());
    check(whole.size() > 20, "the decoder corpus produces events at all");
    bool same = true;
    for (size_t chunk : {size_t(1), size_t(2), size_t(3), size_t(5), size_t(7), size_t(13)})
      if (decode_split(chunk) != whole) same = false;
    check(same, "decoding does not depend on where reads are split");
  }

  // A bare Escape must resolve, and must not corrupt whatever follows it.
  {
    InputDecoder d;
    d.feed("\x1b");
    check(!d.next(), "lone ESC waits for more bytes");
    check(d.pending_escape(), "lone ESC is reported as pending");
    auto e = d.flush();
    check(e && e->key.key == Key::Escape, "flush resolves a lone ESC");

    d.feed("\x1b");
    d.feed("\x1b[18~");
    bool esc = false, f7 = false;
    int n = 0;
    while (auto x = d.next()) {
      n++;
      if (x->key.key == Key::Escape) esc = true;
      if (x->key.key == Key::F7) f7 = true;
    }
    check(n == 2 && esc && f7, "a buffered ESC does not swallow the next key");

    d.feed("\x1bOQ");
    auto alt = d.next();
    check(alt && alt->key.key == Key::F2, "F2 still decodes normally");
  }

  // Backspace is DEL and Ctrl+Backspace is BS; ESC-prefixed, either is
  // Alt+Backspace. CSI u reports what the legacy bytes cannot tell apart.
  {
    InputDecoder d;
    const auto one = [&](const char* bytes) {
      d.feed(bytes);
      auto e = d.next();
      return e ? e->key : KeyEvent{};
    };
    KeyEvent k = one("\x7f");
    check(k.key == Key::Backspace && !k.ctrl && !k.alt, "DEL is Backspace");
    k = one("\x08");
    check(k.key == Key::Backspace && k.ctrl, "BS is Ctrl+Backspace");
    k = one("\x1b\x7f");
    check(k.key == Key::Backspace && k.alt && !k.ctrl, "ESC DEL is Alt+Backspace");
    k = one("\x1b[13;2u");
    check(k.key == Key::Enter && k.shift, "CSI u Shift+Enter");
    k = one("\x1b[122;6u");
    check(k.key == Key::Char && k.ch == 'z' && k.ctrl && k.shift, "CSI u Ctrl+Shift+Z");
    k = one("\x1b[127;5u");
    check(k.key == Key::Backspace && k.ctrl, "CSI u Ctrl+Backspace");
    check_str(encode_key(KeyEvent{Key::Backspace, 0, true}, false), "\x08",
              "Ctrl+Backspace reaches the agent as BS");
  }

  // Ctrl+digit has no byte of its own: it arrives only as kitty's CSI u or
  // xterm's modifyOtherKeys, and both must read as the same key.
  {
    InputDecoder d;
    d.feed("\x1b[49;5u\x1b[27;5;50~\x1b[51;3u");
    auto one = d.next(), two = d.next(), three = d.next();
    check(one && one->key.key == Key::Char && one->key.ch == '1' && one->key.ctrl && !one->key.alt,
          "CSI 49;5u is Ctrl+1");
    check(two && two->key.ch == '2' && two->key.ctrl, "CSI 27;5;50~ (modifyOtherKeys) is Ctrl+2");
    check(three && three->key.ch == '3' && three->key.alt && !three->key.ctrl, "CSI 51;3u is Alt+3");
  }

  // Enter is CR in a raw terminal and Ctrl+J is LF. Collapsing them into a
  // single Enter made the multi-line prompt unreachable.
  {
    InputDecoder d;
    d.feed("\r\n");
    auto enter = d.next();
    auto ctrl_j = d.next();
    check(enter && enter->key.key == Key::Enter, "CR decodes as Enter");
    check(ctrl_j && ctrl_j->key.key == Key::Char && ctrl_j->key.ch == 'j' &&
              ctrl_j->key.ctrl,
          "LF decodes as Ctrl+J, not Enter");
  }

  // Mouse selection reads text off a composed surface. The two ends can be in
  // any order and are clamped to the pane the drag began in.
  {
    Surface sf;
    sf.resize(12, 3);
    sf.clear(Style{});
    Painter p(sf, Rect{0, 0, 12, 3});
    p.text(0, 0, "hello world", Style{});
    p.text(0, 1, "second line", Style{});
    p.text(0, 2, "third", Style{});

    const Rect area{0, 0, 12, 3};
    Point top{}, bot{};
    normalize_selection(Point{6, 0}, Point{3, 1}, area, top, bot);
    check_str(selection_text(sf, top, bot, area), "world\nseco",
              "a selection reads in reading order, trailing blanks trimmed");

    normalize_selection(Point{3, 1}, Point{6, 0}, area, top, bot);
    check_str(selection_text(sf, top, bot, area), "world\nseco",
              "a reversed drag selects the same text");

    // Clamped to the pane: an end dragged past the right edge stops at it.
    normalize_selection(Point{99, 0}, Point{0, 0}, area, top, bot);
    check_str(selection_text(sf, top, bot, area), "hello world",
              "selection ends clamp to the pane's interior");
  }

  // Clipping is single-pass for speed, so it is checked against the obvious
  // two-pass version over random text. Wide glyphs are the whole point: an
  // ellipsis dropped onto the trailing half of one erases it and overruns the
  // width that was asked for.
  {
    std::mt19937 rng(20260910);
    static const char* pool[] = {"a",  "bb", " ",  "\xE4\xB8\xAD", "\xE6\x96\x87",
                                 "\xE2\x96\xBE", "\xC3\xA9", "z", "\xF0\x9F\x98\x80", "-"};
    int bad = 0;
    for (int trial = 0; trial < 3000; trial++) {
      std::string src;
      const int n = int(rng() % 14);
      for (int k = 0; k < n; k++) src += pool[rng() % (sizeof pool / sizeof *pool)];
      const int W = 1 + int(rng() % 16);
      const int limit = 1 + int(rng() % 16);
      const int x = int(rng() % W);

      auto draw = [&](bool single) {
        Surface sf;
        sf.resize(W, 1);
        sf.clear(Style{});
        Painter p(sf, Rect{0, 0, W, 1});
        const int used = single ? p.text_clipped(x, 0, src, Style{}, limit)
                                : p.text(x, 0, text::ellipsize(src, std::min(limit, W - x)), Style{});
        std::string out;
        for (int c = 0; c < W; c++) {
          const Cell& cell = sf.at(c, 0);
          if (cell.width == 0) continue;
          text::encode(cell.cp ? cell.cp : U' ', out);
        }
        while (!out.empty() && out.back() == ' ') out.pop_back();
        return std::pair<std::string, int>{out, used};
      };

      if (draw(true) != draw(false)) bad++;
    }
    check(bad == 0, "single-pass clipping matches the two-pass reference");
    if (bad) printf("        %d of 3000 random cases differ\n", bad);
  }

  // OSC 52 is how a copy crosses an ssh connection; a wrong pad byte means a
  // silently truncated clipboard.
  check_str(clipboard_seq("hi"), "\x1b]52;c;aGk=\x07", "clipboard base64, 2 bytes");
  check_str(clipboard_seq("a"), "\x1b]52;c;YQ==\x07", "clipboard base64, 1 byte");
  check_str(clipboard_seq("abc"), "\x1b]52;c;YWJj\x07", "clipboard base64, 3 bytes");
  check_str(clipboard_seq("hello world"), "\x1b]52;c;aGVsbG8gd29ybGQ=\x07",
            "clipboard base64, mixed length");
  check(mouse_mode_seq(false).find("1002l") != std::string::npos, "mouse release sequence");
  check(mouse_mode_seq(true).find("1002h") != std::string::npos, "mouse capture sequence");
  // Mode 1002 reports motion only while a button is held, so a popup menu
  // switches to 1003 (any-event) to let a plain hover move the highlight.
  check(mouse_mode_seq(true, true).find("1003h") != std::string::npos,
        "any-event mouse sequence for an open menu");
  check(mouse_mode_seq(true, false).find("1002h") != std::string::npos,
        "ordinary mouse sequence outside a menu");
  check(mouse_mode_seq(false, true).find("1003l") != std::string::npos &&
            mouse_mode_seq(false, true).find("1002l") != std::string::npos,
        "turning the mouse off clears both tracking modes");

  // Notifications. Each terminal family gets the escape it shows, and text
  // from a chat title can neither end the sequence nor add a field.
  {
    check_str(notify_seq(NotifyEscape::Osc777, "Fix; tabs\x1b", "Claude finished"),
              "\x1b]777;notify;Fix, tabs ;Claude finished\x1b\\", "OSC 777 notification, cleaned");
    check_str(notify_seq(NotifyEscape::Osc9, "1;2", "Claude needs you"),
              "\x1b]9;Claude needs you: 1;2\x07", "OSC 9 starts with mico's words");
    check_str(notify_seq(NotifyEscape::Bell, "t", "b"), "\x07", "a bell where nothing else works");
    const std::string k = notify_seq(NotifyEscape::Osc99, "Fix\x07", "Claude finished");
    check(k.starts_with("\x1b]99;i=mico") && k.find(":d=0;Fix \x1b\\") != std::string::npos &&
              k.find(":p=body;Claude finished\x1b\\") != std::string::npos,
          "kitty notification: title, then body under one id");
    check(notify_seq(NotifyEscape::Osc99, "a", "b").substr(0, 14) != k.substr(0, 14),
          "each kitty notification has its own id");

    check(tty::notify_escape("kitty(0.32.2)") == NotifyEscape::Osc99, "kitty by XTVERSION");
    check(tty::notify_escape("WezTerm 20240203") == NotifyEscape::Osc777, "WezTerm by XTVERSION");
    check(tty::notify_escape("ghostty 1.1.0") == NotifyEscape::Osc777, "Ghostty by XTVERSION");
    check(tty::notify_escape("foot(1.16.2)") == NotifyEscape::Osc777, "foot by XTVERSION");
    check(tty::notify_escape("iTerm2 3.5.0") == NotifyEscape::Osc9, "iTerm2 by XTVERSION");
    check(tty::notify_escape("XTerm(390)") == NotifyEscape::Bell, "an unknown terminal gets the bell");

    // The escape travels in Hello's flags, beside what it says about images.
    GfxCaps sent;
    sent.kitty = true;
    sent.tmux = true;
    sent.cell_w = 9;
    sent.cell_h = 18;
    sent.notify = NotifyEscape::Osc99;
    std::string wire, payload;
    proto::encode_size(proto::Type::Hello, 80, 24, sent, wire);
    proto::Type t{};
    GfxCaps got;
    check(proto::decode(wire, &t, &payload) && proto::decode_caps(payload, &got) && got == sent,
          "Hello carries the notification escape");

    InputDecoder d;
    d.feed("\x1b[O\x1b[I");
    auto out = d.next();
    auto in = d.next();
    check(out && out->type == InputEvent::Type::Focus && !out->focus_in, "CSI O is focus lost");
    check(in && in->type == InputEvent::Type::Focus && in->focus_in, "CSI I is focus gained");

    App::Notice shown{"t", "b", true}, hidden{"t", "b", false};
    check(!App::announce(shown, true, true) && !App::announce(hidden, true, true),
          "a focused terminal is told nothing");
    check(App::announce(shown, true, false), "an unfocused terminal is told even of the chat it shows");
    check(App::announce(hidden, false, true) && !App::announce(shown, false, true),
          "without focus reports, only a chat not shown is announced");
  }

  // Claude's background work, from the lines it writes: what is running now.
  {
    BackgroundTasks t;
    const auto feed = [&](const std::string& line, uint64_t at = 0) { claude_adapter().read_background(line, at, t); };
    const std::string ts = "\"timestamp\":\"2026-10-01T09:34:00.000Z\"";
    feed("{\"type\":\"assistant\"," + ts + ",\"message\":{\"content\":[{\"type\":\"tool_use\",\"id\":\"m1\",\"name\":\"Monitor\","
         "\"input\":{\"description\":\"perf sweep\",\"timeout_ms\":300000,\"command\":\"tail -f x\"}}]}}", 100);
    check(t.running.empty(), "a monitor call alone starts nothing");
    feed("{\"type\":\"user\"," + ts + ",\"message\":{\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"m1\","
         "\"content\":\"Monitor started (task b4zoqx6hu)\"}]},\"toolUseResult\":{\"taskId\":\"b4zoqx6hu\",\"timeoutMs\":300000}}");
    check(t.running.size() == 1 && t.running[0].id == "b4zoqx6hu" && t.running[0].kind == "monitor" &&
              t.running[0].what == "perf sweep" && t.running[0].offset == 100 && t.running[0].expires_ms > 0,
          "a monitor runs once its result names the task");
    feed("{\"type\":\"assistant\"," + ts + ",\"message\":{\"content\":[{\"type\":\"tool_use\",\"id\":\"b1\",\"name\":\"Bash\","
         "\"input\":{\"command\":\"make -j8\"}},{\"type\":\"tool_use\",\"id\":\"b2\",\"name\":\"Bash\",\"input\":{\"command\":\"ls\"}}]}}");
    feed("{\"type\":\"user\"," + ts + ",\"message\":{\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"b1\","
         "\"content\":\"moved to the background (ID: blfsbvne0). Output is being written to: /tmp/claude-1000/-w/s/tasks/blfsbvne0.output. "
         "You will be notified\"}]},\"toolUseResult\":{\"stdout\":\"\",\"backgroundTaskId\":\"blfsbvne0\"}}");
    check(t.running.size() == 2 && t.running[1].output == "/tmp/claude-1000/-w/s/tasks/blfsbvne0.output",
          "a background command's output file, from its result");
    feed("{\"type\":\"user\"," + ts + ",\"message\":{\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"b2\","
         "\"content\":\"a.txt\"}]},\"toolUseResult\":{\"stdout\":\"a.txt\"}}");
    check(t.running.size() == 2 && t.running[1].kind == "shell" && t.running[1].what == "make -j8" && t.calls.empty(),
          "a command moved to the background runs; one that finished does not");
    check(t.running[0].output.empty(), "a monitor's result names no output file");
    const std::string event = "<task-notification>\\n<task-id>b4zoqx6hu</task-id>\\n<summary>Monitor event: &quot;perf sweep&quot;"
                              "</summary>\\n<event>skip_terrain: 5.66 ms</event>\\n</task-notification>";
    feed("{\"type\":\"queue-operation\",\"operation\":\"enqueue\"," + ts + ",\"content\":\"" + event + "\"}");
    feed("{\"type\":\"attachment\"," + ts + ",\"attachment\":{\"type\":\"queued_command\",\"prompt\":\"" + event + "\"}}");
    check(t.running[0].events == 1 && t.running[0].last_event == "skip_terrain: 5.66 ms",
          "a monitor's event, counted once though recorded twice");
    feed("{\"type\":\"user\"," + ts + ",\"message\":{\"role\":\"user\",\"content\":\"<task-notification>\\n<task-id>blfsbvne0</task-id>"
         "\\n<status>completed</status>\\n<summary>Background command done</summary>\\n</task-notification>\"}}");
    check(t.running.size() == 1 && t.running[0].id == "b4zoqx6hu", "a completed command is no longer running");
    feed("{\"type\":\"assistant\"," + ts + ",\"message\":{\"content\":[{\"type\":\"tool_use\",\"id\":\"s1\",\"name\":\"TaskStop\","
         "\"input\":{\"task_id\":\"b4zoqx6hu\"}}]}}");
    feed("{\"type\":\"user\"," + ts + ",\"message\":{\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"s1\",\"content\":\"stopped\"}]}}");
    check(t.running.empty(), "a monitor TaskStop stopped is no longer running");
    // Another monitor, ended by its expiry notice; a third by the clock.
    feed("{\"type\":\"assistant\"," + ts + ",\"message\":{\"content\":[{\"type\":\"tool_use\",\"id\":\"m2\",\"name\":\"Monitor\",\"input\":{}}]}}");
    feed("{\"type\":\"user\"," + ts + ",\"message\":{\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"m2\"}]},\"toolUseResult\":{\"taskId\":\"x2\"}}");
    feed("{\"type\":\"user\"," + ts + ",\"message\":{\"content\":\"<task-notification>\\n<task-id>x2</task-id>\\n<event>[Monitor expired "
         "after 5m with 0 events delivered.]</event>\\n</task-notification>\"}}");
    check(t.running.empty(), "a monitor's expiry ends it");
    feed("{\"type\":\"assistant\"," + ts + ",\"message\":{\"content\":[{\"type\":\"tool_use\",\"id\":\"m3\",\"name\":\"Monitor\",\"input\":{}}]}}");
    feed("{\"type\":\"user\"," + ts + ",\"message\":{\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"m3\"}]},\"toolUseResult\":{\"taskId\":\"x3\"}}");
    check(t.running.size() == 1 && t.running[0].expires_ms == t.running[0].started_ms + 300000,
          "a monitor that names no timeout gets Claude's five minutes");
    t.expire(t.running[0].expires_ms + 61000);
    check(t.running.empty(), "a monitor long past its time is let go");
    BackgroundTask pt;
    pt.fraction = 0.45;
    pt.done = 450;
    pt.total = 1000;
    pt.eta_s = 125;
    check(background_progress(pt, 0) == "45% \xC2\xB7 450/1000 \xC2\xB7 2m 05s left", "a task's progress, in words");
    check(background_progress(pt, 4).starts_with("\xE2\x96\x95") && background_percent({pt}) == " 45%",
          "with a bar, and as a percentage");
    pt.fraction = -1;
    check(background_progress(pt, 4).empty() && background_percent({pt}).empty(), "no progress printed, none shown");
    const auto task = [](const char* kind) {
      BackgroundTask b;
      b.kind = kind;
      return b;
    };
    check(background_summary({task("monitor"), task("monitor"), task("shell")}) ==
              "2 monitors \xC2\xB7 1 command",
          "what runs in the background, in words");
  }

  // Memory by process tree, from /proc: what names the agent behind a kill.
  {
    pid_t ppid = 0;
    std::string name;
    int64_t pages = 0;
    const std::string stat =
        "4242 (my (odd) prog) S 17 4242 4242 0 -1 4194560 100 0 0 0 5 1 0 0 20 0 1 0 12345 104857600 2560 "
        "18446744073709551615 1 1 0 0 0 0 0 0 0 0 0 0 17 3 0 0 0 0 0";
    check(proc::Table::parse_stat(stat, &ppid, &name, &pages) && ppid == 17 && name == "my (odd) prog" &&
              pages == 2560,
          "/proc stat: parent, a name with parentheses, resident pages");
    check(!proc::Table::parse_stat("4242 (cut", &ppid, &name, &pages), "a cut stat line is none");
    // A child holding 64 MB counts toward this process's tree.
    int ready[2];
    if (pipe(ready) == 0) {
      const pid_t child = fork();
      if (child == 0) {
        // Touched page by page through a volatile pointer: an optimiser may
        // drop a buffer that is filled and never read.
        std::vector<char> hold(64u << 20);
        volatile char* touch = hold.data();
        for (size_t i = 0; i < hold.size(); i += 4096) touch[i] = 1;
        if (write(ready[1], "x", 1) != 1) {}
        pause();
        _exit(0);
      }
      char c;
      if (read(ready[0], &c, 1) != 1) {}
      proc::Table t;
      t.read();
      const proc::Usage u = t.tree(getpid());
      check(u.procs >= 2 && u.rss >= (int64_t(64) << 20) && u.top_pid == child,
            "a process tree adds up its children, the largest named");
      check(t.tree(999999999).procs == 0, "a process that is gone holds nothing");
      kill(child, SIGKILL);
      waitpid(child, nullptr, 0);
      close(ready[0]);
      close(ready[1]);
    }
    const proc::Memory m = proc::system_memory();
    check(m.total > 0 && m.available > 0 && m.available <= m.total, "/proc/meminfo: total and available");
    check(proc::bytes(int64_t(3) << 29) == "1.5 GB" && proc::bytes(int64_t(412) << 20) == "412 MB", "sizes read as sizes");
  }

  // Commits, as agents' commands announce them.
  {
    check(makes_commits("git commit -m 'x'"), "git commit commits");
    check(makes_commits("cd sub && git -C repo -c user.name=a commit --amend"), "past git's own options");
    check(makes_commits("git add -A\ngit cherry-pick abc1234"), "a cherry-pick on a later line");
    check(!makes_commits("grep -rn 'git commit' ."), "only mentioning a commit is none");
    check(!makes_commits("git log --oneline"), "git log commits nothing");
    check(!makes_commits("echo git commit"), "echo is not git");
    const std::vector<ChatCommit> c = commits_announced(
        "[main 777c513] The view is remembered\n 2 files changed\n"
        "[feature/x (root-commit) 0a1b2c3d] First\r\n[detached HEAD 1234567] Loose\n"
        "  [main 89abcde] indented\n[main nothex1] no\n[two words 1234567] no");
    check(c.size() == 3, "three commits announced");
    if (c.size() == 3) {
      check(c[0].hash == "777c513" && c[0].branch == "main" && c[0].subject == "The view is remembered",
            "hash, branch and subject");
      check(c[1].branch == "feature/x" && c[1].hash == "0a1b2c3d" && c[1].subject == "First",
            "a root commit, its carriage return dropped");
      check(c[2].branch.empty() && c[2].hash == "1234567", "a detached HEAD has no branch");
    }
  }

  // Codex records a command twice, as a call with its output and as a
  // finished item with all it printed: one commit, read from either.
  {
    char path[] = "/tmp/mico_codex_commit_XXXXXX";
    const int fd = mkstemp(path);
    if (fd >= 0) {
      const std::string rollout =
          "{\"timestamp\":\"2026-09-10T18:55:43.000Z\",\"type\":\"response_item\",\"payload\":{\"type\":\"function_call\","
          "\"name\":\"shell\",\"call_id\":\"c1\",\"arguments\":\"{\\\"command\\\":[\\\"bash\\\",\\\"-lc\\\",\\\"git commit -m x\\\"]}\"}}\n"
          "{\"timestamp\":\"2026-09-10T18:55:44.000Z\",\"type\":\"response_item\",\"payload\":{\"type\":\"function_call_output\","
          "\"call_id\":\"c1\",\"output\":\"Exit code: 0\\nWall time: 0.1 seconds\\nOutput:\\n[main f574561] Port it\\n\"}}\n"
          "{\"timestamp\":\"2026-09-10T18:55:44.100Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"item_completed\","
          "\"item\":{\"type\":\"CommandExecution\",\"command\":[\"/bin/bash\",\"-lc\",\"git diff --cached --check\\ngit commit -m x\"],"
          "\"aggregated_output\":\"[main f574561] Port it\\n 1 file changed\\n\",\"exit_code\":0,\"status\":\"completed\"}}}\n"
          "{\"timestamp\":\"2026-09-10T18:56:00.000Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"item_completed\","
          "\"item\":{\"type\":\"CommandExecution\",\"command\":[\"/bin/bash\",\"-lc\",\"cat log.txt\"],"
          "\"aggregated_output\":\"[main 1234567] quoted in a log\\n\",\"exit_code\":0,\"status\":\"completed\"}}}\n";
      if (write(fd, rollout.data(), rollout.size()) != ssize_t(rollout.size())) {}
      close(fd);
      const ChatActivity a = ActivityIndex::read_file(path, "codex");
      check(a.commits.size() == 1 && a.commits[0].hash == "f574561" && a.commits[0].subject == "Port it",
            "a codex commit, once, and not one a log quotes");
      unlink(path);
    }
  }

  // git status and git show, as mico reads them.
  {
    // Written with '|' where git writes a NUL: a literal ends at its first.
    const auto nul = [](std::string t) {
      std::replace(t.begin(), t.end(), '|', '\0');
      return t;
    };
    GitStatus st;
    const std::string porcelain = nul(std::string("# branch.oid 777c513aaaa|# branch.head main|") +
                                  "# branch.upstream origin/main|# branch.ab +2 -1|" +
                                  "1 .M N... 100644 100644 100644 a a src/a.cpp|" +
                                  "1 M. N... 100644 100644 100644 a a src/b.cpp|" +
                                  "2 R. N... 100644 100644 100644 a a R100 new.cpp|old.cpp|" +
                                  "u UU N... 1 2 3 4 a b c conflict.cpp|? notes.txt|! build/|");
    check(parse_status(porcelain, st) && st.repo, "porcelain v2 reads");
    check(st.branch == "main" && st.head == "777c513" && st.upstream == "origin/main" && st.ahead == 2 &&
              st.behind == 1,
          "branch, head, upstream, ahead and behind");
    check(st.changed == 1 && st.staged == 2 && st.conflicts == 1 && st.untracked == 1 && st.files == 5,
          "each file counted once, a rename's old path not a file");
    check(st.entries.size() == 5 && st.entries[0].path == "src/a.cpp" && st.entries[0].x == '.' &&
              st.entries[0].y == 'M' && st.entries[2].path == "new.cpp" && st.entries[2].orig == "old.cpp" &&
              st.entries[3].x == 'U' && st.entries[3].path == "conflict.cpp" && st.entries[4].x == '?' &&
              st.entries[4].path == "notes.txt",
          "each file's letters and path, a rename's old path with it");

    const std::vector<GitWorktree> wts = parse_worktrees(
        "worktree /r\nHEAD 0123456789abcdef\nbranch refs/heads/main\n\n"
        "worktree /r-wt\nHEAD fedcba9876543210\ndetached\nlocked reason\n\nworktree /bare\nbare\n");
    check(wts.size() == 3 && wts[0].path == "/r" && wts[0].branch == "main" && wts[0].head == "0123456" &&
              wts[1].branch.empty() && wts[1].locked && wts[2].bare,
          "git worktree list: paths, branches, detached, locked, bare");
    const std::vector<GitBranch> brs = parse_branches(
        nul("old|1111111|1600000000|||Old work|\nnew|2222222|1700000000|origin/new|[ahead 2]|New work|\n"));
    check(brs.size() == 2 && brs[0].name == "new" && brs[0].upstream == "origin/new" && brs[0].track == "[ahead 2]" &&
              brs[1].subject == "Old work",
          "branches, newest first, with upstream and track");
    const std::vector<GitLogEntry> log =
        parse_log(nul("aaaa1111|Ada|1700000001|Second|HEAD -> main|\nbbbb2222|Bob|1700000000|First||\n"));
    check(log.size() == 2 && log[0].author == "Ada" && log[0].refs == "HEAD -> main" && log[1].subject == "First" &&
              log[1].refs.empty(),
          "git log: hash, author, time, subject, refs");
    const auto ns = parse_numstat(nul("3\t1\tsrc/a.cpp|-\t-\timg.png|2\t0\t|old.c|new.c|"));
    check(ns.size() == 3 && ns[0].first == "src/a.cpp" && ns[0].second.added == 3 && ns[1].second.added == -1 &&
              ns[2].first == "new.c" && ns[2].second.added == 2,
          "numstat: counts, binary files, a rename by its new path");

    GitStatus detached;
    check(parse_status(nul("# branch.oid (initial)|# branch.head (detached)|"), detached) &&
              detached.branch.empty() && detached.head.empty(),
          "detached, with no commit yet");

    GitCommit cm;
    const std::string show = nul(std::string("0123456789abcdef0123456789abcdef01234567|Ada|1700000000|Fix it|\n") +
                             "diff --git a/a.txt b/a.txt\nindex 1..2 100644\n--- a/a.txt\n+++ b/a.txt\n"
                             "@@ -1,2 +1,2 @@\n-old\n+new\n same\n"
                             "diff --git a/n.txt b/n.txt\nnew file mode 100644\n--- /dev/null\n+++ b/n.txt\n"
                             "@@ -0,0 +1 @@\n+hello\n"
                             "diff --git a/x.bin b/x.bin\nnew file mode 100644\nBinary files /dev/null and b/x.bin differ\n"
                             "diff --git a/old.c b/new.c\nsimilarity index 100%\nrename from old.c\nrename to new.c\n");
    check(parse_show(show, "/r", cm) && cm.found, "git show reads");
    check(cm.subject == "Fix it" && cm.author == "Ada" && cm.time == 1700000000 && cm.hash.size() == 40,
          "the commit's fields");
    check(cm.files.size() == 4 && cm.added == 2 && cm.removed == 1, "four files, their lines counted");
    if (cm.files.size() == 4) {
      check(cm.files[0].file == "/r/a.txt" && cm.files[0].lines.size() == 4, "an edit, with its hunk");
      check(cm.files[1].op == EditOp::Create && cm.files[1].file == "/r/n.txt", "a new file");
      check(cm.files[2].file == "/r/x.bin" && cm.files[2].lines.empty(), "a binary file, no lines");
      check(cm.files[3].file == "/r/old.c" && cm.files[3].moved_to == "/r/new.c", "a rename");
    }
    check(!parse_show("not a commit", "/r", cm) && !cm.found, "git's error is no commit");
  }

  // The agents a daemon is running, as the next one reads them back.
  {
    char dir[] = "/tmp/mico_running_XXXXXX";
    if (mkdtemp(dir)) {
      const char* old = getenv("XDG_STATE_HOME");
      const std::string keep = old ? old : "";
      setenv("XDG_STATE_HOME", dir, 1);
      const std::vector<RunningAgent> list = {{"claude", "abc", "/w/one"}, {"codex", "def", "/w/two dir"}};
      write_running(list);
      check(read_running() == list, "running agents read back as written");
      write_running({{"claude", "x", "/a\tb"}, {"pi", "y", "/w"}});
      check(read_running() == std::vector<RunningAgent>{{"pi", "y", "/w"}},
            "a folder that holds a tab is left out, not misread");
      write_running({});
      check(!fs::exists(running_path()) && read_running().empty(), "an empty list removes the file");
      rmdir((std::string(dir) + "/mico").c_str());
      rmdir(dir);
      if (old) setenv("XDG_STATE_HOME", keep.c_str(), 1);
      else unsetenv("XDG_STATE_HOME");
    }
  }
  {
    App app;
    check(!app.wants_motion(), "no menu open: ordinary button-event tracking");
    app.open_menu(nullptr, Point{2, 2}, {MenuItem{"one", "a"}, MenuItem{"two", "b"}});
    check(app.wants_motion(),
          "an open menu switches to any-event tracking so a plain hover follows the mouse");
    app.close_menu();
    check(!app.wants_motion(), "closing the menu drops any-event tracking");
  }

  // Usage extraction across the transcript formats. The four agents name the
  // same quantities differently and only two report money at all; the scanner
  // has to collapse them onto one shape without inventing a field.
  {
    auto with_file = [&](const char* tag, const std::string& body, const char* agent,
                         auto&& fn) {
      std::string tmpl = std::string("/tmp/mico_usage_") + tag + "_XXXXXX";
      std::vector<char> path(tmpl.begin(), tmpl.end());
      path.push_back('\0');
      const int fd = mkstemp(path.data());
      if (fd < 0) return;
      const ssize_t n = write(fd, body.data(), body.size());
      (void)n;
      close(fd);
      fn(path.data());
      unlink(path.data());
    };

    const std::string pi_body =
        R"({"type":"message","message":{"role":"user","content":"hi"}})" "\n"
        R"({"type":"message","message":{"role":"assistant","model":"m-a","content":[],"usage":{"input":100,"output":20,"cacheRead":400,"cacheWrite":0,"reasoning":5,"totalTokens":520,"cost":{"total":0.01}}}})" "\n"
        R"({"type":"message","message":{"role":"assistant","model":"m-a","content":[],"usage":{"input":50,"output":10,"cacheRead":0,"cacheWrite":0,"reasoning":2,"totalTokens":60,"cost":{"total":0.002}}}})" "\n";
    with_file("pi", pi_body, "pi", [&](const char* path) {
      UsageEntry e = usage_for_file(path, "pi", "", "p", 0);
      check(e.stat.input == 150 && e.stat.output == 30 && e.stat.cache_read == 400,
            "pi usage sums across assistant messages");
      check(e.stat.reasoning == 7 && e.stat.total == 580, "pi reasoning and total tracked");
      check(e.stat.has_cost && std::fabs(e.stat.cost_usd - 0.012) < 1e-9, "pi cost summed");
      check(e.models.size() == 1 && e.models[0].model == "m-a", "pi groups by model");
    });

    // claude writes one line per content block, each repeating the response's
    // usage: m1 appears twice and counts once. The cost-state covers only its
    // own process (10/20/30/40 tokens) and is a price sample, never the total.
    const std::string claude_head =
        R"({"type":"assistant","message":{"id":"m1","role":"assistant","model":"claude-x","usage":{"input_tokens":1,"output_tokens":2,"cache_read_input_tokens":3,"cache_creation_input_tokens":4,"output_tokens_details":{"thinking_tokens":1}}}})" "\n"
        R"({"type":"assistant","message":{"id":"m1","role":"assistant","model":"claude-x","usage":{"input_tokens":1,"output_tokens":2,"cache_read_input_tokens":3,"cache_creation_input_tokens":4,"output_tokens_details":{"thinking_tokens":1}}}})" "\n"
        R"({"type":"cost-state","totalCostUSD":1.5,"modelUsage":{"claude-x":{"inputTokens":10,"outputTokens":20,"thinkingTokens":5,"cacheReadInputTokens":30,"cacheCreationInputTokens":40,"costUSD":1.5}}})" "\n"
        R"({"type":"cost-state","totalCostUSD":1.5,"modelUsage":{"claude-x":{"inputTokens":10,"outputTokens":20,"thinkingTokens":5,"cacheReadInputTokens":30,"cacheCreationInputTokens":40,"costUSD":1.5}}})" "\n";
    const std::string claude_m2 =
        R"({"type":"assistant","message":{"id":"m2","role":"assistant","model":"claude-y","usage":{"input_tokens":100,"output_tokens":200,"cache_read_input_tokens":300,"cache_creation_input_tokens":400}}})";
    const std::string claude_synthetic =
        R"({"type":"assistant","message":{"id":"s","role":"assistant","model":"<synthetic>","usage":{"input_tokens":0,"output_tokens":0}}})" "\n";
    with_file("claude", claude_head + claude_synthetic + claude_m2 + "\n", "claude",
              [&](const char* path) {
      UsageEntry e = usage_for_file(path, "claude", "", "p", 0);
      check(e.stat.input == 101 && e.stat.output == 202 && e.stat.cache_read == 303 &&
                e.stat.cache_write == 404 && e.stat.reasoning == 1,
            "claude sums message usage once per message id, not the cost-state");
      check(e.models.size() == 2 && e.models[0].model == "claude-x" &&
                e.models[1].model == "claude-y" && e.models[1].stat.output == 200,
            "claude breaks usage out by model, skipping <synthetic>");
      check(e.price_samples.size() == 1 && e.price_samples[0].cost == 1.5 &&
                e.price_samples[0].cache_write == 40,
            "claude keeps each distinct cost-state as a price sample");
      check(!e.stat.has_cost, "claude dollars come from prices, not from the file");
    });

    // A running session: the second pass reads only what was appended, and a
    // line still being written (no newline yet) waits for the next pass.
    {
      std::string tmpl = "/tmp/mico_usage_grow_XXXXXX";
      std::vector<char> path(tmpl.begin(), tmpl.end());
      path.push_back('\0');
      const int fd = mkstemp(path.data());
      if (fd >= 0) {
        const auto put = [&](const std::string& b) {
          const ssize_t n = write(fd, b.data(), b.size());
          (void)n;
        };
        put(claude_head);
        put(claude_m2.substr(0, 40));
        UsageResume r;
        UsageEntry e1 = usage_for_file(path.data(), "claude", "", "p", 0, &r, nullptr);
        check(e1.stat.output == 2 && r.offset == claude_head.size(),
              "claude usage stops before a half-written line");
        put(claude_m2.substr(40) + "\n" + claude_head);
        UsageEntry e2 = usage_for_file(path.data(), "claude", "", "p", 0, &r, &e1);
        UsageEntry full = usage_for_file(path.data(), "claude", "", "p", 0);
        check(e2.stat.output == 202 && full.stat.output == 202 &&
                  e2.stat.cache_write == full.stat.cache_write &&
                  e2.price_samples.size() == 1,
              "claude usage resumes where it stopped and still dedupes");
        close(fd);
        unlink(path.data());
      }
    }

    // Prices solved from cost-state samples, as claude charges them: exactly
    // linear in the four token kinds.
    {
      const double p[4] = {5e-6, 25e-6, 0.5e-6, 10e-6};
      const auto sample = [&](const char* m, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
        return PriceSample{m, a, b, c, d, p[0] * a + p[1] * b + p[2] * c + p[3] * d};
      };
      PriceBook book;
      book.add(sample("opus", 2, 14, 27600, 6164));
      book.add(sample("opus", 40, 9000, 3000000, 90000));
      book.add(sample("opus", 700, 20000, 5000000, 400000));
      book.add(sample("opus", 1500, 90000, 60000000, 700000));
      book.add(PriceSample{"lone", 0, 1000, 0, 0, 0.01});  // one record: shape assumed
      book.fit();
      UsageStat s;
      s.input = 1000000;
      double c = 0;
      check(book.cost("opus", s, &c) && std::fabs(c - 5.0) < 1e-6,
            "prices solved from cost-state samples");
      s = {};
      s.cache_read = 1000000;
      check(book.cost("opus", s, &c) && std::fabs(c - 0.5) < 1e-6, "cache-read price solved");
      s = {};
      s.output = 1000;
      check(book.cost("lone", s, &c) && std::fabs(c - 0.01) < 1e-9,
            "a model with one record is priced by the usual shape");
      check(!book.cost("never-seen", s, &c), "a model never in a record has no price");

      UsageEntry e;
      e.agent = "claude";
      e.models.push_back(ModelUsage{"opus", {}});
      e.models[0].stat.output = 1000000;
      e.models.push_back(ModelUsage{"never-seen", {}});
      e.models[1].stat.output = 5;
      apply_prices(book, e);
      check(e.stat.has_cost && std::fabs(e.stat.cost_usd - 25.0) < 1e-6 &&
                e.models[0].stat.has_cost && !e.models[1].stat.has_cost,
            "claude sessions priced per model");
    }

    const std::string codex_body =
        R"({"type":"turn_context","payload":{"model":"gpt-5.6"}})" "\n"
        R"({"type":"event_msg","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":1000,"cached_input_tokens":800,"cache_write_input_tokens":0,"output_tokens":50,"reasoning_output_tokens":10,"total_tokens":1050}},"rate_limits":{"primary":{"used_percent":42.5,"resets_at":4102444800,"window_minutes":10080}}}})" "\n";
    with_file("codex", codex_body, "codex", [&](const char* path) {
      UsageEntry e = usage_for_file(path, "codex", "", "p", 0);
      check(e.stat.input == 200 && e.stat.cache_read == 800, "codex splits cached input out");
      check(e.stat.output == 50 && e.stat.total == 1050, "codex step usage read");
      check(e.has_quota && e.quota_used_pct == 42.5 && e.quota_resets_at == 4102444800,
            "codex account quota read");
      check(e.models.size() == 1 && e.models[0].model == "gpt-5.6",
            "codex model comes from the turn context");
    });

    // A resumed codex session starts its running total again from zero, and
    // codex repeats a record unchanged at times. Steps are summed: 100 + 50
    // (the repeat skipped) before the resume, 30 after it, under a new model.
    const auto tc = [](int in, int total_in, bool with_last, int total_out = 1) {
      std::string t = "{\"input_tokens\":" + std::to_string(total_in) + ",\"output_tokens\":" +
                      std::to_string(total_out) + ",\"total_tokens\":" +
                      std::to_string(total_in + total_out) + "}";
      std::string l = "{\"input_tokens\":" + std::to_string(in) +
                      ",\"output_tokens\":1,\"total_tokens\":" + std::to_string(in + 1) + "}";
      return std::string(R"({"type":"event_msg","payload":{"type":"token_count","info":{"total_token_usage":)") +
             t + (with_last ? ",\"last_token_usage\":" + l : std::string()) + "}}}\n";
    };
    const std::string resumed =
        R"({"type":"turn_context","payload":{"model":"a"}})" "\n" + tc(100, 100, true) +
        tc(50, 150, false, 2) + tc(50, 150, true, 2) +
        R"({"type":"turn_context","payload":{"model":"b"}})" "\n" + tc(30, 30, true);
    with_file("codex_resume", resumed, "codex", [&](const char* path) {
      UsageEntry e = usage_for_file(path, "codex", "", "p", 0);
      check(e.stat.input == 180 && e.stat.output == 3,
            "codex sums steps across a resume, skipping repeats");
      check(e.models.size() == 2 && e.models[0].stat.input == 150 &&
                e.models[1].model == "b" && e.models[1].stat.input == 30,
            "codex credits each step to its turn's model");
    });
  }

  // Renames and archive flags are mico's own, so they must round-trip through
  // the config dir without ever touching an agent's transcript.
  {
    char tmpl[] = "/tmp/mico_marks_XXXXXX";
    char* dir = mkdtemp(tmpl);
    if (dir) {
      setenv("XDG_CONFIG_HOME", dir, 1);
      Store a;
      a.scan();
      a.set_custom_name("claude", "abc", "My chat");
      a.set_archived("claude", "abc", true);
      a.set_custom_name("pi", "xyz", "Pi chat");
      check(a.custom_name("claude", "abc") && *a.custom_name("claude", "abc") == "My chat",
            "a custom name is kept in memory");
      check(a.archived("claude", "abc") && !a.archived("pi", "xyz"),
            "the archive flag is per chat");

      Store b;
      b.scan();
      check(b.custom_name("claude", "abc") && *b.custom_name("claude", "abc") == "My chat",
            "a custom name survives a reload");
      check(b.archived("claude", "abc"), "the archive flag survives a reload");
      check(b.custom_name("claude", "nope") == nullptr, "an unknown chat has no name");
      b.set_custom_name("claude", "abc", "");  // empty clears
      check(b.custom_name("claude", "abc") == nullptr, "clearing a name removes it");
      b.set_archived("claude", "abc", false);
      check(!b.archived("claude", "abc"), "unarchiving clears the flag");
      unsetenv("XDG_CONFIG_HOME");
    }
  }

  // Equations drawn as images: parser, layout, the cells they become, what
  // goes to the terminal, and how markdown lays them out.
  {
    using math::Node;
    const math::List q = math::parse(R"(x = \frac{-b \pm \sqrt{b^2-4ac}}{2a})");
    bool frac = false;
    for (const auto& n : q) frac |= n.k == Node::K::Frac;
    check(frac, "math: \\frac parses to a fraction");
    check(math::needs_drawing(q), "math: a fraction needs drawing");
    check(!math::needs_drawing(math::parse(R"(\alpha + x^2 \leq 3)")),
          "math: symbols and digit scripts stay Unicode");
    check(math::needs_drawing(math::parse(R"(x^{ab})")), "math: a letter script needs drawing");
    check(!math::needs_drawing(math::parse(R"(\sin^2 x)")), "math: \\sin^2 stays Unicode");
    check(math::needs_drawing(math::parse(R"(\lim_{n\to\infty} a_n)")), "math: \\lim with limits is drawn");
    const math::List arr = math::parse(R"(\begin{pmatrix} a & b \\ c & d \end{pmatrix})");
    check(arr.size() == 1 && arr[0].k == Node::K::Array && arr[0].cols == 2 && arr[0].kids.size() == 4 &&
              arr[0].open == '(',
          "math: pmatrix is a 2x2 array in parentheses");
    const math::List al = math::parse("a &= b \\\\ &= c");
    check(al.size() == 1 && al[0].k == Node::K::Array && al[0].text == "rl",
          "math: top-level & and \\\\ make an aligned block");
    const math::List unk = math::parse(R"(\frobnicate{x} + \left( y)");
    check(!unk.empty(), "math: unknown commands and an unclosed \\left still parse");

    // Garbage in never crashes the daemon: deep nesting, unbalanced input,
    // random token soup.
    {
      std::string deep(20000, '{');
      deep += "x";
      (void)math::draw(deep, true, 10, 20);
      std::string fracs, hats = "\\sqrt", subs = "x";
      for (int i = 0; i < 5000; i++) fracs += "\\frac{";
      for (int i = 0; i < 20000; i++) hats += "\\hat";
      for (int i = 0; i < 20000; i++) subs += "_1'^2";
      (void)math::draw(fracs, true, 10, 20);
      (void)math::draw(hats + "x", true, 10, 20);
      (void)math::draw(subs, false, 10, 20);
      static const char* kTok[] = {"\\frac", "{", "}", "^", "_", "&", "\\\\", "\\left(", "\\right)",
                                   "\\sqrt[", "]", "x", "\\begin{pmatrix}", "\\end{pmatrix}",
                                   "\\middle|", "\\over", "\\hat", "\\sum", "'", "\\not", "$", "\\"};
      std::mt19937 rng(7);
      for (int n = 0; n < 300; n++) {
        std::string soup;
        for (int k = 0; k < 40; k++) soup += kTok[rng() % (sizeof kTok / sizeof kTok[0])];
        const math::Image im = math::draw(soup, n % 2 == 0, 10, 20);
        if (im.alpha.size() != size_t(im.w) * size_t(im.h)) { check(false, "math: random input"); break; }
      }
      check(true, "math: malformed and deeply nested input");
    }

    // Layout: a display fraction is taller than a text one, and scripts are
    // never set below the legibility floor.
    math::LayoutOptions dopt, topt;
    dopt.display = true;
    topt.display = false;
    const math::Box db = math::layout(math::parse(R"(\frac{a}{b})"), dopt);
    const math::Box tb = math::layout(math::parse(R"(\frac{a}{b})"), topt);
    check(db.h + db.d > tb.h + tb.d && db.w > 0, "math: display fractions are larger than inline ones");
    math::LayoutOptions floor = dopt;
    floor.min_scale = 0.9f;
    const math::Box sb = math::layout(math::parse("x^{x^{x}}"), floor);
    bool small = false;
    for (const auto& it : sb.items) small |= it.kind == math::Item::Glyph && it.a < 0.85f;
    check(!small, "math: script sizes respect the minimum scale");

    // Drawing: whole cells, and inline math keeps its baseline row.
    const math::Image di = math::draw(R"(\sum_{i=1}^n i = \frac{n(n+1)}{2})", true, 10, 20);
    check(di.w == di.cols * 10 && di.h == di.rows * 20 && di.rows >= 3 &&
              di.alpha.size() == size_t(di.w) * size_t(di.h),
          "math: an image is exactly its cells");
    bool ink = false;
    for (uint8_t a : di.alpha) ink |= a > 128;
    check(ink, "math: an image has ink in it");
    const math::Image ii = math::draw(R"(\frac{1}{2})", false, 10, 20);
    check(ii.rows >= 2 && ii.base_row >= 1 && ii.base_row < ii.rows,
          "math: an inline fraction reserves a row above its baseline");
    const std::string png = math::png(ii, 0xFFFFFF);
    check(png.size() > 8 && png.compare(1, 3, "PNG") == 0, "math: --math writes a PNG");

    // Cells: placeholders with row and column diacritics, or blanks for a
    // terminal without the protocol.
    Surface s;
    s.resize(4, 1);
    s.at(1, 0) = Cell{char32_t(2u << 16 | 3u), Style{0x000007, kDefaultColor, attr::kImage}, 1};
    Surface f1, f2;
    std::string with, without;
    encode_frame(s, f1, with, true, ImageMode::Kitty);
    encode_frame(s, f2, without, true, ImageMode::None);
    check(with.find("\xF4\x8E\xBB\xAE" "\xCC\x8E" "\xCC\x90") != std::string::npos,
          "math: an image cell is U+10EEEE with its row and column marks");
    check(with.find("38;2;0;0;7") != std::string::npos, "math: the image id travels as the colour");
    check(without.find("\xF4\x8E\xBB\xAE") == std::string::npos,
          "math: a terminal without images gets a blank");
    check(selection_text(s, Point{0, 0}, Point{3, 0}, Rect{0, 0, 4, 1}).find('\xF4') == std::string::npos,
          "math: selecting over an image copies no placeholder bytes");

    // Configured on: markdown lays equations out as image rows.
    math::configure(math::Config{true, 10, 20, 0x56B6C2});
    const uint64_t gen = math::generation();
    Arena sc;
    std::vector<md::Seg> segs;
    std::vector<md::Line> lines;
    std::vector<text::Span> spans;
    std::vector<md::Seg> inl;
    md::Out o{size_t(-1), &sc, &segs, &lines, &spans, &inl};
    md::render("see:\n$$\n\\int_0^1 x\\,dx = \\frac{1}{2}\n$$\ndone", 0, false, 60, o);
    size_t image_rows = 0;
    uint32_t id = 0;
    for (const auto& L : lines)
      for (uint16_t k = 0; k < L.seg_count; k++) {
        int row, cols;
        if (md::image_ref(sc, segs[L.seg_first + k], &id, &row, &cols)) image_rows++;
      }
    const math::Image* shown = math::find(id);
    check(shown && image_rows == size_t(shown->rows) && lines.size() == image_rows + 2,
          "math: a closed $$ block becomes one image, one line per row");

    segs.clear(); lines.clear();
    md::render("still typing:\n$$\n\\int_0^1 x", 0, false, 60, o);
    bool any_image = false;
    for (const auto& sg : segs) any_image |= sg.ink == md::Ink::MathImage;
    check(!any_image, "math: a $$ block still streaming stays Unicode");

    segs.clear(); lines.clear();
    md::render("- the step is $\\eta = \\frac{1}{L}$ here", 0, false, 60, o);
    size_t lead = 0, bullet_line = SIZE_MAX;
    for (size_t li = 0; li < lines.size(); li++) {
      if (lines[li].flags & md::kLeadRow) lead++;
      for (uint16_t k = 0; k < lines[li].seg_count; k++)
        if (segs[lines[li].seg_first + k].ink == md::Ink::Bullet) bullet_line = li;
    }
    check(lead >= 1 && bullet_line == lead,
          "math: an inline fraction adds rows above its line, and the bullet stays on the text");

    segs.clear(); lines.clear();
    md::render("plain $x^2 + \\alpha$ math", 0, false, 60, o);
    any_image = false;
    for (const auto& sg : segs) any_image |= sg.ink == md::Ink::MathImage;
    check(!any_image && lines.size() == 1, "math: simple inline math stays one line of text");

    // Each terminal is sent an image once; a dropped image is freed.
    Surface screen;
    screen.resize(8, 1);
    screen.at(0, 0) = Cell{0, Style{Color(id), kDefaultColor, attr::kImage}, 1};
    math::KittyHeld sent;
    std::string first, second;
    math::send_images(screen, sent, first);
    math::send_images(screen, sent, second);
    check(first.find("\x1b_Ga=t") != std::string::npos && first.find("U=1") != std::string::npos &&
              second.empty(),
          "math: an image is transmitted once, with a virtual placement");
    std::string wrapped;
    math::KittyHeld sent_tmux;
    math::send_images(screen, sent_tmux, wrapped, true);
    check(wrapped.starts_with("\x1bPtmux;\x1b\x1b_Ga=t") && wrapped.find("o=z") != std::string::npos,
          "math: inside tmux each command is wrapped for passthrough, data compressed");

    // Copying over an equation gives its LaTeX, once, without the rows it
    // spans.
    Surface sel;
    sel.resize(6, 3);
    for (int y = 0; y < 3; y++)
      for (int x = 1; x < 4; x++)
        sel.at(x, y) = Cell{char32_t(uint32_t(y) << 16 | uint32_t(x - 1)),
                            Style{Color(id), kDefaultColor, attr::kImage}, 1};
    sel.at(0, 1) = Cell{U'a', Style{}, 1};
    const auto latex_of = [](uint32_t i) -> std::string {
      const math::Image* im = math::find(i);
      return im ? "[" + im->src + "]" : std::string();
    };
    const std::string copied = selection_text(sel, Point{0, 0}, Point{5, 2}, Rect{0, 0, 6, 3}, latex_of);
    check(copied == " [" + std::string(shown ? shown->src : "") + "]\na",
          "math: copying an equation gives its source once, and drops its other rows");

    // Sixel: a picture decodes back to the levels it was made of, and the
    // pass draws an image when its cells change and not again until then.
    {
      std::vector<uint8_t> a(20 * 13);
      for (size_t k = 0; k < a.size(); k++) a[k] = uint8_t((k * 37) % 256);
      const std::string six = math::sixel(a.data(), 20, 20, 13, 0xFFFFFF, 0x000000);
      std::vector<int> px;
      bool same = decode_sixel(six, 20, 13, px);
      for (size_t k = 0; same && k < a.size(); k++) same = px[k] == (a[k] * 15 + 127) / 255;
      check(same && six.starts_with("\x1bP") && six.ends_with("\x1b\\"),
            "math: a sixel picture decodes back to its pixels");

      Surface back, front;
      back.resize(10, 4);
      front.resize(10, 4);
      const math::Image* im = math::find(id);
      for (int y = 0; y < std::min(4, im ? im->rows : 0); y++)
        for (int x = 0; x < std::min(10, im->cols); x++)
          back.at(x, y) = Cell{char32_t(uint32_t(y) << 16 | uint32_t(x)),
                               Style{Color(id), 0x171D25, attr::kImage}, 1};
      std::string o1, o2;
      math::sixel_pass(back, front, o1);
      math::sixel_pass(back, front, o2);
      check(o1.find("\x1bP0;1;0q") != std::string::npos && o2.empty(),
            "math: the sixel pass draws a changed image once");
    }

    // Deflate: round trips, and an image-like buffer shrinks.
    {
      std::mt19937 rng(3);
      bool ok = true;
      size_t raw = 0, packed = 0;
      for (int t = 0; t < 60 && ok; t++) {
        std::string d(size_t(rng() % 5000), '\0');
        for (size_t k = 0; k < d.size(); k++)
          d[k] = t % 3 == 0 ? char(rng()) : t % 3 == 1 ? char(rng() % 4) : char(k % 4 == 3 ? (rng() % 9 ? 0 : rng()) : 0x56);
        const std::string z = math::zlib_compress(reinterpret_cast<const uint8_t*>(d.data()), d.size());
        std::string back_out;
        ok = inflate_fixed(z, back_out) && back_out == d;
        if (t % 3 == 2) { raw += d.size(); packed += z.size(); }
      }
      check(ok, "math: deflate round-trips");
      check(packed * 4 < raw, "math: an image-like buffer compresses well");
    }

    // The probe: replies are pulled out of whatever was typed meanwhile, and
    // a kitty too old for placeholders is refused.
    {
      std::string typed;
      const tty::ProbeReplies r = tty::read_probe_replies(
          "\x1bP>|kitty(0.32.2)\x1b\\x" "\x1b_Gi=31;OK\x1b\\" "\x1b[6;20;10t" "y\x1b[?62;4;22c", &typed);
      check(r.graphics_ok && r.sixel && r.version == "kitty(0.32.2)" && r.cell_w == 10 &&
                r.cell_h == 20 && typed == "xy",
            "probe: replies are read and keystrokes kept");
      check(tty::kitty_version_ok("kitty(0.32.2)", true) && tty::kitty_version_ok("kitty(1.0.0)", true) &&
                !tty::kitty_version_ok("kitty(0.27.1)", true) && !tty::kitty_version_ok("", true) &&
                tty::kitty_version_ok("ghostty 1.1.0", false),
            "probe: kitty older than 0.28 is refused");
      const tty::ProbeReplies plain = tty::read_probe_replies("\x1b[?62;22c", nullptr);
      check(!plain.sixel && !plain.graphics_ok, "probe: no sixel unless the terminal lists it");
    }

    // Charts: a real plot when images are on, cached, and a live chart's
    // next frame replaces its last.
    {
      chart::Spec cs;
      std::string why;
      check(chart::parse(R"({"type":"line","title":"t","series":[{"name":"a","y":[1,3,2]}]})", cs, &why),
            "chart image: spec parses");
      const math::Image* ci = chart::image(cs, "block", 60);
      check(ci && ci->cols == 60 && ci->rows > cs.height && ci->rgba.size() == size_t(ci->w) * size_t(ci->h) * 4 &&
                ci->alpha.empty() && ci->copy == "[chart: t]",
            "chart image: a line chart becomes a colour picture of whole cells");
      bool coloured = false;
      if (ci)
        for (size_t k = 0; k + 3 < ci->rgba.size() && !coloured; k += 4)
          coloured = ci->rgba[k + 3] > 200 && ci->rgba[k] != ci->rgba[k + 2];
      check(coloured, "chart image: it has opaque coloured ink");
      check(chart::image(cs, "block", 60) == ci, "chart image: drawn once, then cached");
      check(chart::image(cs, "block", 20) == nullptr, "chart image: too narrow to read stays cells");

      chart::Spec live = cs;
      live.file = "metrics.csv";
      const math::Image* f1 = chart::image(live, "live", 60);
      const uint32_t f1_id = f1 ? f1->id : 0;
      live.series[0].y.push_back(5);
      const math::Image* f2 = chart::image(live, "live", 60);
      check(f1_id && f2 && f2->id != f1_id && math::find(f1_id) == nullptr,
            "chart image: a live chart's new frame replaces the old one");

      // In markdown, a ```chart block lays out as image rows.
      Arena csc;
      std::vector<md::Seg> csegs;
      std::vector<md::Line> clines;
      std::vector<text::Span> cspans;
      std::vector<md::Seg> cinl;
      md::Out co{size_t(-1), &csc, &csegs, &clines, &cspans, &cinl};
      md::render("```chart\n{\"type\":\"bar\",\"labels\":[\"a\",\"b\"],\"series\":[{\"y\":[1,2]}]}\n```\n",
                 0, false, 80, co);
      // Each bar row: its label as text, then the bar's picture cells.
      bool rows_ok = clines.size() == 2;
      for (const auto& L : clines) {
        bool label = false, bar = false;
        for (uint16_t k = 0; k < L.seg_count; k++) {
          const md::Seg& sg = csegs[L.seg_first + k];
          bar |= sg.ink == md::Ink::MathImage;
          label |= sg.ink == md::Ink::Text && !bar;
        }
        rows_ok &= label && bar;
      }
      check(rows_ok, "chart image: a chart block becomes rows of labels and picture cells");
    }

    // Figures: labels in cells around the plot's picture, subplots side by
    // side, the math font as one picture.
    {
      const auto images_in = [](const std::vector<chart::Piece>& row) {
        int n = 0;
        for (const auto& p : row) n += p.image != 0;
        return n;
      };
      const auto text_of = [](const std::vector<chart::Piece>& row) {
        std::string t;
        for (const auto& p : row) if (!p.image) t += p.text;
        return t;
      };
      chart::Spec fs;
      std::string why;
      chart::parse(R"({"type":"line","title":"t","xlabel":"step","x":[0,10,20,30],"series":[{"y":[0,4,2,8]}],"height":8})", fs, &why);
      chart::Figure f;
      chart::figure(fs, "fig", 70, f);
      int plot_rows = 0, labelled = 0;
      bool aligned = true;
      for (const auto& row : f.rows)
        if (images_in(row) == 1) {
          plot_rows++;
          const std::string margin = row[0].image ? std::string() : row[0].text;
          if (margin.find_first_not_of(' ') != std::string::npos) labelled++;
          aligned &= !row[0].image && row[1].image;
        }
      check(f.width <= 70 && plot_rows >= 5 && labelled >= 2 && aligned,
            "figure: the plot is a picture, its y labels text in the margin beside tick rows");
      bool xticks = false, title = false;
      for (const auto& row : f.rows) {
        xticks |= images_in(row) == 0 && text_of(row).find("30") != std::string::npos;
        title |= !row.empty() && row[0].ink == chart::kInkTitle && row[0].text == "t";
      }
      check(xticks && title, "figure: the title and the x tick labels are text rows");

      chart::Spec bs;
      chart::parse(R"({"type":"bar","labels":["short","longer"],"series":[{"y":[1,4]}]})", bs, &why);
      chart::figure(bs, "bars", 60, f);
      bool bars_ok = f.rows.size() == 2;
      int c0 = 0, c1 = 0;
      for (size_t i = 0; bars_ok && i < f.rows.size(); i++) {
        const auto& row = f.rows[i];
        bars_ok = row.size() == 3 && row[1].image && text_of(row).find(i ? "4" : "1") != std::string::npos;
        (i ? c1 : c0) = bars_ok ? row[1].cols : 0;
      }
      check(bars_ok && c0 < c1, "figure: a bar is as many picture cells as it is long, its value after it");

      chart::Spec ps;
      chart::parse(R"({"title":"both","subplots":[{"y":[1,3,2],"height":6},{"type":"scatter","y":[5,1,4],"height":6}]})", ps, &why);
      chart::figure(ps, "pair", 100, f);
      int side = 0;
      for (const auto& row : f.rows) side = std::max(side, images_in(row));
      check(side == 2 && f.width <= 100 && !f.rows.empty() && f.rows[0][0].text == "both",
            "figure: subplots sit side by side under the figure's title");
      chart::Spec cs1;
      chart::parse(R"({"columns":1,"subplots":[{"y":[1,3,2],"height":6},{"y":[5,1,4],"height":6}]})", cs1, &why);
      chart::figure(cs1, "stack", 100, f);
      side = 0;
      for (const auto& row : f.rows) side = std::max(side, images_in(row));
      check(side == 1, "figure: \"columns\": 1 stacks them");
      chart::figure(ps, "pair", 40, f);
      side = 0;
      for (const auto& row : f.rows) side = std::max(side, images_in(row));
      check(side == 1, "figure: too narrow for two, they stack");

      chart::Spec ms;
      chart::parse(R"({"font":"math","subplots":[{"y":[1,2]},{"font":"terminal","y":[2,1]}]})", ms, &why);
      check(ms.subplots[0].font == chart::Spec::Font::Math && ms.subplots[1].font == chart::Spec::Font::Terminal,
            "figure: subplots take the figure's font unless they name their own");
      chart::Spec one;
      chart::parse(R"({"font":"math","y":[1,3,2]})", one, &why);
      chart::figure(one, "m", 60, f);
      bool whole = !f.rows.empty();
      for (const auto& row : f.rows) whole &= row.size() == 1 && row[0].image;
      check(whole, "figure: the math font is one picture");
    }

    math::configure(math::Config{false, 10, 20, 0x56B6C2});
    {
      chart::Spec cs;
      std::string why;
      chart::parse(R"({"type":"line","series":[{"y":[1,2]}]})", cs, &why);
      check(chart::image(cs, "x", 60) == nullptr, "chart image: images off, charts stay cells");
      chart::Spec ps;
      chart::parse(R"({"subplots":[{"y":[1,3,2]},{"type":"bar","labels":["a","b"],"series":[{"y":[1,2]}]}]})", ps, &why);
      chart::Figure f;
      chart::figure(ps, "cells", 100, f);
      bool no_images = !f.rows.empty();
      for (const auto& row : f.rows)
        for (const auto& p : row) no_images &= p.image == 0;
      check(no_images && f.width > 60, "figure: without images, subplots side by side in cells");
    }
    std::vector<uint32_t> gone;
    math::take_evicted(gone);
    std::string freed;
    math::free_images(gone, sent, freed);
    check(math::generation() != gen && freed.find("a=d") != std::string::npos && sent.empty(),
          "math: turning images off drops them, and terminals free them");
  }

  // Markdown structure: nested and task lists, callouts, details, strikes, tables.
  {
    struct Rendered {
      std::vector<std::string> rows;
      std::vector<std::vector<std::pair<std::string, uint8_t>>> segs;  // text and attr per segment
      std::vector<int> indent;
    };
    const auto render = [](const std::string& msg, int cols) {
      Arena sc;
      std::vector<md::Seg> segs;
      std::vector<md::Line> lines;
      std::vector<text::Span> spans;
      std::vector<md::Seg> inl;
      md::Work wk;
      md::Out o{size_t(-1), &sc, &segs, &lines, &spans, &inl, &wk};
      md::render(msg, 0, false, cols, o);
      Rendered r;
      for (const auto& L : lines) {
        std::string t;
        std::vector<std::pair<std::string, uint8_t>> ss;
        for (uint16_t k = 0; k < L.seg_count; k++) {
          const md::Seg& sg = segs[L.seg_first + k];
          const std::string piece = (sg.off & md::kScratchBit)
                                        ? std::string(sc.view(Str{sg.off & ~md::kScratchBit, sg.len}))
                                        : msg.substr(sg.off, sg.len);
          t += piece;
          ss.emplace_back(piece, sg.attr);
        }
        r.rows.push_back(t);
        r.segs.push_back(ss);
        r.indent.push_back(L.indent);
      }
      return r;
    };
    const auto row_with = [](const Rendered& r, std::string_view needle) {
      for (size_t i = 0; i < r.rows.size(); i++)
        if (r.rows[i].find(needle) != std::string::npos) return int(i);
      return -1;
    };
    const auto lead = [](const Rendered& r, int i) {
      return i < 0 ? -1 : r.indent[size_t(i)] + int(r.rows[size_t(i)].find_first_not_of(' '));
    };

    Rendered r = render("- alpha\n  - beta\n    - gamma\n- delta\n\n1. one\n2. two\n", 60);
    const int a = row_with(r, "alpha"), b = row_with(r, "beta"), g = row_with(r, "gamma");
    check(a >= 0 && b >= 0 && g >= 0 && r.rows[size_t(a)].find("\xE2\x80\xA2") != std::string::npos &&  // •
              r.rows[size_t(b)].find("\xE2\x97\xA6") != std::string::npos &&                           // ◦
              r.rows[size_t(g)].find("\xE2\x96\xAA") != std::string::npos &&                           // ▪
              lead(r, a) < lead(r, b) && lead(r, b) < lead(r, g),
          "markdown: nested lists step in, each level its own bullet");
    check(row_with(r, "1.") >= 0 && row_with(r, "2.") >= 0 && row_with(r, "two") == row_with(r, "2."),
          "markdown: ordered lists keep their numbers");

    r = render("- [ ] todo item\n- [x] done item\n", 60);
    const int td = row_with(r, "todo item"), dn = row_with(r, "done item");
    bool struck = false;
    if (dn >= 0)
      for (const auto& [t, at] : r.segs[size_t(dn)]) struck |= t.find("done") != std::string::npos && (at & md::kAttrStrike);
    check(td >= 0 && dn >= 0 && r.rows[size_t(td)].find("\xE2\x98\x90") != std::string::npos &&  // ☐
              r.rows[size_t(dn)].find("\xE2\x98\x91") != std::string::npos && struck &&            // ☑
              row_with(r, "[x]") < 0,
          "markdown: task items are boxes, a done one struck through");

    r = render("> [!WARNING]\n> mind the gap\n", 60);
    check(row_with(r, "Warning") >= 0 && row_with(r, "mind the gap") >= 0 && row_with(r, "[!WARNING]") < 0,
          "markdown: a callout names its kind instead of its tag");

    r = render("<details>\n<summary>More detail</summary>\n\nthe inside\n</details>\n", 60);
    check(row_with(r, "\xE2\x96\xBE More detail") >= 0 && row_with(r, "the inside") >= 0 && row_with(r, "<details") < 0 &&
              row_with(r, "summary>") < 0,
          "markdown: <details> shows its summary and contents, not its tags");

    r = render("~~gone~~ but kept\n", 60);
    struck = false;
    for (const auto& [t, at] : r.segs[0]) struck |= t == "gone" && (at & md::kAttrStrike);
    check(struck && row_with(r, "~~") < 0 && row_with(r, "but kept") == 0, "markdown: ~~strikethrough~~");

    r = render("| name | count |\n|---|---|\n| apples | 5 |\n| pears | 1234 |\n", 60);
    const int ra = row_with(r, "apples"), rp = row_with(r, "pears");
    check(ra >= 0 && rp >= 0 && r.rows[size_t(ra)].find_last_not_of(' ') == r.rows[size_t(rp)].find_last_not_of(' ') &&
              r.rows[size_t(ra)].find('5') > r.rows[size_t(rp)].find('1'),
          "markdown: a column of numbers is right-aligned");

    const std::string words = "the quick brown fox jumps over the lazy dog again and again";
    r = render("| key | description |\n|---|---|\n| k | " + words + " |\n", 30);
    bool narrow = true;
    std::string joined;
    for (const auto& row : r.rows) {
      narrow &= text::str_width(row) <= 30;
      joined += row;
    }
    bool all_words = true;
    for (std::string_view w : {"quick", "brown", "lazy", "again"}) all_words &= joined.find(w) != std::string::npos;
    check(narrow && all_words && r.rows.size() >= 5, "markdown: a table too wide wraps its cells, losing nothing");
  }

  // Charts: histograms bin, sparklines take a row each, heatmaps draw.
  {
    math::configure(math::Config{false, 10, 20, 0x56B6C2});
    std::string why;
    chart::Spec hs;
    check(chart::parse(R"({"type":"hist","values":[1,2,2,3,3,3,4,4,5],"bins":5})", hs, &why), "chart: hist parses");
    const chart::Spec hb = chart::binned(hs);
    double total = 0;
    for (double c : hb.series[0].y) total += c;
    check(hb.series.size() == 1 && hb.series[0].y.size() == 5 && total == 9 && hb.series[0].y[2] == 3,
          "chart: a histogram counts every value into its bins");
    chart::Spec ss;
    chart::parse(R"({"type":"spark","series":[{"name":"cpu","y":[1,3,2,5]},{"name":"mem","y":[2,2,3,4]}]})", ss, &why);
    chart::Figure f;
    chart::figure(ss, "spark", 60, f);
    std::string all;
    for (const auto& row : f.rows)
      for (const auto& p : row) all += p.text;
    check(f.rows.size() == 2 && all.find("cpu") != std::string::npos && all.find("mem") != std::string::npos &&
              all.find('5') != std::string::npos,
          "chart: sparklines, a row each, named, with their last value");
    chart::Spec ms;
    check(chart::parse(R"({"type":"heatmap","z":[[1,2],[3,4]],"labels":["a","b"],"ylabels":["r1","r2"]})", ms, &why),
          "chart: heatmap parses");
    chart::figure(ms, "heat", 60, f);
    all.clear();
    for (const auto& row : f.rows)
      for (const auto& p : row) all += p.text;
    check(!f.rows.empty() && all.find("r1") != std::string::npos && all.find("r2") != std::string::npos,
          "chart: a heatmap in cells, rows labelled");
  }

  // Notebooks: Claude's reading of one, and an .ipynb's own JSON.
  {
    const std::string line =
        R"nb({"type":"user","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"t1","content":[)nb"
        R"nb({"type":"text","text":"<cell id=\"c0\"><cell_type>markdown</cell_type># Loss\nIt falls.</cell id=\"c0\">"},)nb"
        R"nb({"type":"text","text":"<cell id=\"c1\">import numpy as np\nprint(np.pi)</cell id=\"c1\">"},)nb"
        R"nb({"type":"text","text":"\n3.14159"},)nb"
        R"nb({"type":"image","source":{"type":"base64","media_type":"image/png","data":"iVBORw0KGgoAAAANSUhEUg=="}},)nb"
        R"nb({"type":"text","text":"<cell id=\"c2\"><language>r</language>x <- 1</cell id=\"c2\">"}]}]}})nb";
    std::string md;
    check(notebook::is_claude_read("<cell id=\"a\">x</cell id=\"a\">") && !notebook::is_claude_read("<cell>"),
          "notebook: Claude's reading is recognised");
    check(notebook::from_claude(line, md) && md.find("# Loss\nIt falls.\n") == 0 &&
              md.find("```python\nimport numpy as np\nprint(np.pi)\n```") != std::string::npos &&
              md.find("```out\n3.14159\n```") != std::string::npos &&
              md.find("![output](data:image/png;base64,iVBORw0KGgoAAAANSUhEUg==)") != std::string::npos &&
              md.find("```r\nx <- 1\n```") != std::string::npos &&
              md.find("3.14159") < md.find("![output]") && md.find("![output]") < md.find("x <- 1"),
          "notebook: cells, outputs, and each plot under its own cell");
    const std::string nb = R"nb({"nbformat":4,"metadata":{"language_info":{"name":"python"}},"cells":[)nb"
                           R"nb({"cell_type":"code","source":["df.head()"],"outputs":[{"output_type":"execute_result","data":{)nb"
                           R"nb("text/html":["<table><thead><tr><th></th><th>a</th><th>b</th></tr></thead><tbody>",)nb"
                           R"nb("<tr><th>0</th><td>1</td><td>x &amp; y</td></tr></tbody></table>"],"text/plain":["   a  b"]}}]},)nb"
                           R"nb({"cell_type":"code","source":"1/0","outputs":[{"output_type":"error","ename":"ZeroDivisionError",)nb"
                           R"nb("evalue":"division by zero","traceback":["\u001b[0;31mZeroDivisionError\u001b[0m: division by zero"]}]},)nb"
                           R"nb({"cell_type":"code","source":"```oops","outputs":[]}]})nb";
    check(notebook::from_ipynb(nb, md) && md.find("```python\ndf.head()\n```") != std::string::npos &&
              md.find("|  | a | b |\n|---|---|---|\n| 0 | 1 | x & y |") != std::string::npos &&
              md.find("```out\nZeroDivisionError: division by zero\n```") != std::string::npos &&
              md.find("\xE2\x80\x8B```oops") != std::string::npos,
          "notebook: a DataFrame as a table, a traceback without its colours, fences kept shut");
    const std::string frame =
        R"nb({"type":"user","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"t1","content":[)nb"
        R"nb({"type":"text","text":"<cell id=\"c0\">df</cell id=\"c0\">"},)nb"
        R"nb({"type":"text","text":"\n     city  pop_m\n0   Paris   2.10\n1  Berlin   3.70\n2    Rome   2.80\n\n[3 rows x 2 columns]"},)nb"
        R"nb({"type":"text","text":"<cell id=\"c1\">s</cell id=\"c1\">"},)nb"
        R"nb({"type":"text","text":"\n0    1\n1    2\ndtype: int64"},)nb"
        R"nb({"type":"text","text":"<cell id=\"c2\">print(x)</cell id=\"c2\">"},)nb"
        R"nb({"type":"text","text":"\nloss 0.31\ndone"}]}]}})nb";
    check(notebook::from_claude(frame, md) &&
              md.find("|  | city | pop_m |\n|---|---|---|\n| 0 | Paris | 2.10 |\n| 1 | Berlin | 3.70 |") != std::string::npos &&
              md.find("*[3 rows x 2 columns]*") != std::string::npos &&
              md.find("```out\n0    1\n1    2\ndtype: int64\n```") != std::string::npos &&
              md.find("```out\nloss 0.31\ndone\n```") != std::string::npos,
          "notebook: a DataFrame's text becomes a table; a Series or a print stays text");
    check(!notebook::from_ipynb(R"({"cells":[]})", md) && !notebook::from_ipynb("[1]", md), "notebook: not a notebook");
  }

  // JSON results: laid out whole when expanded, a one-line outline folded.
  {
    const std::string j = R"({"name":"mico","tags":[1,2,3],"items":[{"id":1,"v":"a"},{"id":2,"v":"b"}],"deep":{"x":{"y":true}},"e":[]})";
    std::string out;
    check(json_view::pretty(j, false, out) &&
              out == "{\n  \"name\": \"mico\",\n  \"tags\": [1, 2, 3],\n  \"items\": [\n    {\n      \"id\": 1,\n"
                     "      \"v\": \"a\"\n    },\n    {\n      \"id\": 2,\n      \"v\": \"b\"\n    }\n  ],\n"
                     "  \"deep\": {\n    \"x\": {\n      \"y\": true\n    }\n  },\n  \"e\": []\n}\n",
          "json: indented, short plain arrays on one line");
    check(json_view::pretty(j, true, out) &&
              out == "{ \"name\": \"mico\", \"tags\": [\xE2\x80\xA6" "3 items], \"items\": [\xE2\x80\xA6" "2 items], "
                     "\"deep\": {\xE2\x80\xA6" "1 key}, \"e\": [] }\n",
          "json: folded, the top level with what each member holds");
    std::string long_s = "{\"s\":\"" + std::string(200, 'x') + "\"}";
    check(json_view::pretty(long_s, true, out) && out.size() < 80 && out.find("\xE2\x80\xA6\"") != std::string::npos,
          "json: folded, long strings shortened");
    // Folding: marks on opening lines, a big document partly closed, and a
    // flipped container the other way.
    {
      json_view::Folding fo;
      check(json_view::pretty(R"({"a":{"b":{"c":1}},"d":2})", false, out, &fo) &&
                out == "\xE2\x96\xBE {\n  \xE2\x96\xBE \"a\": {\n    \xE2\x96\xBE \"b\": {\n        \"c\": 1\n      }\n    },\n"
                       "    \"d\": 2\n  }\n" &&
                fo.line_nodes == std::vector<uint16_t>({1, 2, 3, 0, 0, 0, 0, 0}),
            "json: folding marks each container's opening line");
      std::string big = "{\"rows\":[";
      for (int i = 0; i < 40; i++) big += std::string(i ? "," : "") + "{\"id\":" + std::to_string(i) + ",\"tags\":{\"x\":1}}";
      big += "]}";
      check(json_view::pretty(big, false, out, &fo) && out.find("\xE2\x96\xB8 {\xE2\x80\xA6" "2 keys},") != std::string::npos &&
                std::count(out.begin(), out.end(), '\n') < 50,
            "json: a big document opens with its deeper containers closed");
      const std::unordered_set<int> flipped{2};  // the first row object
      fo.flipped = &flipped;
      check(json_view::pretty(big, false, out, &fo) && out.find("\"id\": 0,") != std::string::npos &&
                out.find("\"id\": 1,") == std::string::npos,
            "json: a flipped container opens on its own");
    }
    check(!json_view::pretty("{\"a\": 1,}", false, out) && !json_view::pretty("42", false, out) &&
              !json_view::pretty("{\"a\":1} trailing", false, out) && !json_view::pretty("not json", false, out),
          "json: not one valid object or array, not laid out");
  }

  // Short-lived programs: found on PATH, their output collected, cut short
  // at the cap or what is enough, stopped at the deadline.
  {
    check(!proc::find_program("sh").empty() && proc::find_program("/bin/sh") == "/bin/sh" &&
              proc::find_program("mico-no-such-program").empty() && proc::find_program("").empty(),
          "process: programs are found on PATH, or by a path, or not at all");
    proc::Result r = proc::capture({"sh", "-c", "printf hello; exit 0"});
    check(r.ok() && r.out == "hello", "process: what a program prints, and that it succeeded");
    r = proc::capture({"sh", "-c", "printf partial; exit 3"});
    check(!r.ok() && r.ran && r.exit_code == 3 && r.out == "partial", "process: a failure keeps its exit code and output");
    r = proc::capture({"mico-no-such-program"});
    check(!r.ran && !r.ok(), "process: a missing program does not run");
    proc::Options cap;
    cap.cap = 10;
    r = proc::capture({"sh", "-c", "yes"}, cap);
    check(r.cut && r.out.size() == 10 && !r.ok(), "process: output past the cap stops the program");
    proc::Options enough;
    enough.enough = [](const std::string& got) { return got.find('\n') != std::string::npos; };
    r = proc::capture({"sh", "-c", "echo one; sleep 5; echo two"}, enough);
    check(r.cut && r.out == "one\n", "process: enough output lets the program go early");
    proc::Options slow;
    slow.timeout_ms = 100;
    r = proc::capture({"sh", "-c", "sleep 5"}, slow);
    check(r.timed_out && !r.ok() && r.exit_code == -1, "process: a program past its deadline is stopped");
  }

  // Progress read from a running command's output.
  {
    progress::Progress pr;
    check(progress::parse(" 45%|\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x8C     | 450/1000 [00:12<00:15, 36.2it/s]", pr) &&
              std::abs(pr.fraction - 0.45) < 1e-9 && pr.done == 450 && pr.total == 1000 && pr.eta_s == 15,
          "progress: tqdm, with its time left");
    check(progress::parse("[123/456] Building CXX object src/CMakeFiles/mico.dir/views/chart.cpp.o", pr) &&
              pr.done == 123 && pr.total == 456 && pr.eta_s < 0,
          "progress: a ninja counter");
    check(progress::parse("  \xE2\x8E\xBF  [ 37%] Building CXX object foo.o", pr) && std::abs(pr.fraction - 0.37) < 1e-9,
          "progress: make's percentage, behind the agent's output mark");
    check(progress::parse("    Building [=======================>   ] 260/289: tokio", pr) && pr.done == 260,
          "progress: cargo");
    check(progress::parse("   \xE2\x94\x81\xE2\x94\x81\xE2\x94\x81\xE2\x94\x81\xE2\x94\x81\xE2\x94\x81\xE2\x95\xB8\xE2\x94\x81\xE2\x94\x81 2.1/5.0 MB 3.4 MB/s eta 0:00:02", pr) &&
              pr.eta_s == 2 && std::abs(pr.fraction - 0.42) < 1e-9,
          "progress: pip");
    check(!progress::parse("Context left until auto-compact: 12%", pr) && !progress::parse("coverage is 85% now", pr) &&
              !progress::parse("released 2024/05/01", pr) && !progress::parse("see src/a/b.cpp", pr),
          "progress: a bare percentage, a date or a path is not progress");
    check(progress::bar(0.5, 4) == "\xE2\x96\x88\xE2\x96\x88  " && progress::bar(1, 2) == "\xE2\x96\x88\xE2\x96\x88" &&
              progress::duration(125) == "2m 05s",
          "progress: bars in eighths, durations");
  }

  // Mermaid: flowcharts, state and sequence diagrams drawn in cells.
  {
    const auto drawn = [](std::string_view src, int cols, std::string* text) {
      chart::Figure f;
      if (!diagram::draw(src, cols, f)) return false;
      text->clear();
      for (const auto& row : f.rows) {
        for (const auto& p : row) *text += p.text;
        *text += '\n';
      }
      return f.width <= cols;
    };
    std::string t;
    check(drawn("graph TD\n  A[Start] --> B{Ok?}\n  B -->|yes| C(Done)\n  B -- no --> A\n", 60, &t) &&
              t.find("Start") != std::string::npos && t.find("\xE2\x95\x94") != std::string::npos &&  // ╔ a decision
              t.find("\xE2\x95\xAD") != std::string::npos && t.find("yes") != std::string::npos &&    // ╭ rounded
              t.find("no") != std::string::npos && t.find("\xE2\x96\xBC") != std::string::npos &&     // ▼
              t.find("\xE2\x96\xB2") != std::string::npos,                                             // ▲ a loop back
          "mermaid: a flowchart's shapes, labels, and an edge back up");
    std::string first_line = t.substr(0, t.find('\n'));
    check(first_line.find("Start") == std::string::npos && t.find("Start") < t.find("Done"),
          "mermaid: top-down, first node on top");
    check(drawn("flowchart LR\n  a --> b --> c\n", 60, &t) && t.find('\n') != std::string::npos &&
              t.find("\xE2\x96\xB6") != std::string::npos,  // ▶
          "mermaid: left to right");
    const std::string wide = "graph LR\n  a[aaaaaaaaaaaa] --> b[bbbbbbbbbbbb] --> c[cccccccccccc] --> d[dddddddddddd]\n";
    check(drawn(wide, 30, &t) && t.find("\xE2\x96\xBC") != std::string::npos,
          "mermaid: too wide across turns top-down");
    check(!drawn("graph TD\n  a[a very long label here] & b[another very long label] & c[and a third] --> d\n", 20, &t),
          "mermaid: what cannot fit is not drawn");
    check(drawn("stateDiagram-v2\n  [*] --> Idle\n  Idle --> Busy : go\n  Busy --> [*]\n", 60, &t) &&
              t.find("\xE2\x97\x8F") != std::string::npos && t.find("\xE2\x97\x89") != std::string::npos &&
              t.find("go") != std::string::npos,
          "mermaid: a state diagram's start, end, and transitions");
    check(drawn("sequenceDiagram\n  participant A as Alice\n  A->>B: hi\n  B-->>A: hello\n  loop again\n  A->>A: think\n  end\n",
                60, &t) &&
              t.find("Alice") != std::string::npos && t.find("\xE2\x96\xB6") != std::string::npos &&
              t.find("\xE2\x97\x80") != std::string::npos && t.find("\xE2\x94\x84") != std::string::npos &&
              t.find("loop again") != std::string::npos && t.find("think") != std::string::npos,
          "mermaid: a sequence diagram's messages, replies and blocks");
    check(drawn("graph TD\n  x[Outside] --> a\n  subgraph g1 [Group]\n    a[Inner A] --> b[Inner B]\n  end\n", 60, &t) &&
              t.find(" Group ") != std::string::npos && t.find("\xE2\x95\x8C") != std::string::npos &&  // ╌
              t.find("\xE2\x95\x8E") != std::string::npos &&                                            // ╎
              t.find("Outside") < t.find("Inner A"),
          "mermaid: a subgraph is a titled box round its members");
    check(drawn("stateDiagram-v2\n  [*] --> A\n  state Busy {\n    A --> B\n  }\n", 60, &t) && t.find(" Busy ") != std::string::npos,
          "mermaid: a composite state is a box");
    check(drawn("pie title Pets\n  \"Dogs\" : 3\n  \"Cats\" : 2\n", 60, &t) && t.find("Dogs") != std::string::npos,
          "mermaid: a pie as bars");
    check(!drawn("gantt\n  title x\n", 60, &t) && !drawn("graph TD\n", 60, &t), "mermaid: kinds not drawn stay code");

    Arena sc;
    std::vector<md::Seg> segs;
    std::vector<md::Line> lines;
    std::vector<text::Span> spans;
    std::vector<md::Seg> inl;
    md::Work wk;
    md::Out o{size_t(-1), &sc, &segs, &lines, &spans, &inl, &wk};
    const std::string msg = "Before\n\n```mermaid\ngraph TD\n  A --> B\n```\nAfter\n";
    md::render(msg, 0, false, 60, o);
    std::string all;
    bool code_rows = false;
    for (const auto& L : lines) {
      code_rows |= (L.flags & md::kCodeRow) != 0;
      for (uint16_t k = 0; k < L.seg_count; k++) {
        const md::Seg& sg = segs[L.seg_first + k];
        all += (sg.off & md::kScratchBit) ? std::string(sc.view(Str{sg.off & ~md::kScratchBit, sg.len}))
                                          : msg.substr(sg.off, sg.len);
      }
      all += '\n';
    }
    check(!code_rows && all.find("\xE2\x94\x8C") != std::string::npos && all.find("graph") == std::string::npos &&
              all.find("After") != std::string::npos,
          "mermaid: a ```mermaid block in a message is drawn, not shown as code");
  }

  // Code: the highlighter, and how a fenced block is laid out.
  {
    using code::Tok;
    const auto toks = [](std::string_view lang, std::string_view line, code::State& st) {
      std::vector<code::Run> runs;
      code::highlight(line, code::lang_of(lang), st, runs);
      std::vector<std::pair<std::string, Tok>> out;
      for (const auto& r : runs)
        if (std::string(line.substr(r.off, r.len)).find_first_not_of(' ') != std::string::npos)
          out.emplace_back(std::string(line.substr(r.off, r.len)), r.tok);
      return out;
    };
    const auto has_tok = [](const std::vector<std::pair<std::string, Tok>>& v, std::string_view text, Tok t) {
      for (const auto& [x, k] : v)
        if (x == text && k == t) return true;
      return false;
    };
    code::State st;
    auto v = toks("python", R"(def f(x): return "a#b"  # note)", st);
    check(has_tok(v, "def", Tok::Keyword) && has_tok(v, "f", Tok::Func) && has_tok(v, R"("a#b")", Tok::String) &&
              has_tok(v, "# note", Tok::Comment),
          "code: python keywords, calls, strings, and a comment after a string's #");
    st = {};
    toks("cpp", "int x = 1; /* starts", st);
    v = toks("cpp", "still a comment */ return 0x1F;", st);
    check(has_tok(v, "still a comment */", Tok::Comment) && has_tok(v, "return", Tok::Keyword) &&
              has_tok(v, "0x1F", Tok::Number) && st.open == 0,
          "code: a block comment runs on to the line it closes on");
    st = {};
    v = toks("py", R"(s = f"x{y}" + '''doc)", st);
    check(has_tok(v, R"(f"x{y}")", Tok::String) && st.open == 3, "code: string prefixes, an open triple quote");
    st = {};
    v = toks("rust", R"(fn f<'a>(s: &'a str) { println!("{s}"); })", st);
    check(has_tok(v, "'a", Tok::Type) && has_tok(v, "println!", Tok::Func) && has_tok(v, "str", Tok::Type),
          "code: rust lifetimes, macros, types");
    st = {};
    v = toks("json", R"({"key": "value", "n": true})", st);
    check(has_tok(v, R"("key")", Tok::Type) && has_tok(v, R"("value")", Tok::String) && has_tok(v, "true", Tok::Keyword),
          "code: JSON keys stand apart from values");
    st = {};
    v = toks("bash", "echo $HOME#x # real comment", st);
    check(has_tok(v, "$HOME", Tok::Func) && has_tok(v, "# real comment", Tok::Comment) &&
              !has_tok(v, "#x # real comment", Tok::Comment),
          "code: shell variables, and # only after a space");
    st = {};
    v = toks("sql", "SELECT name FROM users -- all", st);
    check(has_tok(v, "SELECT", Tok::Keyword) && has_tok(v, "FROM", Tok::Keyword) && has_tok(v, "-- all", Tok::Comment),
          "code: SQL keywords in any case");
    st = {};
    v = toks("diff", "-old line", st);
    check(v.size() == 1 && v[0].second == Tok::Removed, "code: diff lines");
    check(code::lang_of("{.python}") && code::lang_of("C++") && !code::lang_of("text") && !code::lang_of(""),
          "code: fence names, decorated or not");

    // A fenced block: rows flagged as code, tabs expanded, long lines wrapped
    // under their indent with a mark, the language on the first row.
    Arena sc;
    std::vector<md::Seg> segs;
    std::vector<md::Line> lines;
    std::vector<text::Span> spans;
    std::vector<md::Seg> inl;
    md::Work wk;
    md::Out o{size_t(-1), &sc, &segs, &lines, &spans, &inl, &wk};
    const std::string msg = "```go\nfunc f() {\n\treturn aaaa + bbbb + cccc + dddd + eeee + ffff + gggg\n}\n```\n";
    md::render(msg, 0, false, 40, o);
    const auto row_text = [&](const md::Line& L) {
      std::string t;
      for (uint16_t k = 0; k < L.seg_count; k++) {
        const md::Seg& sg = segs[L.seg_first + k];
        t += (sg.off & md::kScratchBit) ? std::string(sc.view(Str{sg.off & ~md::kScratchBit, sg.len}))
                                        : msg.substr(sg.off, sg.len);
      }
      return t;
    };
    bool all_code = !lines.empty(), tabs = false, wrapped = false, label = false, fits = true;
    for (const auto& L : lines) {
      all_code &= (L.flags & md::kCodeRow) != 0;
      const std::string t = row_text(L);
      tabs |= t.starts_with("     return");             // padding, then a tab as four spaces
      wrapped |= t.starts_with("\xE2\x86\xAA    ");      // a mark, then the line's indent
      label |= t.find("func f()") != std::string::npos && t.ends_with("go");
      fits &= text::str_width(t) <= 38;
    }
    check(all_code && tabs && wrapped && label && fits && lines.size() >= 4,
          "code: a block is code rows, tabs expanded, wrapped under its indent, labelled");

    // Copying a wrapped code line gives it back whole, without its marks.
    Surface cs;
    cs.resize(20, 3);
    const auto put = [&](int x, int y, std::string_view t, uint16_t a) {
      for (size_t i = 0; i < t.size();) {
        const char32_t cp = text::decode(t, i);
        cs.at(x++, y) = Cell{cp, Style{0, 0, a}, 1};
      }
    };
    put(2, 0, " ", attr::kDecor);
    put(3, 0, "x = f(a,", 0);
    put(2, 1, "\xE2\x86\xAA", attr::kDecor | attr::kJoin);
    put(3, 1, "  ", attr::kDecor);
    put(5, 1, " b)", 0);
    put(2, 2, " ", attr::kDecor);
    put(3, 2, "y = 1", 0);
    check(selection_text(cs, Point{0, 0}, Point{19, 2}, Rect{0, 0, 20, 3}) == "  x = f(a, b)\n  y = 1",
          "code: copying joins a wrapped line and leaves the marks out");
  }

  // Links: found in text, kept by markdown, stamped on cells, written as OSC 8.
  {
    const auto urls_in = [](std::string_view s) {
      std::vector<std::pair<size_t, size_t>> r;
      md::find_urls(s, r);
      std::vector<std::string> out;
      for (auto [a, b] : r) out.emplace_back(s.substr(a, b - a));
      return out;
    };
    check(urls_in("see https://x.org/a).") == std::vector<std::string>{"https://x.org/a"},
          "links: a URL leaves out the bracket and full stop after it");
    check(urls_in("(https://en.wikipedia.org/wiki/Foo_(bar))") ==
              std::vector<std::string>{"https://en.wikipedia.org/wiki/Foo_(bar)"},
          "links: a URL keeps the brackets it opened");
    check(urls_in("go to www.example.com, or abcwww.no") == std::vector<std::string>{"www.example.com"},
          "links: www. starts a word, and loses its comma");
    check(urls_in("server at http://localhost:3000/api ready") ==
              std::vector<std::string>{"http://localhost:3000/api"},
          "links: localhost with a port");
    check(md::url_target("www.a.org") == "https://www.a.org", "links: www. opens over https");
    check(links::intern("javascript:alert(1)") == 0 && links::intern("https://a b") == 0 &&
              links::intern("https://ok.example/x") != 0 &&
              links::intern("https://ok.example/x") == links::intern("https://ok.example/x"),
          "links: only openable URLs, each interned once");

    Arena sc;
    std::vector<md::Seg> segs;
    std::vector<md::Line> lines;
    std::vector<text::Span> spans;
    std::vector<md::Seg> inl;
    md::Out o{size_t(-1), &sc, &segs, &lines, &spans, &inl};
    const std::string msg =
        "Read [the docs](https://docs.example/x \"t\") or https://bare.example/y, and `https://code.example/z`.\n"
        "```py\nurl = \"https://block.example/q\"\n```\n";
    md::render(msg, 0, false, 200, o);
    const auto link_of = [&](std::string_view want) -> std::pair<md::Ink, std::string> {
      for (const auto& sg : segs) {
        if (!sg.link) continue;
        const std::string t = (sg.off & md::kScratchBit) ? std::string(sc.view(Str{sg.off & ~md::kScratchBit, sg.len}))
                                                        : msg.substr(sg.off, sg.len);
        if (t == want) return {sg.ink, std::string(links::url(sg.link))};
      }
      return {md::Ink::Text, ""};
    };
    check(link_of("the docs") == std::pair<md::Ink, std::string>{md::Ink::Link, "https://docs.example/x"},
          "links: a markdown link shows its label and opens its target");
    check(link_of("https://bare.example/y").first == md::Ink::Link &&
              link_of("https://bare.example/y").second == "https://bare.example/y",
          "links: a bare URL in prose is a link");
    check(link_of("https://code.example/z").first == md::Ink::Code, "links: in inline code it stays code, linked");
    check(link_of("https://block.example/q").second == "https://block.example/q",
          "links: in a code block too");

    Surface back;
    back.resize(10, 1);
    const uint16_t id = links::intern("https://ok.example/x");
    for (int x = 2; x < 5; x++) back.at(x, 0) = Cell{U'a', Style{}, 1, id};
    Surface front;
    std::string out1, out2;
    encode_frame(back, front, out1, true);
    encode_frame(back, front, out2, false);
    const std::string open = "\x1b]8;id=" + std::to_string(id) + ";https://ok.example/x\x1b\\";
    check(out1.find(open) != std::string::npos && out1.find("\x1b]8;;\x1b\\") > out1.find(open) && out2.empty(),
          "links: OSC 8 around a link's cells, closed after, nothing for an unchanged frame");
  }

  // Clicking a link in a chat: the renderer stamps link ids on the cells it
  // draws, and a click on one (press and release on it) hands back its URL.
  {
    const std::string path = "/tmp/mico_link_chat.jsonl";
    put_file(path,
             R"({"type":"session_meta","payload":{"id":"l1","cwd":"/tmp"}})" "\n"
             R"({"type":"response_item","payload":{"type":"message","role":"assistant","channel":"final","content":[{"text":"See [the PR](https://example.org/pr/7) now."}]}})" "\n");
    ChatRenderer c;
    check(c.open(path, &codex_adapter()), "links: a chat with a link opens");
    Theme theme;
    Filters filters;
    Surface sf;
    sf.resize(60, 8);
    {
      Painter p(sf, Rect{0, 0, 60, 8});
      c.render(p, theme, filters);
    }
    Point at{-1, -1};
    for (int y = 0; y < 8 && at.x < 0; y++)
      for (int x = 0; x < 60; x++)
        if (sf.at(x, y).link) { at = Point{x + 1, y}; break; }
    check(at.x >= 0 && links::url(sf.at(at.x, at.y).link) == "https://example.org/pr/7",
          "links: the label's cells carry the link");
    MouseEvent press{MouseKind::Press, MouseButton::Left, at};
    MouseEvent release{MouseKind::Release, MouseButton::Left, at};
    c.on_mouse(press, at);
    c.on_mouse(release, at);
    check(c.take_url() == "https://example.org/pr/7" && c.take_url().empty(),
          "links: a click on one gives its URL, once");
    c.on_mouse(press, at);
    c.on_mouse(MouseEvent{MouseKind::Release, MouseButton::Left, Point{0, 7}}, Point{0, 7});
    check(c.take_url().empty(), "links: pressing on one and letting go elsewhere opens nothing");
  }

  // Render settings: saved as "name way" lines, unknown names and ways
  // ignored, missing ones at their defaults, and the old "on"/"off" read as
  // they meant.
  {
    char tmpl[] = "/tmp/mico_settings_XXXXXX";
    const char* dir = mkdtemp(tmpl);
    const char* old_home = getenv("XDG_CONFIG_HOME");
    const std::string keep = old_home ? old_home : "";
    setenv("XDG_CONFIG_HOME", dir, 1);
    reload_render_settings();
    const RenderSettings defaults = render_settings();
    RenderSettings s = defaults;
    s.way[kEquations] = uint8_t(Equations::Source);
    s.way[kLinks] = uint8_t(Links::Urls);
    s.way[kPictures] = uint8_t(Pictures::Large);
    s.theme = "warm";
    const uint64_t g = render_settings_generation();
    set_render_settings(s);
    check(render_settings_generation() != g, "settings: a change bumps the generation");
    reload_render_settings();
    check(render_settings().equations() == Equations::Source && render_settings().links() == Links::Urls &&
              render_settings().pictures() == Pictures::Large && render_settings().picture_rows() == 48 &&
              render_settings().charts() == Charts::Pictures && render_settings().theme == "warm",
          "settings: every way and the theme saved and read back");
    check(active_theme().bg == themes()[3].theme.bg, "settings: the theme named is the one in use");
    put_file(config_dir() + "/render", "math off\ncharts on\nlinks sideways\nbogus on\n");
    reload_render_settings();
    check(render_settings().equations() == Equations::Source && render_settings().charts() == Charts::Pictures &&
              render_settings().links() == Links::UrlsAndPaths,
          "settings: old on/off files still read; unknown names and ways keep the default");
    check(render_settings().theme == "dark" && active_theme().bg == Theme{}.bg, "settings: no theme named, the default");
    // One-line settings: trimmed when read, written whole, on/off with a fallback.
    check(read_setting("no-such-setting").empty() && setting_on("no-such-setting", true) &&
              !setting_on("no-such-setting", false),
          "settings: one not set reads empty and falls back");
    put_file(config_dir() + "/word", "  bell \nsecond line\n");
    check(read_setting("word") == "bell", "settings: a setting is its first line, trimmed");
    check(write_setting("word", "desktop") && read_setting("word") == "desktop", "settings: written and read back");
    set_setting_on("switch", false);
    check(!setting_on("switch", true), "settings: off, whatever the fallback");
    set_setting_on("switch", true);
    check(setting_on("switch", false), "settings: on, whatever the fallback");
    struct stat st{};
    check(stat((config_dir() + "/switch").c_str(), &st) == 0 && (st.st_mode & 077) == 0,
          "settings: kept for the user alone");
    bool stray = false;
    fs::list_dir(config_dir(), false, [&](const std::string& n) { stray |= n.find(".tmp") != std::string::npos; });
    check(!stray, "settings: no temporary file left behind");
    if (old_home) setenv("XDG_CONFIG_HOME", keep.c_str(), 1);
    else unsetenv("XDG_CONFIG_HOME");
    set_render_settings(defaults, false);
    check(defaults == RenderSettings{}, "settings: the tests run with every part at its default");
  }

  // Images, file links and tool output.
  {
    // A 2x1 PNG (red, transparent), made by hand: signature, IHDR, IDAT, IEND.
    const std::string png_b64 =
        "iVBORw0KGgoAAAANSUhEUgAAAAIAAAABCAYAAAD0In+KAAAAEUlEQVR4nGP4z8DwnwGIGBgAHvEC/k7Hw9EAAAAASUVORK5CYII=";
    std::string bytes;
    check(math::base64_decode(png_b64, bytes) && bytes.compare(1, 3, "PNG") == 0, "images: base64 decodes");
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    check(math::decode_image(bytes, rgba, &w, &h) && w == 2 && h == 1 && rgba.size() == 8 && rgba[0] == 255 &&
              rgba[3] == 255 && rgba[7] == 0,
          "images: a PNG decodes, in a child process");
    std::string broken = bytes;
    broken.resize(bytes.size() / 2);
    check(!math::decode_image(broken, rgba, &w, &h) && !math::decode_image("not an image", rgba, &w, &h),
          "images: a truncated or bogus image fails, and nothing else does");

    const std::string claude_line = R"({"type":"user","message":{"content":[{"type":"tool_result","content":[{"type":"text","text":"shot"},{"type":"image","source":{"type":"base64","media_type":"image/png","data":")" + png_b64 + R"("}}]}]}})";
    const std::string codex_line = R"({"content":[{"type":"input_image","image_url":"data:image/jpeg;base64,)" + png_b64 + R"("}]})";
    const std::string pi_line = R"({"content":[{"type":"image","data":")" + png_b64 + R"(","mimeType":"image/webp"}]})";
    std::string media;
    std::string_view data;
    check(count_images(claude_line) == 1 && transcript_image(claude_line, 0, &media, &data) && media == "image/png" &&
              data == png_b64,
          "images: claude's image blocks are found");
    check(transcript_image(codex_line, 0, &media, &data) && media == "image/jpeg" && data == png_b64,
          "images: codex's data URLs are found");
    check(transcript_image(pi_line, 0, &media, &data) && media == "image/webp", "images: pi's image objects are found");
    check(count_images(R"({"text":"a message saying \"type\":\"image\" in prose"})") == 0,
          "images: text that mentions an image block is not one");

    math::configure(math::Config{true, 10, 20, 0x56B6C2});
    const math::Image* pic = math::picture("test:png", [&](std::string& b) { b = bytes; return true; }, 40, 10, "[image]");
    check(pic && pic->cols == 1 && pic->rows == 1 && pic->w == 10 && pic->h == 20 && pic->rgba.empty(),
          "images: a picture is laid out from its size alone, fitted to whole cells, not enlarged");
    check(pic && math::pixels(*pic) && pic->rgba.size() == 10 * 20 * 4 && pic->rgba[0] == 255,
          "images: its pixels are decoded when a terminal needs them");
    {
      std::string round;
      math::base64_encode(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), round);
      std::string back;
      check(round == png_b64 && math::base64_decode("aGk\\/\n", back) && back == "hi?" &&
                !math::base64_decode("aGk*", back),
            "images: base64 goes both ways; escapes and whitespace skipped, other bytes refused");
    }
    {
      // kitty takes a PNG as it is, at once: nothing decoded here.
      const math::Image* fresh = math::picture("test:png-wire", [&](std::string& b) { b = bytes; return true; }, 40, 10, "[image]");
      check(fresh && math::kitty_wire(*fresh) && fresh->wire_png && fresh->rgba.empty() && fresh->wire == png_b64,
            "images: kitty is sent a PNG as it is, without decoding it");
      Surface one;
      one.resize(4, 1);
      one.at(0, 0) = Cell{0, Style{Color(fresh->id), kDefaultColor, attr::kImage}, 1};
      math::KittyHeld held;
      held.images[0xABCDE] = math::KittyHeld::Entry{300u << 20, 0};  // drawn long ago, and big
      held.bytes = 300u << 20;
      std::string out;
      math::send_images(one, held, out);
      check(out.find("f=100") != std::string::npos && out.find("U=1") != std::string::npos && held.has(fresh->id),
            "images: and placed in its cells like any other");
      check(!held.has(0xABCDE) && out.find("a=d,d=I,i=703710") != std::string::npos,
            "images: past a terminal's budget, what it showed longest ago is freed from it");

      // Any other format is decoded off the UI thread: the frame goes out
      // without it, and a later one sends it.
      std::string bmp(54, '\0');
      const auto le32 = [&](size_t at, uint32_t v) { for (int i = 0; i < 4; i++) bmp[at + size_t(i)] = char(v >> (8 * i)); };
      bmp[0] = 'B', bmp[1] = 'M';
      le32(10, 54), le32(14, 40), le32(18, 2), le32(22, 1);
      bmp[26] = 1, bmp[28] = 24;
      bmp += std::string("\x00\x00\xFF\x00\xFF\x00\x00\x00", 8);  // red, green; padded to four bytes
      le32(2, uint32_t(bmp.size()));
      const math::Image* other = math::picture("test:bmp", [&](std::string& b) { b = bmp; return true; }, 40, 10, "[image]");
      check(other && !math::kitty_wire(*other) && math::preparing(), "images: another format is prepared on a worker");
      one.at(0, 0) = Cell{0, Style{Color(other->id), kDefaultColor, attr::kImage}, 1};
      std::string before;
      math::send_images(one, held, before);
      check(before.find("a=t") == std::string::npos && !held.has(other->id), "images: no frame waits for it");
      bool got = false;
      for (int i = 0; i < 500 && !got; i++) {
        got = math::collect_prepared();
        if (!got) std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      std::string after;
      math::send_images(one, held, after);
      check(got && !other->wire_png && other->rgba.size() == size_t(other->w) * size_t(other->h) * 4 &&
                other->rgba[0] == 255 && other->rgba[1] == 0 && other->rgba[4] == 0 && other->rgba[5] == 255 &&
                after.find("f=32") != std::string::npos && held.has(other->id) && !math::preparing(),
            "images: once ready, the next frame sends its pixels");
    }
    {
      // A chat full of big screenshots: laying them all out drops nothing,
      // so no chat has to lay itself out again; decoded, only the ones used
      // last keep their pixels.
      math::Image big;
      big.w = 1600, big.h = 900;
      big.rgba.assign(size_t(big.w) * size_t(big.h) * 4, 0);
      for (size_t i = 0; i < big.rgba.size(); i += 4) big.rgba[i] = 200, big.rgba[i + 3] = 255;
      const std::string png = math::png(big, 0);
      const uint64_t gen0 = math::generation();
      std::vector<const math::Image*> shots;
      for (int k = 0; k < 24; k++)
        shots.push_back(math::picture("test:shot" + std::to_string(k), [&](std::string& b) { b = png; return true; },
                                      200, 48, "[image]"));
      bool laid_out = true;
      for (const auto* im : shots) laid_out &= im && im->rgba.empty() && im->cols == 160;
      check(laid_out && math::generation() == gen0, "images: many big pictures lay out without decoding or dropping any");
      bool decoded = true;
      for (const auto* im : shots) decoded &= math::pixels(*im);
      size_t held = 0;
      bool ids = true;
      for (const auto* im : shots) {
        held += im->rgba.size();
        ids &= math::find(im->id) == im;
      }
      check(decoded && ids && math::generation() == gen0 && held <= (64u << 20) && !shots.back()->rgba.empty() &&
                shots.front()->rgba.empty(),
            "images: past the budget the pictures used longest ago give back their pixels, keeping their ids");
      check(math::pixels(*shots.front()) && shots.front()->rgba.size() == size_t(1600) * 900 * 4,
            "images: and get them back when needed again");
    }

    // Pictures are their own setting: on, a screenshot inside a finished
    // (folded) turn shows at normal density; off, it does not.
    {
      const std::string path = "/tmp/mico_folded_picture.jsonl";
      put_file(path,
               R"({"type":"user","message":{"role":"user","content":"look"}})" "\n"
               R"({"type":"assistant","message":{"role":"assistant","content":[{"type":"tool_use","id":"s1","name":"mcp__browser__shot","input":{"page":"x"}}]}})" "\n"
               R"({"type":"user","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"s1","content":[{"type":"text","text":"shot"},{"type":"image","source":{"type":"base64","media_type":"image/png","data":")" +
                   png_b64 + R"("}}]}]}})" "\n"
               R"({"type":"assistant","message":{"role":"assistant","content":[{"type":"text","text":"It looks fine."}]}})" "\n"
               R"({"type":"user","message":{"role":"user","content":"thanks"}})" "\n");
      const auto image_cells = [&] {
        ChatRenderer c;
        c.open(path, &claude_adapter());
        Surface sf;
        sf.resize(60, 20);
        Theme theme;
        Filters filters;  // normal density: the first turn's steps are folded
        Painter p(sf, Rect{0, 0, 60, 20});
        c.render(p, theme, filters);
        int n = 0;
        for (int y = 0; y < 20; y++)
          for (int x = 0; x < 60; x++) n += (sf.at(x, y).st.a & attr::kImage) != 0;
        return n;
      };
      const RenderSettings saved = render_settings();
      RenderSettings on = saved, off = saved, small = saved;
      on.way[kPictures] = uint8_t(Pictures::Large);
      off.way[kPictures] = uint8_t(Pictures::Off);
      small.way[kPictures] = uint8_t(Pictures::Small);
      set_render_settings(on, false);
      const int shown = image_cells();
      set_render_settings(small, false);
      const int shown_small = image_cells();
      set_render_settings(off, false);
      const int hidden = image_cells();
      set_render_settings(saved, false);
      check(shown_small > 0 && shown_small <= shown, "pictures: small draws no more than large");
      check(shown > 0 && hidden == 0, "pictures: a separate setting from density, shown inside a folded turn");
      unlink(path.c_str());
    }
    check(!math::picture("test:bad", [](std::string& b) { b = "junk"; return true; }, 40, 10, "x"),
          "images: what does not decode is not drawn");

    // File paths as links: only files that exist, with their line.
    char tmpl[] = "/tmp/mico_paths_XXXXXX";
    const std::string dir = mkdtemp(tmpl) ? std::string(tmpl) : std::string("/tmp");
    mkdir((dir + "/src").c_str(), 0700);
    put_file(dir + "/src/app.py", "x = 1\n");
    std::vector<md::LinkHit> hits;
    md::find_links("see src/app.py:12, not src/gone.py, and https://a.example/x.", dir, hits);
    check(hits.size() == 2 && hits[0].target == "file://" + dir + "/src/app.py#L12" &&
              hits[1].target == "https://a.example/x",
          "links: an existing file path links to its line; a missing one does not");
    md::find_links("version 1.2.3 and e.g. this", dir, hits);
    check(hits.empty(), "links: version numbers and abbreviations are not paths");

    // Tool output: colours kept, redraws settled, a listing coloured.
    Arena sc;
    std::vector<md::Seg> segs;
    std::vector<md::Line> lines;
    std::vector<text::Span> spans;
    std::vector<md::Seg> inl;
    md::ChartEnv env;
    env.base_dir = dir;
    md::Out o{size_t(-1), &sc, &segs, &lines, &spans, &inl};
    o.charts = &env;
    const std::string out_text = "\x1b[1;31mFAIL\x1b[0m ok \x1b[38;5;46mgreen\x1b[0m\n10%\r55%\rdone\nsrc/app.py:3\n";
    md::render_output(out_text, 0, false, 60, o, nullptr);
    const auto seg_text = [&](const md::Seg& sg) {
      return (sg.off & md::kScratchBit) ? std::string(sc.view(Str{sg.off & ~md::kScratchBit, sg.len}))
                                        : out_text.substr(sg.off, sg.len);
    };
    bool red_bold = false, green = false, settled = false, path_link = false;
    for (const auto& L : lines) {
      std::string row;
      for (uint16_t k = 0; k < L.seg_count; k++) {
        const md::Seg& sg = segs[L.seg_first + k];
        const std::string t = seg_text(sg);
        row += t;
        red_bold |= t == "FAIL" && sg.ink == md::Ink::Ansi1 && (sg.attr & md::kAttrBold);
        green |= t == "green" && sg.ink == md::Ink::Text && md::paint(sg.paint).fg == 0x00FF00;
        path_link |= t == "src/app.py:3" && sg.link && links::url(sg.link) == "file://" + dir + "/src/app.py#L3";
      }
      settled |= row == "done";
    }
    check(red_bold && green, "output: ANSI colours and bold are kept, a 256-colour one exactly");
    check(settled, "output: a carriage-return redraw shows only its last state");
    check(path_link, "output: a file path in output is a link");
    check(lines.size() == 3, "output: one row per line, no escape bytes left as text");

    segs.clear();
    lines.clear();
    const std::string painted =
        "\x1b[38;2;255;100;0mHOT\x1b[48;5;196mON\x1b[39;44mBLUE\x1b[49;38;5;3mOLIVE\x1b[0m plain\n";
    md::render_output(painted, 0, false, 60, o, nullptr);
    std::map<std::string, md::Seg> by_text;
    for (const auto& sg : segs) by_text[seg_text(sg)] = sg;
    const auto paint_of = [&](const char* t) { return md::paint(by_text[t].paint); };
    check(paint_of("HOT").fg == 0xFF6400 && paint_of("HOT").bg == kDefaultColor &&
              paint_of("ON").fg == 0xFF6400 && paint_of("ON").bg == 0xFF0000,
          "output: 24-bit and 256-colour foregrounds and backgrounds are kept exactly");
    check(by_text["BLUE"].ink == md::Ink::Text && paint_of("BLUE").fg == kDefaultColor &&
              paint_of("BLUE").bg == md::palette(4) && by_text["OLIVE"].ink == md::Ink::Ansi3 &&
              !by_text["OLIVE"].paint && !by_text[" plain"].paint,
          "output: palette colours stay the theme's, and resets clear what they name");

    segs.clear();
    lines.clear();
    md::render_output("     1\xE2\x86\x92" "def f():\n     2\xE2\x86\x92" "    return 1\n", 0, false, 60, o, code::lang_of("py"));
    bool number = false, keyword = false;
    for (const auto& sg : segs) {
      number |= sg.ink == md::Ink::CodeMark;
      keyword |= sg.ink == md::Ink::CodeKeyword;
    }
    check(number && keyword && lines.size() == 2, "output: a Read listing is coloured, its numbers set apart");
    math::configure(math::Config{false, 10, 20, 0x56B6C2});
  }

  g_fail += run_regression_tests();
  printf("\n  %s\n", g_fail == 0 ? "all checks passed" : "FAILURES ABOVE");
  return g_fail == 0 ? 0 : 1;
}

}  // namespace mico
