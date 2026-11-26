#include "adapters/pi/form.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include "base/json_write.h"
#include "base/text.h"

namespace mico {
namespace {

// The little of HTML a form needs: elements, their attributes and text.
struct Node {
  std::string tag;   // empty for a text node
  std::string text;  // a text node's, entities decoded
  std::vector<std::pair<std::string, std::string>> attrs;
  std::vector<std::unique_ptr<Node>> kids;
  Node* parent = nullptr;

  const std::string* attr(std::string_view k) const {
    for (const auto& [n, v] : attrs)
      if (n == k) return &v;
    return nullptr;
  }
  bool has(std::string_view k) const { return attr(k) != nullptr; }
};

bool one_of(std::string_view t, std::initializer_list<std::string_view> set) {
  return std::find(set.begin(), set.end(), t) != set.end();
}
bool is_void(std::string_view t) {
  return one_of(t, {"input", "br", "hr", "img", "meta", "link", "col", "area", "base", "embed", "source", "track",
                    "wbr", "param"});
}
// What omp never shows, with all it holds.
bool is_skipped(std::string_view t) {
  return one_of(t, {"button", "head", "iframe", "link", "meta", "noscript", "script", "style", "template", "title"});
}
bool is_block(std::string_view t) {
  return one_of(t, {"address", "article", "aside", "blockquote", "dd", "details", "div", "dl", "dt", "figure",
                    "figcaption", "footer", "header", "li", "main", "nav", "p", "section"});
}
bool is_heading(std::string_view t) { return one_of(t, {"h1", "h2", "h3", "h4", "h5", "h6", "summary"}); }
bool is_control(std::string_view t) { return one_of(t, {"input", "select", "textarea"}); }

std::string lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string decode_entities(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    if (s[i] != '&') {
      out += s[i++];
      continue;
    }
    const size_t semi = s.find(';', i);
    if (semi == std::string_view::npos || semi - i > 10) {
      out += s[i++];
      continue;
    }
    const std::string_view name = s.substr(i + 1, semi - i - 1);
    char32_t cp = 0;
    if (name == "amp") cp = '&';
    else if (name == "lt") cp = '<';
    else if (name == "gt") cp = '>';
    else if (name == "quot") cp = '"';
    else if (name == "apos") cp = '\'';
    else if (name == "nbsp") cp = ' ';
    else if (name.size() > 1 && name[0] == '#') {
      const bool hex = name[1] == 'x' || name[1] == 'X';
      cp = char32_t(std::strtoul(std::string(name.substr(hex ? 2 : 1)).c_str(), nullptr, hex ? 16 : 10));
    }
    if (cp == 0) {
      out += s[i++];
      continue;
    }
    text::encode(cp, out);
    i = semi + 1;
  }
  return out;
}

// Runs of white space as one space, none at either end: how omp shows text.
std::string squash(std::string_view s) {
  std::string out;
  bool space = false;
  for (unsigned char c : s) {
    if (std::isspace(c)) {
      space = !out.empty();
      continue;
    }
    if (space) out += ' ';
    space = false;
    out += char(c);
  }
  return out;
}

std::unique_ptr<Node> parse_html(std::string_view h) {
  auto root = std::make_unique<Node>();
  root->tag = "#root";
  Node* cur = root.get();
  const auto add = [&](std::unique_ptr<Node> n) {
    n->parent = cur;
    Node* raw = n.get();
    cur->kids.push_back(std::move(n));
    return raw;
  };
  const auto add_text = [&](std::string_view t) {
    if (t.empty()) return;
    auto n = std::make_unique<Node>();
    n->text = decode_entities(t);
    add(std::move(n));
  };
  // Closes the innermost open `tag`, and everything opened inside it.
  const auto close = [&](std::string_view tag) {
    for (Node* n = cur; n && n != root.get(); n = n->parent)
      if (n->tag == tag) {
        cur = n->parent;
        return;
      }
  };
  size_t i = 0;
  while (i < h.size()) {
    if (h[i] != '<') {
      const size_t next = h.find('<', i);
      add_text(h.substr(i, next == std::string_view::npos ? std::string_view::npos : next - i));
      i = next == std::string_view::npos ? h.size() : next;
      continue;
    }
    if (h.substr(i, 4) == "<!--") {
      const size_t end = h.find("-->", i + 4);
      i = end == std::string_view::npos ? h.size() : end + 3;
      continue;
    }
    if (i + 1 < h.size() && (h[i + 1] == '!' || h[i + 1] == '?')) {
      const size_t end = h.find('>', i);
      i = end == std::string_view::npos ? h.size() : end + 1;
      continue;
    }
    const bool closing = i + 1 < h.size() && h[i + 1] == '/';
    size_t p = i + (closing ? 2 : 1);
    size_t q = p;
    while (q < h.size() && (std::isalnum(static_cast<unsigned char>(h[q])) || h[q] == '-')) q++;
    if (q == p) {  // a '<' that opens no tag is text
      add_text(h.substr(i, 1));
      i++;
      continue;
    }
    const std::string tag = lower(h.substr(p, q - p));
    if (closing) {
      const size_t end = h.find('>', q);
      i = end == std::string_view::npos ? h.size() : end + 1;
      close(tag);
      continue;
    }
    auto node = std::make_unique<Node>();
    node->tag = tag;
    bool self_closing = false;
    p = q;
    while (p < h.size() && h[p] != '>') {
      if (std::isspace(static_cast<unsigned char>(h[p]))) { p++; continue; }
      if (h[p] == '/') { self_closing = true; p++; continue; }
      size_t n0 = p;
      while (p < h.size() && !std::isspace(static_cast<unsigned char>(h[p])) && h[p] != '=' && h[p] != '>' &&
             h[p] != '/')
        p++;
      std::string name = lower(h.substr(n0, p - n0));
      std::string value;
      while (p < h.size() && std::isspace(static_cast<unsigned char>(h[p]))) p++;
      if (p < h.size() && h[p] == '=') {
        p++;
        while (p < h.size() && std::isspace(static_cast<unsigned char>(h[p]))) p++;
        if (p < h.size() && (h[p] == '"' || h[p] == '\'')) {
          const char quote = h[p++];
          const size_t end = h.find(quote, p);
          value = decode_entities(h.substr(p, end == std::string_view::npos ? std::string_view::npos : end - p));
          p = end == std::string_view::npos ? h.size() : end + 1;
        } else {
          const size_t v0 = p;
          while (p < h.size() && !std::isspace(static_cast<unsigned char>(h[p])) && h[p] != '>') p++;
          value = decode_entities(h.substr(v0, p - v0));
        }
      }
      if (!name.empty()) node->attrs.emplace_back(std::move(name), std::move(value));
      else p++;
    }
    i = p < h.size() ? p + 1 : h.size();
    // The ends HTML leaves implicit.
    if (tag == "option" || tag == "optgroup") close("option");
    if (tag == "li" && cur->tag == "li") close("li");
    if (tag == "p" && cur->tag == "p") close("p");
    if ((tag == "td" || tag == "th") && (cur->tag == "td" || cur->tag == "th")) close(cur->tag);
    if (tag == "tr") {
      if (cur->tag == "td" || cur->tag == "th") close(cur->tag);
      if (cur->tag == "tr") close("tr");
    }
    Node* opened = add(std::move(node));
    if (is_void(tag) || self_closing) continue;
    // Text that is not markup: a textarea's value, a script's code.
    if (tag == "textarea" || tag == "script" || tag == "style" || tag == "title") {
      const size_t end = h.find("</" + tag, i);
      if (tag == "textarea") {
        auto t = std::make_unique<Node>();
        t->text = decode_entities(h.substr(i, end == std::string_view::npos ? std::string_view::npos : end - i));
        t->parent = opened;
        opened->kids.push_back(std::move(t));
      }
      if (end == std::string_view::npos) break;
      const size_t gt = h.find('>', end);
      i = gt == std::string_view::npos ? h.size() : gt + 1;
      continue;
    }
    cur = opened;
  }
  return root;
}

template <class F>
void each_element(const Node& n, F&& f) {
  for (const auto& k : n.kids)
    if (!k->tag.empty()) {
      f(*k);
      each_element(*k, f);
    }
}

const Node* first_element(const Node& n, std::string_view tag) {
  const Node* found = nullptr;
  each_element(n, [&](const Node& e) {
    if (!found && e.tag == tag) found = &e;
  });
  return found;
}

std::string text_content(const Node& n) {
  if (n.tag.empty()) return n.text;
  std::string out;
  for (const auto& k : n.kids) out += text_content(*k);
  return out;
}

struct Option {
  std::string value, label;
  bool checked = false, disabled = false;
};
struct Field {
  bool choice = false;  // else text
  bool multi = false, select = false;
  std::string name, label;
  std::vector<Option> options;
};
struct Block {
  Field* field = nullptr;  // else text, which may be empty: a picture
  std::string text;
};
struct Section {
  std::string title;
  std::vector<Block> blocks;
};

// omp's walk over a form (its parse.ts), keeping what says which field asks
// what: the fields, in order, and the text and headings around them.
class Builder {
 public:
  explicit Builder(const Node& root) : root_(root) { sections_.emplace_back(); }

  void build() {
    std::set<std::string> ids;
    each_element(root_, [&](const Node& e) {
      if (is_control(e.tag))
        if (const std::string* id = e.attr("id"); id && !id->empty()) ids.insert(*id);
    });
    each_element(root_, [&](const Node& e) {
      if (e.tag != "label") return;
      if (const std::string* f = e.attr("for"); f && ids.count(*f) && !label_for_.count(*f)) label_for_[*f] = &e;
    });
    const Node* form = first_element(root_, "form");
    kids(form ? *form : root_);
    flush();
  }

  const std::vector<std::unique_ptr<Field>>& fields() const { return fields_; }
  const std::vector<Section>& sections() const { return sections_; }

 private:
  Section& cur() { return sections_.back(); }

  void flush() {
    std::string t = squash(acc_);
    acc_.clear();
    if (!t.empty()) cur().blocks.push_back(Block{nullptr, std::move(t)});
  }

  void kids(const Node& n) {
    for (const auto& k : n.kids) visit(*k);
  }

  // A label's or a legend's words: no controls, a space between blocks.
  std::string words(const Node& n) {
    std::string out;
    const auto walk = [&](const auto& self, const Node& x) -> void {
      if (x.tag.empty()) {
        out += x.text;
        return;
      }
      if (is_skipped(x.tag) || is_control(x.tag) || x.tag == "svg") return;
      if (x.tag == "br") {
        out += '\n';
        return;
      }
      if (x.tag == "img") {
        if (const std::string* alt = x.attr("alt")) out += *alt;
        return;
      }
      if (is_block(x.tag) || is_heading(x.tag)) out += ' ';
      for (const auto& k : x.kids) self(self, *k);
    };
    for (const auto& k : n.kids) walk(walk, *k);
    return squash(out);
  }

  void visit(const Node& n) {
    if (n.tag.empty()) {
      acc_ += n.text;
      return;
    }
    const std::string& t = n.tag;
    if (is_skipped(t)) return;
    if (t == "br") {
      acc_ += '\n';
      return;
    }
    if (t == "hr") return flush();
    if (is_control(t)) return control(n, {});
    if (t == "label") return label(n);
    if (t == "fieldset") return fieldset(n);
    if (t == "legend" || is_heading(t) || is_block(t)) {
      flush();
      kids(n);
      return flush();
    }
    if (t == "ul" || t == "ol" || t == "table") {
      flush();
      bool controls = false;
      each_element(n, [&](const Node& e) { controls |= is_control(e.tag); });
      if (controls) {
        kids(n);
        return flush();
      }
      cur().blocks.push_back(Block{nullptr, squash(text_content(n))});
      return;
    }
    if (t == "pre") {
      flush();
      cur().blocks.push_back(Block{nullptr, squash(text_content(n))});
      return;
    }
    if (t == "img" || t == "svg") {
      flush();
      cur().blocks.push_back(Block{});  // not text: it parts a field from the text before it
      return;
    }
    kids(n);
  }

  void label(const Node& n) {
    if (const std::string* f = n.attr("for"); f) {
      const auto it = label_for_.find(*f);
      if (it != label_for_.end() && it->second == &n) return;  // its control names it
    }
    std::vector<const Node*> controls;
    each_element(n, [&](const Node& e) {
      if (is_control(e.tag)) controls.push_back(&e);
    });
    if (controls.empty()) return kids(n);
    flush();
    const std::string text = words(n);
    for (const Node* c : controls) control(*c, text);
  }

  void fieldset(const Node& n) {
    flush();
    const Node* legend = nullptr;
    for (const auto& k : n.kids)
      if (k->tag == "legend") {
        legend = k.get();
        break;
      }
    if (depth_ > 0) {
      if (legend) visit(*legend);
    } else {
      section(legend ? words(*legend) : std::string());
    }
    depth_++;
    for (const auto& k : n.kids)
      if (k.get() != legend) visit(*k);
    flush();
    depth_--;
    if (depth_ == 0) section({});
  }

  void section(std::string title) {
    if (cur().blocks.empty() && cur().title.empty()) {
      cur().title = std::move(title);
      return;
    }
    sections_.push_back(Section{std::move(title), {}});
  }

  std::string named_by(const Node& n) {
    if (const std::string* id = n.attr("id"))
      if (const auto it = label_for_.find(*id); it != label_for_.end()) return words(*it->second);
    if (const std::string* a = n.attr("aria-label")) return squash(*a);
    if (const std::string* a = n.attr("title")) return squash(*a);
    return {};
  }

  Field* add(std::unique_ptr<Field> f) {
    Field* raw = f.get();
    fields_.push_back(std::move(f));
    cur().blocks.push_back(Block{raw, {}});
    return raw;
  }

  void control(const Node& n, const std::string& label) {
    flush();
    const std::string k = label.empty() ? named_by(n) : label;
    const std::string name = n.attr("name") ? *n.attr("name") : std::string();
    if (n.tag == "select") {
      auto f = std::make_unique<Field>();
      f->choice = f->select = true;
      f->multi = n.has("multiple");
      f->name = name;
      f->label = k;
      each_element(n, [&](const Node& e) {
        if (e.tag != "option") return;
        Option o;
        o.label = squash(text_content(e));
        o.value = e.attr("value") ? *e.attr("value") : o.label;
        o.checked = e.has("selected");
        o.disabled = e.has("disabled");
        f->options.push_back(std::move(o));
      });
      if (!f->multi && std::none_of(f->options.begin(), f->options.end(), [](const Option& o) { return o.checked; }))
        for (Option& o : f->options)
          if (!o.disabled) {
            o.checked = true;
            break;
          }
      add(std::move(f));
      return;
    }
    const std::string type = n.tag == "textarea" ? "textarea" : lower(n.attr("type") ? *n.attr("type") : "text");
    if (one_of(type, {"button", "file", "image", "reset", "submit", "hidden"})) return;
    if (type == "radio" || type == "checkbox") {
      Option o;
      o.value = n.attr("value") ? *n.attr("value") : "on";
      o.label = !k.empty() ? k : n.attr("value") ? *n.attr("value") : name;
      o.checked = n.has("checked");
      o.disabled = n.has("disabled");
      const std::string key = type + '\0' + name;
      if (const auto it = groups_.find(key); it != groups_.end()) {
        it->second->options.push_back(std::move(o));
        return;
      }
      auto f = std::make_unique<Field>();
      f->choice = true;
      f->multi = type == "checkbox";
      f->name = name;
      f->options.push_back(std::move(o));
      groups_[key] = add(std::move(f));
      return;
    }
    auto f = std::make_unique<Field>();
    f->name = name;
    f->label = k;
    add(std::move(f));
  }

  const Node& root_;
  std::vector<std::unique_ptr<Field>> fields_;
  std::vector<Section> sections_;
  std::map<std::string, Field*> groups_;
  std::map<std::string, const Node*> label_for_;
  std::string acc_;
  int depth_ = 0;
};

}  // namespace

bool omp_form_questions(std::string_view html, std::string& questions, std::string& first_question) {
  questions.clear();
  first_question.clear();
  const auto root = parse_html(html);
  Builder b(*root);
  b.build();
  if (std::none_of(b.fields().begin(), b.fields().end(), [](const auto& f) { return !f->name.empty(); }))
    return false;

  // What each field asks (omp's Sp): its label, else the text just before
  // it, else its section's heading, else its only option, else its name.
  std::map<const Field*, std::string> asks;
  for (const Section& s : b.sections()) {
    std::string before;
    for (const Block& blk : s.blocks) {
      if (!blk.field) {
        before = blk.text;
        continue;
      }
      const Field& f = *blk.field;
      std::string q = f.label;
      if (q.empty()) q = before;
      if (q.empty()) q = s.title;
      if (q.empty() && f.choice && f.options.size() == 1) q = squash(f.options[0].label);
      if (q.empty()) q = f.name;
      asks[&f] = q;
      before.clear();
    }
  }

  jw::Writer w(questions);
  w.begin_array();
  for (const auto& fp : b.fields()) {
    const Field& f = *fp;
    const std::string& q = asks[&f];
    if (first_question.empty()) first_question = q;
    std::vector<const Option*> on;
    for (const Option& o : f.options)
      if (!o.disabled) on.push_back(&o);
    w.begin_object().field("question", q);
    const bool answerable = f.choice && !f.name.empty() && !on.empty();
    if (!answerable) {
      w.field("multi", false).key("options").begin_array().end_array();
      w.key("form").begin_object().field("control", "text").field("name", f.name).end_object();
      w.end_object();
      continue;
    }
    // A radio group's last checked option is the one selected.
    std::vector<int> checked;
    for (size_t i = 0; i < on.size(); i++)
      if (on[i]->checked) {
        if (!f.multi) checked.clear();
        checked.push_back(int(i));
      }
    w.field("multi", f.multi).field("recommended", checked.empty() ? 0 : checked.front());
    w.key("options").begin_array();
    for (const Option* o : on) {
      std::string label = squash(o->label);
      if (label.empty()) label = o->value;
      w.begin_object().field("label", label).field("value", o->value).end_object();
    }
    w.end_array();
    const char* control = f.select ? (f.multi ? "multiselect" : "select") : (f.multi ? "checkbox" : "radio");
    w.key("form").begin_object().field("control", control).field("name", f.name);
    w.key("checked").begin_array();
    for (int c : checked) w.num(c);
    w.end_array().end_object();
    w.end_object();
  }
  w.end_array();
  return true;
}

}  // namespace mico
