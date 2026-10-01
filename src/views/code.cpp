#include "views/code.h"

#include "core/settings.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <string>

namespace mico::code {

enum : uint32_t {
  kSingleQuote = 1u << 0,   // '…' is a string
  kCharLiteral = 1u << 1,   // '…' is a short character literal
  kBacktick = 1u << 2,      // `…` is a string, and may span lines
  kTriple = 1u << 3,        // """…""" and '''…''', spanning lines; f"…" prefixes
  kPreproc = 1u << 4,       // # directives
  kDecorators = 1u << 5,    // @name
  kMacroBang = 1u << 6,     // name!(…)
  kLifetime = 1u << 7,      // 'a
  kShellVars = 1u << 8,     // $x ${x} $(…)
  kHashBoundary = 1u << 9,  // # starts a comment only after a space
  kCapsTypes = 1u << 10,    // Capitalized names are types, ALL_CAPS ones constants
  kKeys = 1u << 11,         // name: and "name": are keys
  kIniKeys = 1u << 12,      // name = is a key, [section] a heading
  kMarkup = 1u << 13,       // <tag attr="…">
  kNoCase = 1u << 14,       // keywords in any case (SQL)
  kDiff = 1u << 15,         // + - @@ lines
  kFirstWord = 1u << 16,    // an UPPERCASE first word is the keyword (Dockerfile)
  kRustAttr = 1u << 17,     // #[…]
  kHexColor = 1u << 18,     // #fff (CSS)
  kDashIdents = 1u << 19,   // names may contain '-' (CSS, YAML)
};

struct Lang {
  std::string_view name;
  std::string_view aliases;  // space separated, lower case
  std::string_view keywords;
  std::string_view types;
  std::string_view comment1, comment2;  // line comments
  std::string_view block_open, block_close;
  uint32_t flags;
};

namespace {

constexpr std::string_view kCKeywords =
    "alignas alignof and asm auto break case catch class const consteval constexpr constinit "
    "const_cast continue co_await co_return co_yield decltype default delete do dynamic_cast else "
    "enum explicit export extern false final for friend goto if inline mutable namespace new "
    "noexcept not nullptr operator or override private protected public register reinterpret_cast "
    "requires return sizeof static static_assert static_cast struct switch template this "
    "thread_local throw true try typedef typeid typename union using virtual volatile while NULL";
constexpr std::string_view kCTypes =
    "bool char char8_t char16_t char32_t double float int long short signed unsigned void wchar_t "
    "size_t ssize_t ptrdiff_t intptr_t uintptr_t int8_t int16_t int32_t int64_t uint8_t uint16_t "
    "uint32_t uint64_t std string string_view vector map unordered_map set array optional "
    "unique_ptr shared_ptr FILE";

constexpr Lang kLangs[] = {
    {"c++", "c cpp c++ cc cxx h hpp hh hxx h++ objc objective-c cuda cu arduino ino glsl hlsl metal",
     kCKeywords, kCTypes, "//", "", "/*", "*/", kCharLiteral | kPreproc | kCapsTypes},
    {"rust", "rust rs",
     "as async await break const continue crate dyn else enum extern false fn for if impl in let "
     "loop match mod move mut pub ref return self Self static struct super trait true type unsafe "
     "use where while",
     "i8 i16 i32 i64 i128 isize u8 u16 u32 u64 u128 usize f32 f64 bool char str String Vec "
     "Option Result Box Some None Ok Err",
     "//", "", "/*", "*/", kCharLiteral | kLifetime | kMacroBang | kCapsTypes | kRustAttr},
    {"go", "go golang",
     "break case chan const continue default defer else fallthrough for func go goto if import "
     "interface map package range return select struct switch type var true false nil iota",
     "bool byte complex64 complex128 error float32 float64 int int8 int16 int32 int64 rune string "
     "uint uint8 uint16 uint32 uint64 uintptr any",
     "//", "", "/*", "*/", kCharLiteral | kBacktick | kCapsTypes},
    {"typescript", "js javascript jsx mjs cjs ts typescript tsx node deno",
     "abstract as async await break case catch class const continue debugger declare default "
     "delete do else enum export extends false finally for from function get if implements import "
     "in instanceof interface keyof let new null of private protected public readonly return set "
     "static super switch this throw true try type typeof undefined var void while with yield",
     "string number boolean any unknown never object symbol bigint Array Promise Record Map Set "
     "Partial Readonly",
     "//", "", "/*", "*/", kSingleQuote | kBacktick | kCapsTypes | kDecorators},
    {"java", "java kotlin kt kts cs csharp c# scala swift dart groovy gradle",
     "abstract as break case catch class companion const continue data default do else enum "
     "extends false final finally for fun func guard if implements import in init interface "
     "internal is let namespace new null nil object open override package private protected public "
     "return sealed static struct super switch this throw throws true try typealias using val var "
     "void when where while async await defer extension get set lazy",
     "int long short byte char boolean bool float double String Int Long Double Float Boolean "
     "Unit Any Object List Map",
     "//", "", "/*", "*/", kCharLiteral | kCapsTypes | kDecorators},
    {"python", "py python python3 py3 pyi pyw sage ipython",
     "and as assert async await break class continue def del elif else except False finally for "
     "from global if import in is lambda None nonlocal not or pass raise return True try while "
     "with yield match case self cls",
     "int str float bool list dict set tuple bytes object type Exception ValueError KeyError "
     "TypeError RuntimeError print len range open super isinstance enumerate zip map filter sorted "
     "min max sum abs any all",
     "#", "", "", "", kSingleQuote | kTriple | kDecorators | kCapsTypes},
    {"shell", "sh bash zsh shell console fish ksh shellscript terminal",
     "if then else elif fi for while until do done case esac in function return local export "
     "readonly declare unset shift break continue exit source alias set eval exec trap sudo",
     "", "#", "", "", "", kSingleQuote | kShellVars | kHashBoundary},
    {"ruby", "rb ruby rake gemspec",
     "alias and begin break case class def defined? do else elsif end ensure false for if in "
     "module next nil not or redo rescue retry return self super then true undef unless until when "
     "while yield require require_relative attr_accessor attr_reader puts",
     "", "#", "", "", "", kSingleQuote | kCapsTypes},
    {"lua", "lua luau",
     "and break do else elseif end false for function goto if in local nil not or repeat return "
     "then true until while",
     "", "--", "", "--[[", "]]", kSingleQuote},
    {"sql", "sql psql postgres postgresql mysql sqlite plsql tsql",
     "select from where and or not insert into values update set delete create table index view "
     "drop alter add column join left right inner outer full cross on as group by order having "
     "limit offset union all distinct case when then else end null is in like ilike between "
     "exists primary key foreign references default unique check begin commit rollback "
     "transaction with returning asc desc if replace",
     "int integer bigint smallint text varchar char boolean bool real float double numeric "
     "decimal date time timestamp timestamptz json jsonb uuid serial blob count sum avg min max",
     "--", "", "/*", "*/", kSingleQuote | kNoCase},
    {"json", "json jsonc json5 jsonl ndjson geojson", "true false null", "", "//", "", "/*", "*/",
     kKeys},
    {"yaml", "yaml yml", "true false null yes no on off True False Null", "", "#", "", "", "",
     kSingleQuote | kKeys | kHashBoundary | kDashIdents},
    {"toml", "toml ini cfg conf properties env dotenv editorconfig gitconfig",
     "true false", "", "#", ";", "", "", kSingleQuote | kIniKeys},
    {"html", "html htm xml svg xhtml vue svelte plist xaml", "", "", "", "", "<!--", "-->",
     kSingleQuote | kMarkup},
    {"css", "css scss sass less", "important", "", "", "", "/*", "*/",
     kSingleQuote | kKeys | kHexColor | kDashIdents},
    {"dockerfile", "dockerfile docker containerfile", "", "", "#", "", "", "",
     kSingleQuote | kShellVars | kFirstWord},
    {"make", "make makefile mk cmake", "if else endif ifeq ifneq ifdef ifndef include define endef "
     "export override set function endfunction foreach endforeach add_executable target_link_libraries",
     "", "#", "", "", "", kShellVars | kKeys},
    {"diff", "diff patch udiff", "", "", "", "", "", "", kDiff},
};

std::string lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = char(std::tolower(uint8_t(c)));
  return out;
}

bool in_list(std::string_view list, std::string_view word, bool nocase) {
  if (word.empty() || list.empty()) return false;
  const std::string w = nocase ? lower(word) : std::string(word);
  size_t i = 0;
  while (i < list.size()) {
    size_t j = list.find(' ', i);
    if (j == std::string_view::npos) j = list.size();
    if (list.substr(i, j - i) == w) return true;
    i = j + 1;
  }
  return false;
}

bool ident_start(char c) { return std::isalpha(uint8_t(c)) || c == '_' || uint8_t(c) >= 0x80; }
bool ident_char(char c, bool dash) {
  return std::isalnum(uint8_t(c)) || c == '_' || uint8_t(c) >= 0x80 || (dash && c == '-');
}

// The end of a string opened by `q` at `i` (past its closing quote), or npos.
size_t string_end(std::string_view s, size_t i, char q) {
  for (size_t j = i + 1; j < s.size(); j++) {
    if (s[j] == '\\') { j++; continue; }
    if (s[j] == q) return j + 1;
  }
  return std::string_view::npos;
}

struct Out {
  std::vector<Run>& runs;
  void push(size_t off, size_t len, Tok t) {
    if (!len) return;
    if (!runs.empty() && runs.back().tok == t && runs.back().off + runs.back().len == off) {
      runs.back().len += uint32_t(len);
      return;
    }
    runs.push_back(Run{uint32_t(off), uint32_t(len), t});
  }
};

size_t skip_spaces(std::string_view s, size_t i) {
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
  return i;
}

}  // namespace

const Lang* lang_of(std::string_view fence) {
  // "python title=x", "{.rust}", "c++": the first word, any decoration off.
  while (!fence.empty() && (fence.front() == '{' || fence.front() == '.' || fence.front() == ' ')) fence.remove_prefix(1);
  size_t end = 0;
  while (end < fence.size() && fence[end] != ' ' && fence[end] != '}' && fence[end] != ',' && fence[end] != ':') end++;
  const std::string word = lower(fence.substr(0, end));
  if (word.empty()) return nullptr;
  for (const Lang& l : kLangs)
    if (in_list(l.aliases, word, false)) return &l;
  return nullptr;
}

std::string_view name_of(const Lang* l) { return l ? l->name : std::string_view(); }

void highlight(std::string_view s, const Lang* L, State& st, std::vector<Run>& runs) {
  runs.clear();
  Out out{runs};
  const size_t n = s.size();
  if (!L || render_settings().code_colours() == CodeColours::Off) {
    out.push(0, n, Tok::Text);
    return;
  }
  const uint32_t f = L->flags;
  if (f & kDiff) {
    const Tok t = s.starts_with("+++") || s.starts_with("---") || s.starts_with("@@") ? Tok::Hunk
                  : s.starts_with("+") ? Tok::Added
                  : s.starts_with("-") ? Tok::Removed
                  : Tok::Text;
    out.push(0, n, t);
    return;
  }

  size_t i = 0;
  // A block comment or string left open by an earlier line.
  if (st.open) {
    const std::string_view close = st.open == 1 ? L->block_close
                                   : st.open == 2 ? std::string_view("\"\"\"")
                                   : st.open == 3 ? std::string_view("'''")
                                                  : std::string_view("`");
    size_t end = std::string_view::npos;
    if (st.open == 4) {
      for (size_t j = 0; j < n; j++) {
        if (s[j] == '\\') { j++; continue; }
        if (s[j] == '`') { end = j; break; }
      }
    } else {
      end = s.find(close);
    }
    const Tok t = st.open == 1 ? Tok::Comment : Tok::String;
    if (end == std::string_view::npos) {
      out.push(0, n, t);
      return;
    }
    i = end + close.size();
    out.push(0, i, t);
    st.open = 0;
  }

  const bool dash = f & kDashIdents;
  const size_t first = skip_spaces(s, i);
  // Whole-line forms.
  if ((f & kIniKeys) && first < n && s[first] == '[') {
    out.push(i, n - i, Tok::Type);
    return;
  }
  if ((f & kPreproc) && first < n && s[first] == '#') {
    out.push(i, first - i, Tok::Text);
    size_t j = skip_spaces(s, first + 1);
    while (j < n && ident_char(s[j], false)) j++;
    out.push(first, j - first, Tok::Keyword);
    const size_t k = skip_spaces(s, j);
    if (k < n && s[k] == '<') {  // #include <header>
      const size_t close = s.find('>', k);
      out.push(j, k - j, Tok::Text);
      const size_t e = close == std::string_view::npos ? n : close + 1;
      out.push(k, e - k, Tok::String);
      j = e;
    }
    i = j;
  }
  if ((f & kFirstWord) && first < n && std::isupper(uint8_t(s[first]))) {
    size_t j = first;
    while (j < n && std::isupper(uint8_t(s[j]))) j++;
    if (j == n || s[j] == ' ') {
      out.push(i, first - i, Tok::Text);
      out.push(first, j - first, Tok::Keyword);
      i = j;
    }
  }

  size_t str_from = std::string_view::npos;  // a string prefix (f"…") seen just before
  while (i < n) {
    const char c = s[i];
    if (c == ' ' || c == '\t') {
      const size_t j = skip_spaces(s, i);
      out.push(i, j - i, Tok::Text);
      i = j;
      continue;
    }
    // Comments. A block opener is checked first: Lua's --[[ starts with --.
    if (!L->block_open.empty() && s.compare(i, L->block_open.size(), L->block_open) == 0) {
      const size_t end = s.find(L->block_close, i + L->block_open.size());
      if (end == std::string_view::npos) {
        out.push(i, n - i, Tok::Comment);
        st.open = 1;
        return;
      }
      const size_t e = end + L->block_close.size();
      out.push(i, e - i, Tok::Comment);
      i = e;
      continue;
    }
    bool comment = false;
    for (std::string_view lc : {L->comment1, L->comment2})
      if (!lc.empty() && s.compare(i, lc.size(), lc) == 0 &&
          (lc != "#" || !(f & kHashBoundary) || i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t'))
        comment = true;
    if (comment) {
      out.push(i, n - i, Tok::Comment);
      return;
    }
    if ((f & kRustAttr) && c == '#' && i + 1 < n && (s[i + 1] == '[' || s[i + 1] == '!')) {
      const size_t close = s.find(']', i);
      const size_t e = close == std::string_view::npos ? n : close + 1;
      out.push(i, e - i, Tok::Func);
      i = e;
      continue;
    }
    if ((f & kHexColor) && c == '#' && i + 1 < n && std::isxdigit(uint8_t(s[i + 1]))) {
      size_t j = i + 1;
      while (j < n && std::isalnum(uint8_t(s[j]))) j++;
      out.push(i, j - i, Tok::Number);
      i = j;
      continue;
    }

    // Strings.
    const size_t sstart = str_from != std::string_view::npos ? str_from : i;
    str_from = std::string_view::npos;
    if ((f & kTriple) && (s.compare(i, 3, "\"\"\"") == 0 || s.compare(i, 3, "'''") == 0)) {
      const size_t end = s.find(s.substr(i, 3), i + 3);
      if (end == std::string_view::npos) {
        out.push(sstart, n - sstart, Tok::String);
        st.open = c == '"' ? 2 : 3;
        return;
      }
      out.push(sstart, end + 3 - sstart, Tok::String);
      i = end + 3;
      continue;
    }
    const bool quote = c == '"' || (c == '\'' && (f & kSingleQuote)) || (c == '`' && (f & kBacktick));
    if (quote) {
      size_t e = string_end(s, i, c);
      if (e == std::string_view::npos) {
        e = n;
        if (c == '`') st.open = 4;  // a template literal, a raw string: it goes on
      }
      Tok t = Tok::String;
      if (f & kKeys) {  // "key": in JSON, YAML
        const size_t k = skip_spaces(s, e);
        if (k < n && s[k] == ':') t = Tok::Type;
      }
      out.push(sstart, e - sstart, t);
      i = e;
      continue;
    }
    if (c == '\'' && (f & kCharLiteral)) {
      if ((f & kLifetime) && i + 1 < n && ident_start(s[i + 1]) && !(i + 2 < n && s[i + 2] == '\'')) {
        size_t j = i + 1;
        while (j < n && ident_char(s[j], false)) j++;
        out.push(i, j - i, Tok::Type);  // 'a
        i = j;
        continue;
      }
      const size_t e = string_end(s, i, '\'');
      if (e != std::string_view::npos && e - i <= 8) {
        out.push(i, e - i, Tok::String);
        i = e;
        continue;
      }
    }

    // Numbers, not the digits inside a name.
    if ((std::isdigit(uint8_t(c)) || (c == '.' && i + 1 < n && std::isdigit(uint8_t(s[i + 1])))) &&
        (i == 0 || !ident_char(s[i - 1], dash))) {
      const bool hex = c == '0' && i + 1 < n && (s[i + 1] == 'x' || s[i + 1] == 'X');
      size_t j = i + 1;
      while (j < n && (std::isalnum(uint8_t(s[j])) || s[j] == '.' || s[j] == '_' || s[j] == '\'' ||
                       (!hex && (s[j] == '+' || s[j] == '-') && (s[j - 1] == 'e' || s[j - 1] == 'E'))))
        j++;
      out.push(i, j - i, Tok::Number);
      i = j;
      continue;
    }

    if ((f & kDecorators) && c == '@' && i + 1 < n && ident_start(s[i + 1])) {
      size_t j = i + 1;
      while (j < n && (ident_char(s[j], false) || s[j] == '.')) j++;
      out.push(i, j - i, Tok::Func);
      i = j;
      continue;
    }
    if ((f & kShellVars) && c == '$' && i + 1 < n) {
      size_t j = i + 1;
      if (s[j] == '{' || s[j] == '(') {
        const char close = s[j] == '{' ? '}' : ')';
        const size_t e = s.find(close, j);
        j = e == std::string_view::npos ? n : e + 1;
      } else if (ident_start(s[j]) || std::isdigit(uint8_t(s[j]))) {
        while (j < n && ident_char(s[j], false)) j++;
      } else if (std::strchr("@*#?$!-", s[j])) {
        j++;
      }
      if (j > i + 1) {
        out.push(i, j - i, Tok::Func);
        i = j;
        continue;
      }
    }
    if ((f & kMarkup) && c == '<') {
      size_t j = i + 1;
      if (j < n && (s[j] == '/' || s[j] == '?' || s[j] == '!')) j++;
      const size_t name = j;
      while (j < n && (ident_char(s[j], true) || s[j] == ':' || s[j] == '.')) j++;
      if (j > name) {
        out.push(i, j - i, Tok::Keyword);
        i = j;
        continue;
      }
    }

    if (ident_start(c) || (c == '$' && !(f & kShellVars))) {
      size_t j = i + 1;
      while (j < n && ident_char(s[j], dash)) j++;
      if (L->name == "ruby" && j < n && (s[j] == '?' || s[j] == '!')) j++;
      const std::string_view w = s.substr(i, j - i);
      // f"…", r'…', b"…": a prefix belongs to its string.
      if ((f & kTriple) && w.size() <= 2 && j < n && (s[j] == '"' || s[j] == '\'') &&
          w.find_first_not_of("rRbBfFuU") == std::string_view::npos) {
        str_from = i;
        i = j;
        continue;
      }
      const size_t k = skip_spaces(s, j);
      const char next = k < n ? s[k] : '\0';
      Tok t = Tok::Text;
      if ((f & kKeys) && next == ':' && (k + 1 >= n || s[k + 1] != ':') &&
          (L->name != "css" || first == i))
        t = Tok::Type;  // a key
      else if ((f & kIniKeys) && first == i && next == '=')
        t = Tok::Type;
      else if ((f & kMarkup) && next == '=')
        t = Tok::Type;  // an attribute
      else if (in_list(L->keywords, w, f & kNoCase))
        t = Tok::Keyword;
      else if (in_list(L->types, w, f & kNoCase))
        t = Tok::Type;
      else if ((f & kMacroBang) && j < n && s[j] == '!') {
        j++;  // println!
        t = Tok::Func;
      } else if (next == '(' && !(f & kMarkup))
        t = Tok::Func;
      else if ((f & kCapsTypes) && w.size() > 1 && std::isupper(uint8_t(w[0]))) {
        const bool all_caps = w.find_first_of("abcdefghijklmnopqrstuvwxyz") == std::string_view::npos;
        t = all_caps ? Tok::Number : Tok::Type;  // a CONSTANT, a Type
      }
      out.push(i, j - i, t);
      i = j;
      continue;
    }
    // A UTF-8 sequence stays whole.
    size_t j = i + 1;
    while (j < n && (uint8_t(s[j]) & 0xC0) == 0x80) j++;
    out.push(i, j - i, Tok::Text);
    i = j;
  }
}

}  // namespace mico::code
