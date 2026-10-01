// `mico --mcp`: the tools mico offers agents, as an MCP server on stdio.
//
// One tool so far, `plot`: the agent hands over a chart spec and mico draws it
// in the chat, where the call sits in the transcript. The server's part is
// only to check the spec, with the same parser the chat draws with, so a
// mistake comes back to the agent as a tool error it can fix rather than as a
// broken chart the user sees. Agents get the server only when the user has
// turned it on (`:mcp on`); see LiveSession::start.
//
// The protocol is JSON-RPC 2.0, one message per line.
#include <unistd.h>

#include <cstdio>
#include <iostream>
#include <string>

#include "base/json.h"
#include "views/chart.h"

namespace mico {
namespace {

using js::quote;

constexpr const char* kPlotDescription =
    "Draw a chart in the user's chat, inline in the terminal (a real plot where the terminal shows "
    "images, its labels in the terminal's font; braille otherwise). Several charts side by side: "
    "\"subplots\": [chart, chart, ...], \"columns\": n to wrap them into a grid. Use it when a "
    "trend or a comparison reads better as a picture than as numbers. Give the data inline (x plus "
    "series of y values), or name a CSV (with a header) or JSONL file and its columns: mico reads the "
    "file itself and redraws the chart whenever it changes, so it follows a running job such as a "
    "training run. The chart is shown to the user; you get back only whether it could be drawn.";

// The input schema: the same fields a ```chart block takes.
constexpr const char* kPlotSchema = R"json({
  "type": "object",
  "properties": {
    "type": {"type": "string", "enum": ["line", "scatter", "bar", "hist", "spark", "heatmap"], "description": "line (default), scatter, bar (horizontal, labelled), hist (\"values\" binned, \"bins\" optional), spark (a one-line sparkline per series), heatmap (\"z\": rows of values, \"labels\" columns, \"ylabels\" rows)"},
    "title": {"type": "string"},
    "xlabel": {"type": "string"},
    "ylabel": {"type": "string"},
    "x": {"description": "Inline: an array of x values shared by the series (numbers; strings become bar labels). From a file: the x column's name."},
    "y": {"description": "Inline: one series' values (numbers, null for a gap). From a file: a column name, or an array of them."},
    "series": {"type": "array", "description": "Inline data for several series", "items": {"type": "object", "properties": {"name": {"type": "string"}, "y": {"type": "array", "items": {"type": ["number", "null"]}}, "x": {"type": "array", "items": {"type": "number"}}}, "required": ["y"]}},
    "labels": {"type": "array", "items": {"type": "string"}, "description": "Bar chart categories; heatmap columns"},
    "values": {"type": "array", "items": {"type": "number"}, "description": "A histogram's raw values"},
    "bins": {"type": "integer", "minimum": 1, "description": "A histogram's bin count (default: chosen from the data)"},
    "z": {"type": "array", "items": {"type": "array", "items": {"type": ["number", "null"]}}, "description": "A heatmap's values, row by row"},
    "ylabels": {"type": "array", "items": {"type": "string"}, "description": "A heatmap's row labels"},
    "file": {"type": "string", "description": "A CSV or JSONL file to read the columns from, relative to the working directory"},
    "log_y": {"type": "boolean"},
    "marker": {"type": "string", "enum": ["line", "block", "braille"], "description": "braille (default, finest detail), line (box drawing, whole in any font), block (2x2 blocks). Leave it out unless the user asks."},
    "height": {"type": "integer", "minimum": 3, "maximum": 40, "description": "Plot height in rows (default 12)"},
    "font": {"type": "string", "enum": ["terminal", "math"], "description": "terminal (default): labels in the terminal's font around the plot; math: the whole chart typeset like LaTeX. Leave it out unless the user asks."},
    "subplots": {"type": "array", "items": {"type": "object"}, "description": "A figure of several charts, each an object with these same fields, laid side by side; the top-level title goes above them"},
    "columns": {"type": "integer", "minimum": 1, "description": "Subplots per row (default: all in one row; fewer when the chat is narrow)"}
  }
})json";

std::string result(std::string_view id, const std::string& body) {
  return std::string("{\"jsonrpc\":\"2.0\",\"id\":") + std::string(id) + ",\"result\":" + body + "}";
}

std::string error(std::string_view id, int code, std::string_view message) {
  return std::string("{\"jsonrpc\":\"2.0\",\"id\":") + std::string(id) + ",\"error\":{\"code\":" +
         std::to_string(code) + ",\"message\":" + quote(message) + "}}";
}

std::string tool_text(const std::string& text, bool is_error) {
  return std::string("{\"content\":[{\"type\":\"text\",\"text\":") + quote(text) + "}],\"isError\":" +
         (is_error ? "true" : "false") + "}";
}

// Checks a plot call the way the chat will draw it.
std::string plot(const js::Value& args, const std::string& cwd) {
  if (!args.is_object()) return tool_text("plot takes an object of chart fields", true);
  chart::Spec spec;
  std::string why;
  if (!chart::parse(args.raw, spec, &why)) return tool_text("Not drawn: " + why, true);
  std::vector<std::pair<std::string, int64_t>> files;
  if (!chart::load_files(spec, cwd, &why, &files)) return tool_text("Not drawn: " + why, true);
  const auto kind_of = [](const chart::Spec& s) {
    switch (s.kind) {
      case chart::Spec::Kind::Bar: return "bar";
      case chart::Spec::Kind::Scatter: return "scatter";
      case chart::Spec::Kind::Hist: return "histogram";
      case chart::Spec::Kind::Spark: return "sparkline";
      case chart::Spec::Kind::Heatmap: return "heatmap";
      default: return "line";
    }
  };
  std::string text;
  if (!spec.subplots.empty()) {
    text = "Drawn for the user: a figure of " + std::to_string(spec.subplots.size()) + " charts (";
    for (size_t i = 0; i < spec.subplots.size(); i++) text += std::string(i ? ", " : "") + kind_of(spec.subplots[i]);
    text += ")";
    if (!spec.title.empty()) text += " \"" + spec.title + "\"";
    text += ".";
  } else {
    size_t points = 0;
    for (const auto& s : spec.series) points += s.y.size();
    for (const auto& r : spec.z) points += r.size();
    text = std::string("Drawn for the user: a ") + kind_of(spec) + " chart";
    if (!spec.title.empty()) text += " \"" + spec.title + "\"";
    text += ", " + std::to_string(spec.series.size()) + " series, " + std::to_string(points) + " points.";
  }
  if (!files.empty()) text += " It follows its data file" + std::string(files.size() > 1 ? "s" : "") +
                              " and redraws when they change.";
  return tool_text(text, false);
}

}  // namespace

// One message in, the reply out (empty for a notification, which gets none).
std::string mcp_handle(std::string_view msg, const std::string& cwd) {
  std::string_view id, method;
  js::Value params{};
  bool parsed = js::scan_object(msg, [&](std::string_view k, const js::Value& v) {
    if (k == "id") id = v.raw;
    else if (k == "method") method = v.body();
    else if (k == "params") params = v;
    return true;
  });
  if (!parsed) return error("null", -32700, "parse error");
  if (id.empty()) return {};  // a notification: initialized, cancelled…

  if (method == "initialize") {
    // Speak whichever protocol version the client asked for.
    std::string version = "2025-06-18";
    js::scan_object(params.raw, [&](std::string_view k, const js::Value& v) {
      if (k == "protocolVersion" && v.is_string()) version = std::string(v.body());
      return true;
    });
    return result(id, "{\"protocolVersion\":" + quote(version) +
                          ",\"capabilities\":{\"tools\":{\"listChanged\":false}},"
                          "\"serverInfo\":{\"name\":\"mico\",\"version\":\"1\"},"
                          "\"instructions\":" +
                          quote("Tools of mico, the terminal UI showing this chat to the user. Call plot to draw a chart in the chat.") +
                          "}");
  }
  if (method == "ping") return result(id, "{}");
  if (method == "tools/list") {
    // Written readably above; sent on one line, since a line is a message.
    std::string schema;
    for (const char c : std::string_view(kPlotSchema))
      if (c != '\n') schema += c;
    return result(id, std::string("{\"tools\":[{\"name\":\"plot\",\"description\":") + quote(kPlotDescription) +
                          ",\"inputSchema\":" + schema + "}]}");
  }
  if (method == "tools/call") {
    std::string name;
    js::Value args{};
    js::scan_object(params.raw, [&](std::string_view k, const js::Value& v) {
      if (k == "name" && v.is_string()) name = std::string(v.body());
      else if (k == "arguments") args = v;
      return true;
    });
    if (name != "plot") return result(id, tool_text("mico has no tool named " + name, true));
    return result(id, plot(args, cwd));
  }
  return error(id, -32601, "method not found: " + std::string(method));
}

int run_mcp_server() {
  char buf[4096];
  const std::string cwd = getcwd(buf, sizeof buf) ? std::string(buf) : std::string(".");
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) continue;
    const std::string reply = mcp_handle(line, cwd);
    if (reply.empty()) continue;
    fwrite(reply.data(), 1, reply.size(), stdout);
    fputc('\n', stdout);
    fflush(stdout);
  }
  return 0;
}

}  // namespace mico
