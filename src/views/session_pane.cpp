#include <algorithm>

#include "adapters/adapters.h"
#include "adapters/screen.h"
#include "core/settings.h"
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <optional>
#include <vector>

#include "core/session.h"
#include "core/work.h"
#include "vt/keys.h"
#include "base/text.h"
#include "vt/vt.h"
#include "ui/app.h"
#include "views/chat_render.h"
#include "views/markdown_style.h"
#include "views/completion.h"
#include "views/outline.h"
#include "base/progress.h"
#include "core/answers.h"
#include "views/find_bar.h"
#include "views/prompt_editor.h"
#include "core/clipboard.h"
#include "views/views.h"

namespace mico {

// A braille spinner, advanced by the App's animation tick. Braille is a single
// cell in every font that matters, so a turning frame never reflows the line.
char32_t spinner_glyph(uint64_t anim) {
  static const char32_t kFrames[] = {U'\u280B', U'\u2819', U'\u2839', U'\u2838',
                                     U'\u283C', U'\u2834', U'\u2826', U'\u2827',
                                     U'\u2807', U'\u280F'};
  return kFrames[anim % (sizeof(kFrames) / sizeof(kFrames[0]))];
}

char32_t orbit_glyph(uint64_t anim) {
  static const char32_t kFrames[] = {U'\u2801', U'\u2802', U'\u2804', U'\u2840',
                                     U'\u2880', U'\u2820', U'\u2810', U'\u2808'};
  return kFrames[(anim / 2) % (sizeof(kFrames) / sizeof(kFrames[0]))];
}

namespace {

std::string spinner_utf8(uint64_t anim) {
  std::string s;
  text::encode(spinner_glyph(anim), s);
  return s;
}

// A live agent. Holds both planes at once and paints one of them: the raw pty
// screen, or the transcript rendered as a chat. Switching is a render choice
// only — neither plane is ever torn down, so nothing is missed while you are
// looking at the other one.
class SessionPane final : public Pane {
 public:
  enum class View { Raw, Chat };

  explicit SessionPane(LiveSession* s) : s_(s) {}

  // A message longer than this scrolls inside the box rather than growing it
  // further — the chat above still needs room to breathe.
  static constexpr int kMaxPromptRows = 6;

  LiveSession* session() const override { return s_; }
  void handoff(Handoff& h) override {
    int permission_cursor = perm_pick_.cursor();
    h.fields(view_, view_chosen_, prompt_, hist_, hist_at_, hist_shown_, note_q_, note_header_,
             reply_live_, reply_.call_id, reply_.index, reply_.question, reply_.answer,
             perm_key_, perm_note_, note_perm_, answering_question_, raw_scroll_, permission_cursor);
    h.fields(question_focus_, suspended_prompt_, prompt_suspended_);
    h.fields(work_open_, work_focus_, work_tab_, work_selected_, work_prompt_);
    if (h.reading()) {
      refresh_permission();
      perm_pick_.set_cursor(permission_cursor);
    }
  }

  std::string title() const override {
    std::string where = app_ ? app_->workspace().place_text(*s_) : std::string();
    if (!where.empty()) where = " \xC2\xB7 " + where;
    return (app_ ? app_->session_title(*s_) : s_->label()) + where +
           (effective_view() == View::Raw ? " · terminal" : "");
  }

  // Supported agents default to chat, even before their transcript exists.
  // Editors and other commands have no chat adapter: their terminal is the
  // only view. F2 switches planes only for supported agents.
  View effective_view() const {
    if (!s_->adapter()) return View::Raw;
    if (view_chosen_) return view_;
    // An exited session put its reason on its own screen; a session waiting on
    // a startup prompt (a trust dialog) needs that screen to be answerable.
    if (s_->exited() && s_->transcript().empty()) return View::Raw;
    // Only a recognisable startup trust dialog may choose the terminal for
    // us. Ordinary prompt cursors and quoted questions also match needs_input;
    // using that heuristic here made sending a message switch views.
    if (s_->starting() && s_->driver().startup_prompt(s_->vt())) return View::Raw;
    return View::Chat;
  }
  bool showing_raw() const { return effective_view() == View::Raw; }

  void render(Painter& p, bool focused) override {
    work_rect_ = Rect{};
    if (work_open_ && !showing_raw() && p.width() >= 100) {
      const int w = std::clamp(p.width() / 3, 36, 64);
      work_rect_ = Rect{p.width() - w, 0, w, p.height()};
      Painter main = p.sub(Rect{0, 0, p.width() - w, p.height()});
      render_main(main, focused && !work_focus_);
    } else render_main(p, focused && !work_focus_);
    if (work_open_ && !showing_raw()) {
      if (work_rect_.w == 0) { work_rect_ = Rect{0, 0, p.width(), p.height()}; work_focus_ = true; }
      render_work(p.sub(work_rect_), focused && work_focus_);
    }
  }

  void render_main(Painter& p, bool focused) {
    const Theme& th = app_->theme();
    // On screen counts as seen, focused or not: a chat open beside the list
    // is being read.
    s_->mark_seen();
    (void)focused;

    // The pty is spawned here, at the size it will be drawn at, and resized
    // only when the pane changes. Switching views must never resize it: both
    // agents render inline, so a resize repaints and rewrites scrollback.
    s_->set_geometry(p.width(), p.height());

    // The chat plane is polled while chat is painted, and also whenever the
    // agent has stopped on a prompt: a model question is only visible once its
    // transcript line lands, and the view decision below needs it. A busy raw
    // pane is left alone, so a long session does not pile up events it will
    // never draw until memory forces a re-anchor.
    if ((!showing_raw() || s_->needs_input()) && !s_->transcript().empty()) {
      chat_.set_base_dir(s_->cwd());
      chat_.open(s_->transcript(), s_->adapter());
      chat_.poll_growth();
    }
    const bool working = s_->busy() && !chat_.has_pending_question() && !s_->answer_sending();
    if (!working) activity_since_.reset();
    chat_.seed_agent(s_->adapter());
    chat_.set_questions_interactive(!s_->exited());
    chat_.set_working(working, spinner_glyph(app_->anim()));
    chat_.set_background(s_->exited() ? std::vector<BackgroundTask>{} : s_->background());
    // The reply being written, read off the agent's screen: its transcript
    // gets a block only once the block is complete. Kept a moment past the
    // end of the work, until the last block has landed in the transcript.
    {
      const auto now = std::chrono::steady_clock::now();
      if (s_->busy()) draft_until_ = now + std::chrono::seconds(3);
      chat_.set_draft(!showing_raw() && s_->adapter() && now < draft_until_ ? screen_draft() : std::string_view());
    }
    // A question's answer stops once its result lands; a permission answer
    // has no transcript result to wait for and runs to its last key.
    if (answering_question_ && s_->answer_sending() && !chat_.has_pending_question())
      s_->cancel_answer();
    refresh_permission();
    refresh_btw();
    // Notes that never went out, because their answer did not land, come
    // back to the box rather than vanish.
    if (s_->answer_failed()) {
      std::string notes = s_->take_after_answer();
      if (!notes.empty() && prompt_.empty()) {
        insert_text(notes);
        app_->set_status("answer not delivered — your notes are back in the box");
      }
    }
    // A note is for one card; once that card is answered or gone, whatever
    // is in the box is an ordinary draft again.
    if (note_q_ >= 0 && chat_.question_note_target() < 0) note_q_ = -1;
    if (note_perm_ && !perm_live_) note_perm_ = false;

    // A search across chats opened this one at a match.
    {
      uint64_t at = 0;
      std::string q;
      if (app_->take_reveal(s_->transcript(), &at, &q)) {
        if (!q.empty()) find_.open(chat_, q);
        chat_.reveal(at, std::move(q));
      }
    }

    if (showing_raw()) {
      render_raw(p, th);
      return;
    }

    // Bottom rows, outermost first: the prompt box — grown to fit a multi-line
    // message, up to a cap — then the state strip, then — room permitting —
    // a rule marking where the conversation actually ends. Without it the
    // chips read as just another line of chat.
    const bool framed_prompt = p.height() >= 10 && p.width() >= 24;
    const int frame_rows = framed_prompt ? 2 : 0;
    const int prompt_width = p.width() - (framed_prompt ? 4 : 0);
    prompt_w_ = std::max(1, prompt_width - 2);
    const int prompt_h = frame_rows +
        std::clamp(prompt_rows(prompt_width), 1,
                   std::max(1, std::min(kMaxPromptRows, p.height() - frame_rows - 2)));
    // Messages waiting to reach the agent sit right above the box they were
    // typed in: what mico holds until the turn ends, and what the agent holds
    // until its next step.
    held_.clear();
    for (const auto& parts : s_->queued()) held_.push_back({false, preview(parts)});
    if (s_->busy())
      for (std::string_view t : chat_.agent_queue()) held_.push_back({true, text::oneline(t, 200)});
    int queue_h = std::min<int>(int(held_.size()), kMaxHeldRows);
    // A permission dialog takes the place of the waiting messages: nothing
    // moves until it is answered anyway.
    int perm_h = 0;
    if (perm_live_) {
      queue_h = 0;
      perm_h = std::min(permission_rows(p.width()), std::max(0, p.height() - prompt_h - 4));
    }
    queue_h += perm_h;
    if (p.height() < prompt_h + queue_h + 6 && !perm_h) queue_h = 0;  // no room: the prompt comes first
    // The find bar sits right above the box, over whatever waits there.
    const int find_h = find_.active() && p.height() > prompt_h + 4 ? 1 : 0;
    queue_h += find_h;
    const bool chips = s_->adapter() && p.height() > prompt_h + queue_h + 3;
    const bool chip_sep = chips && p.height() > prompt_h + queue_h + 4;
    chip_row_ = chips ? p.height() - prompt_h - queue_h - 1 : -1;
    const int footer_rows = prompt_h + queue_h + (chips ? 1 : 0) + (chip_sep ? 1 : 0);
    // The agent's state is in the title bar (badge()); the pane's rows all go
    // to the chat. The agent and folder it used to repeat here are already the
    // sidebar's selection.
    body_top_ = 0;
    const int act_h = s_->adapter() && p.height() - footer_rows - body_top_ > 1 ? 1 : 0;
    chat_.set_activity_bar(act_h > 0);
    int ch = std::max(1, p.height() - footer_rows - act_h - body_top_);
    Painter body = p.sub(Rect{0, body_top_, p.width(), ch});
    if (s_->transcript().empty()) {
      body.clear(Style{th.text, th.panel});
      std::string msg;
      bool err = false;
      if (!s_->start_error().empty()) {
        msg = s_->start_error();
        err = true;
      } else if (s_->exited()) {
        char b[80];
        snprintf(b, sizeof b, "the agent exited (code %d) before writing anything",
                 s_->exit_status());
        msg = b;
        err = true;
      } else {
        msg = s_->origin().empty() ? "Start a conversation" : "Opening your conversation…";
      }
      {
        const int ey = std::max(0, body.height() / 3);
        body.text_clipped(2, ey, msg, Style{err ? th.err : th.text, th.panel, attr::kBold}, body.width() - 4);
        if (!err && body.height() > ey + 2)
          body.text_clipped(2, ey + 2, "Write a message below. Your draft stays with this chat.",
                            Style{th.dim, th.panel}, body.width() - 4);
      }
    } else {
      chat_.render(body, th, app_->filters());
    }

    if (act_h) render_activity(p.sub(Rect{0, body_top_ + ch, p.width(), act_h}), th, working);

    if (chip_sep) {
      Painter sep = p.sub(Rect{0, chip_row_ - 1, p.width(), 1});
      sep.clear(Style{th.dim, th.strip_bg});
      sep.hline(0, 0, sep.width(), U'─', Style{th.border, th.strip_bg});
    }
    if (chips) {
      Painter strip = p.sub(Rect{0, chip_row_, p.width(), 1});
      chat_.render_chips(strip, th, chips_);
      draw_background(strip, th);
    }
    if (find_h)
      find_.render(p.sub(Rect{0, p.height() - prompt_h - 1, p.width(), 1}), th, chat_);
    perm_top_ = -1;
    if (perm_h) {
      perm_top_ = p.height() - prompt_h - find_h - perm_h;
      perm_pick_.render(p.sub(Rect{0, perm_top_, p.width(), perm_h}), th);
      perm_h_ = perm_h;
    } else if (queue_h - find_h > 0) {
      render_held(p.sub(Rect{0, p.height() - prompt_h - queue_h, p.width(), queue_h - find_h}), th);
    }
    // The "/" and "@" menus float over the chat, right above the box whose
    // word they complete.
    refresh_completion();
    comp_rect_ = Rect{};
    if (focused && comp_.visible()) {
      Picker& pk = comp_.picker();
      const int w = std::min(p.width() - 2, std::max(48, std::min(pk.natural_width(), 100)));
      const int h = std::min(pk.rows(w, 14), p.height() - prompt_h);
      if (h >= 3 && w >= 20) {
        comp_rect_ = Rect{1, p.height() - prompt_h - h, w, h};
        pk.render(p.sub(comp_rect_), th);
      }
    }
    render_prompt(p.sub(Rect{0, p.height() - prompt_h, p.width(), prompt_h}), th, focused, framed_prompt);
  }

  // --- "/" and "@" --------------------------------------------------------
  // Re-reads the word at the cursor. The lists come from the App, per folder:
  // commands whenever a "/" menu is up, files once per "@" word, since
  // listing a folder is the expensive part.
  void refresh_completion() {
    const bool allowed = !showing_raw() && s_->adapter() && note_q_ < 0 && !note_perm_ && !reply_live_ &&
                         !find_.active() && !prompt_.has_selection();
    const Trigger t = allowed ? find_trigger(prompt_.text(), prompt_.cursor()) : Trigger{};
    if (t.kind == '/') comp_.set_commands(&app_->slash_commands(s_->agent(), s_->cwd()), app_->commands_version());
    if (t.kind == '@' && (files_from_ != t.from || !files_)) {
      files_ = &app_->project_files(s_->cwd());
      files_from_ = t.from;
    }
    if (t.kind != '@') files_ = nullptr;
    if (files_) comp_.set_files(files_, app_->files_version());
    comp_.update(prompt_.text(), prompt_.cursor(), allowed);
  }

  // Puts the item under the menu's cursor in place of the word. From Enter,
  // a command that needs no arguments also runs, the way the agents' own
  // menus do.
  void take_completion(bool enter) {
    Completion::Take t = comp_.take();
    if (t.text.empty()) return;
    const Trigger tr = comp_.trigger();
    // No second space before one already there.
    if (t.text.ends_with(' ') && tr.end < prompt_.text().size() && prompt_.text()[tr.end] == ' ')
      t.text.pop_back();
    prompt_.replace(tr.from, tr.end, t.text);
    if (enter && tr.kind == '/' && t.send) submit_prompt();
    refresh_completion();
  }

  // --- permission dialogs -------------------------------------------------
  // Claude's "Do you want to proceed?" and Codex's "Would you like to run the
  // following command?" (or its "Implement this plan?" when a plan is done)
  // are read off the screen each frame, so the panel shows
  // exactly the choices the agent offers now, and disappears the moment it
  // closes the dialog, whoever answered it.
  void refresh_permission() {
    perm_live_ = s_->spawned() && !s_->exited() && !chat_.has_pending_question() &&
                 screen_permission();
    if (!perm_live_) {
      if (!s_->answer_sending()) perm_key_.clear();
      return;
    }
    // A new dialog starts where claude's cursor is, with no note. The option
    // labels change as a note is typed into them, so they are not the key.
    std::string key = perm_.question;
    for (const auto& t : perm_.title) key += "\n" + t;
    key += "\n" + std::to_string(perm_.options.size());
    const bool fresh = key != perm_key_;
    if (fresh) {
      perm_key_ = std::move(key);
      perm_note_.clear();
    }
    Picker::Options& o = perm_pick_.options();
    o.frame = Picker::Frame::Panel;
    std::string head;
    for (size_t k = 0; k < perm_.title.size(); k++) head += (k ? "  \xC2\xB7  " : "") + perm_.title[k];
    o.title = perm_.plan ? "Plan" : head.empty() ? "Permission" : head;
    o.title_lead = "\xE2\x96\xB2";  // ▲
    o.title_color = app_->theme().attention;
    o.subtitle = perm_.question;
    o.numbers = true;
    o.label_rows = 2;
    o.note = perm_note_;
    o.footer_text = s_->answer_sending()
                        ? "answering\xE2\x80\xA6"
                        : perm_.plan ? "\xE2\x86\x91/\xE2\x86\x93 choose   enter confirm   n note   esc keep planning"
                                     : "\xE2\x86\x91/\xE2\x86\x93 choose   enter confirm   n note   esc reject";
    std::vector<PickItem> items;
    for (size_t k = 0; k < perm_.options.size(); k++) {
      PickItem it;
      it.label = perm_.options[k];
      if (k < perm_.details.size()) it.detail = perm_.details[k];
      if (k < perm_.disabled.size() && perm_.disabled[k]) {
        it.enabled = false;
        it.label += " (unavailable)";
      }
      items.push_back(std::move(it));
    }
    perm_pick_.set_items(std::move(items));
    if (fresh) perm_pick_.set_cursor(perm_.cursor);
  }

  // --- side questions (/btw) ----------------------------------------------
  // Claude answers "/btw …" in a panel of its own that never reaches the
  // transcript. New side answers open the shared Work panel; the main
  // transcript keeps its own scroll position and draft.
  void refresh_btw() {
    btw_live_ = s_->spawned() && !s_->exited() && !perm_live_ &&
                screen_side_panel();
    if (!btw_live_) {
      if (chat_.aside_shown()) chat_.set_aside({}, {}, {});
      return;
    }
    const std::string current = btw_.questions[size_t(btw_.current)];
    if (current != work_last_btw_) {
      work_last_btw_ = current;
      if (prompt_.empty() || work_open_) {
        work_open_ = true; work_tab_ = 2; work_selected_ = "btw:" + current;
      }
    }
    if (chat_.aside_shown()) chat_.set_aside({}, {}, {});
  }

  bool permission_takes_note(int i) const { return mico::permission_takes_note(perm_, i); }

  // Walks the agent's cursor to the choice and confirms it, typing a note into
  // the choice first when it takes one. Paced like a question's answer.
  void answer_permission(int i) {
    if (!perm_live_ || i < 0 || i >= int(perm_.options.size())) return;
    if (s_->answer_sending()) {
      app_->set_status("still answering — F2 to finish in the terminal");
      return;
    }
    PermissionAnswer answer;
    if (!permission_answer(perm_, i, perm_note_, answer)) return;  // a choice shown as off
    if (!s_->send_answer(answer.steps)) {
      app_->set_status("could not answer — F2 to finish in the terminal");
      return;
    }
    answering_question_ = false;
    if (!answer.after.empty()) s_->send_after_answer(std::move(answer.after));
    perm_note_.clear();
    app_->set_status("answering: " + text::oneline(perm_.options[size_t(i)], 60));
  }

  // Rows the panel needs at `width`: heading, question, each choice (a long
  // one on two rows), the note, and the key hint.
  int permission_rows(int width) const { return perm_pick_.rows(width, 1000); }

  // One line per waiting message; past the cap, the last line counts the rest.
  void render_held(Painter p, const Theme& th) {
    p.clear(Style{th.text, th.strip_bg});
    const int rows = p.height();
    const bool overflow = int(held_.size()) > rows;
    for (int i = 0; i < rows; i++) {
      if (overflow && i == rows - 1) {
        p.text(2, i, "+" + std::to_string(held_.size() - size_t(rows - 1)) + " more waiting",
               Style{th.dim, th.strip_bg});
        break;
      }
      const Held& h = held_[size_t(i)];
      // Steering: the agent takes it at its next step. Queued: mico sends it
      // once the agent is done.
      const Style tag = h.steering ? Style{th.accent, th.strip_bg} : Style{th.dim, th.strip_bg};
      int x = p.text(2, i, h.steering ? "\xE2\x86\xAA steering  " : "\xE2\x97\xA6 queued    ", tag);
      p.text_clipped(2 + x, i, h.text, Style{th.text, th.strip_bg}, std::max(0, p.width() - x - 3));
    }
  }

  static std::string preview(const std::vector<MessagePart>& parts) {
    std::string s;
    for (const auto& part : parts) s += part.image ? std::string("[Image]") : part.text;
    return text::oneline(s, 200);
  }

  Badge badge() const override {
    const Theme& th = app_->theme();
    const auto status = s_->status();
    const std::string state = s_->exited() ? "Stopped" : !s_->spawned() ? "Starting"
                            : s_->answer_sending() ? "Sending answers…"
                            : s_->message_sending() ? "Sending…"
                            : chat_.question_submitted()
                                ? (s_->answer_failed() ? "Finish answer · F2 terminal" : "Awaiting agent · F2 terminal")
                            : status == LiveSession::Status::Waiting
                                ? (chat_.has_pending_question() || perm_live_ ? "Needs you" : "Needs you · F2 terminal")
                            : status == LiveSession::Status::Working
                                ? (chat_.open_async_questions() ? "Working · question for you" : "Working")
                            : chat_.open_async_questions() ? "Question for you" : "Ready";
    // The same glyphs and colours the chat list uses, so a state reads the
    // same wherever it is shown.
    const bool working_now = status == LiveSession::Status::Working;
    const Color color = s_->exited() ? th.dim : s_->needs_input() ? th.attention
                      : working_now ? th.working : th.accent;
    const char* glyph = s_->exited() ? "·"
                      : s_->needs_input() ? "●"
                      : working_now ? "◍"
                                    : "○";
    return Badge{std::string(glyph) + " " + state, color};
  }

  bool captures_keys() const override { return true; }

  // Turns a committed choice into the agent's own menu keys and delivers it to
  // the pty. The transcript is never touched.
  void deliver_answer(const ChatRenderer::Answer& a) {
    if (a.card.questions.empty()) return;
    if (a.card.async) {
      deliver_async_answer(a);
      return;
    }
    std::vector<bool> multi;
    std::vector<int> recommended, options;
    for (const auto& q : a.card.questions) {
      multi.push_back(q.multi);
      recommended.push_back(q.recommended);
      options.push_back(int(q.options.size()));
    }
    const auto steps = question_answer_steps(s_->driver(), multi, recommended, options, a.chosen);
    if (s_->driver().menu_keys().paced) {
      if (!s_->send_answer(steps)) {
        app_->set_status("could not send answers — F2 to finish in the terminal");
        return;
      }
      answering_question_ = true;
    } else {
      for (const auto& step : steps) s_->pty().write(step);
    }
    // The agents' menus have no room for notes (claude's only on questions
    // with previews), so they follow the answer as a message.
    if (std::string notes = notes_message(a); !notes.empty()) s_->send_after_answer(std::move(notes));

    std::string what = a.card.questions[0].header;
    if (a.chosen.size() > 1) {
      char b[48];
      snprintf(b, sizeof b, "%zu answers", a.chosen.size());
      what = b;
    } else {
      const auto& q = a.card.questions[0];
      const std::vector<uint8_t> empty;
      const std::vector<uint8_t>& chosen = a.chosen.empty() ? empty : a.chosen[0];
      for (int i = 0; i < int(chosen.size()) && i < int(q.options.size()); i++)
        if (chosen[size_t(i)] && !q.options[size_t(i)].label.empty()) {
          what = q.options[size_t(i)].label;
          break;
        }
    }
    if (what.empty()) what = a.card.tool;
    app_->set_status("sending answer: " + what);
  }

  // The notes on an answer, as one message: each names the question it is
  // about, since the agent reads it apart from the answer itself.
  static std::string notes_message(const ChatRenderer::Answer& a) {
    std::string out;
    for (size_t qi = 0; qi < a.notes.size() && qi < a.card.questions.size(); qi++) {
      if (a.notes[qi].empty()) continue;
      if (!out.empty()) out += "\n\n";
      out += "Note on my answer to \"" + a.card.questions[qi].text + "\": " + a.notes[qi];
    }
    return out;
  }

  // Puts `text` in the box as if typed, newlines included.
  void insert_text(std::string_view text) {
    for (size_t i = 0; i < text.size();) {
      const char32_t cp = text::decode(text, i);
      if (cp == '\n') prompt_.insert_newline();
      else prompt_.insert_char(cp);
    }
  }

  // An optional question is not a menu on the agent's screen: codex already
  // returned from the call. The answer is a message, byte for byte what codex
  // writes when its own UI answers one: the envelope naming the question by
  // its call, so codex clears the question from its own screen too, and the
  // card recognises it when it lands. A card without the call id (an older
  // transcript) gets the older quoted form.
  void deliver_async_answer(const ChatRenderer::Answer& a) {
    if (s_->exited()) {
      app_->set_status("agent has exited — resume the chat to answer");
      return;
    }
    if (a.free_text) {
      const size_t qi = size_t(std::clamp(a.free_q, 0, int(a.card.questions.size()) - 1));
      if (!prompt_.empty()) {
        suspended_prompt_ = std::move(prompt_);
        prompt_ = PromptEditor{};
        prompt_suspended_ = true;
      }
      reply_ = AsyncReply{a.card.call_id, int(qi), a.card.questions[qi].text, {}};
      reply_live_ = true;
      app_->set_status("write your answer — Enter sends it");
      return;
    }
    std::vector<AsyncReply> replies;
    std::string first;
    for (size_t qi = 0; qi < a.card.questions.size() && qi < a.chosen.size(); qi++) {
      const auto& q = a.card.questions[qi];
      std::string picked;
      for (size_t oi = 0; oi < q.options.size() && oi < a.chosen[qi].size(); oi++)
        if (a.chosen[qi][oi]) {
          if (!picked.empty()) picked += ", ";
          picked += q.options[oi].label;
        }
      if (picked.empty()) continue;
      if (first.empty()) first = picked;
      replies.push_back(AsyncReply{a.card.call_id, int(qi), q.text, picked});
    }
    if (replies.empty()) return;
    send_async_reply(s_->driver().async_reply(replies));
    app_->set_status("sending answer: " + first);
  }

  void send_async_reply(const std::string& msg) {
    s_->pty().write(encode_paste(msg, s_->vt().bracketed_paste()));
    s_->pty().write("\r");
    chat_.to_bottom();
  }

  // Pastes longer than this are held as one "[Pasted N characters]" token
  // rather than poured into the box: a long log or file is not something to
  // edit line by line in a four-row prompt, and it buries what was typed
  // around it.
  struct Held {
    bool steering;     // held by the agent, not by mico
    std::string text;  // one line of it
  };
  std::vector<Held> held_;  // rebuilt every frame from the session and transcript
  static constexpr int kMaxHeldRows = 3;

  static constexpr size_t kCollapseChars = 1000;
  static constexpr size_t kCollapseLines = 10;

  static std::string grouped(size_t n) {
    std::string s = std::to_string(n);
    for (int i = int(s.size()) - 3; i > 0; i -= 3) s.insert(size_t(i), ",");
    return s;
  }

  void insert_image(const std::string& path) {
    prompt_.insert_attachment(
        {true, path, "[Image #" + std::to_string(prompt_.image_count() + 1) + "]"});
    prompt_.insert_char(U' ');
  }

  void insert_pasted(std::string_view t) {
    // A copied image file arrives as its path (or a file:// URI). Taken as an
    // attachment it reaches the agent as an image, not as a path to read.
    if (const auto images = clip::image_paths(t); !images.empty()) {
      for (const auto& path : images) insert_image(path);
      return;
    }
    size_t chars = 0, lines = 1;
    for (size_t i = 0; i < t.size();) {
      const char32_t cp = text::decode(t, i);
      if (cp == '\n') lines++;
      if (cp != '\r') chars++;
    }
    if (chars > kCollapseChars || lines > kCollapseLines) {
      std::string body;
      body.reserve(t.size());
      for (char c : t) if (c != '\r') body.push_back(c);  // CRLF -> LF
      prompt_.insert_attachment({false, std::move(body), "[Pasted " + grouped(chars) + " characters]"});
      return;
    }
    for (size_t i = 0; i < t.size();) {
      const char32_t cp = text::decode(t, i);
      if (cp == '\r') continue;  // CRLF -> LF
      if (cp == '\n') prompt_.insert_newline();
      else prompt_.insert_char(cp);
    }
  }

  void paste_clipboard() {
    const clip::Content c = clip::read();
    switch (c.kind) {
      case clip::Content::Kind::Image:
        insert_image(c.image_path);
        app_->set_status("image attached");
        break;
      case clip::Content::Kind::Text: insert_pasted(c.text); break;
      case clip::Content::Kind::NoTool: app_->set_status(c.error); break;
      case clip::Content::Kind::Empty:
        app_->set_status(c.error.empty() ? "nothing on the clipboard to paste" : c.error);
        break;
    }
  }

  // Sends the prompt now. While the agent works that is steering: claude
  // takes it at its next tool call, codex at once.
  bool send_prompt() {
    // An open side-question panel would take the typing as its own keys
    // (c copies, f forks, x clears): it is closed first.
    if (!s_->send_parts(prompt_.parts(), btw_live_ ? "\x1b" : "")) {
      app_->set_status("still sending the previous message");
      return false;
    }
    return true;
  }

  // Alt+Enter: holds the prompt until the agent has finished what it is doing.
  void queue_prompt() {
    if (prompt_.empty()) return;
    if (s_->exited()) {
      app_->set_status("agent has exited — resume the chat to send this draft");
      return;
    }
    s_->enqueue(prompt_.parts());
    prompt_.reset();
    chat_.to_bottom();
    app_->set_status(s_->busy() ? "queued — sent when " + agent_label(s_->agent()) + " finishes"
                                : "queued");
  }

  // ↑ on an empty box recalls what you sent before, newest first, read out
  // of the transcript — so it survives a daemon restart and covers turns
  // sent from the agent's own terminal. ↓ steps back toward the empty box.
  // Editing the recalled text ends the browsing: it is a draft now, and the
  // arrows go back to moving in it.
  bool history_step(int dir) {
    if (hist_at_ >= 0 && prompt_.text() != hist_shown_) hist_at_ = -1;
    if (hist_at_ < 0) {
      if (dir > 0 || !prompt_.empty()) return false;
      std::vector<ChatRenderer::OutlineEntry> entries;
      chat_.outline(entries, 60);
      hist_.clear();
      for (const auto& e : entries)
        if (e.kind == 'u') hist_.push_back(e.offset);
      // A chat with nothing said yet: leave ↑ to the transcript, as before.
      if (hist_.empty()) return false;
      hist_at_ = int(hist_.size());
    }
    for (int i = hist_at_ + dir;; i += dir) {
      if (i < 0) {
        app_->set_status("no earlier message");
        return true;
      }
      if (i >= int(hist_.size())) {
        prompt_.clear();
        hist_at_ = -1;
        return true;
      }
      std::string t;
      // A line that reads as no message, and the same text again, both skip.
      if (!chat_.user_text_at(hist_[size_t(i)], &t) || t == prompt_.text()) continue;
      prompt_.clear();
      insert_text(t);
      hist_shown_ = prompt_.text();
      hist_at_ = i;
      return true;
    }
  }

  // ↑ in an empty box: the newest queued message back into the box, to edit
  // or send differently.
  bool unqueue_to_prompt() {
    const auto parts = s_->take_last_queued();
    if (parts.empty()) return false;
    for (const auto& p : parts) {
      if (p.image) insert_image(p.text);
      else insert_pasted(p.text);
    }
    app_->set_status("took the queued message back");
    return true;
  }


  // A paste goes to the agent verbatim in raw view — wrapped back up as a
  // bracketed paste when the agent asked for one, so multi-line text is not a
  // burst of Enters. In chat view it lands in the prompt box, newlines and all.
  bool on_paste(std::string_view t) override {
    if (work_open_ && work_focus_ && work_tab_ == 2) {
      const std::string line = text::oneline(t, 2000);
      for (size_t at = 0; at < line.size();) work_prompt_.insert_char(text::decode(line, at));
      return true;
    }
    if (showing_raw()) {
      s_->pty().write(encode_paste(std::string(t), s_->vt().bracketed_paste()));
      return true;
    }
    if (find_.active()) {
      find_.on_paste(t, chat_);
      return true;
    }
    question_focus_ = false;
    insert_pasted(t);
    return true;
  }

  bool on_key(const KeyEvent& k) override {
    // F2 toggles the plane. It has to be a key no agent uses, because in raw
    // view everything else is forwarded verbatim.
    if (k.key == Key::F2) { toggle_view(); return true; }
    if (k.key == Key::F11 && !showing_raw()) {
      work_open_ = !work_open_; work_focus_ = work_open_;
      return true;
    }
    if (work_open_ && work_focus_ && !showing_raw() && work_key(k)) return true;
    if (s_->answer_sending()) {
      if (k.key == Key::Escape) {
        s_->cancel_answer();
        s_->pty().write("\x1b");
        app_->set_status("answer delivery cancelled");
      } else app_->set_status("sending answers — F2 to finish in the terminal");
      return true;
    }

    // Raw view and a fresh session's startup flow both forward every key to
    // the agent verbatim.
    if (showing_raw()) {
      s_->pty().write(encode_key(k, s_->vt().app_cursor_keys()));
      return true;
    }

    // Find in this chat owns the keyboard while its bar is open.
    if (find_.on_key(k, chat_)) {
      if (find_.wants_all_chats()) app_->open_search(find_.query());
      return true;
    }
    if (k.ctrl && !k.alt && (k.ch == 'f' || k.ch == 'F') && k.key == Key::Char) {
      find_.open(chat_);
      return true;
    }
    // Ctrl+K: any chat, by name. Only on an empty box: with text in it,
    // Ctrl+K is the editing key that deletes to the end of the line.
    if (k.is_ctrl('k') && prompt_.empty() && note_q_ < 0 && !note_perm_ && !reply_live_) {
      app_->open_switcher();
      return true;
    }
    // Ctrl+G: the chat's outline, to go to any message, edit or failure.
    if (k.is_ctrl('g')) {
      open_outline(app_, this, chat_);
      return true;
    }
    // Alt/Ctrl+↑↓: the previous or next thing you said, whatever else is open.
    if ((k.alt || k.ctrl) && !k.shift && (k.key == Key::Up || k.key == Key::Down)) {
      chat_.jump_user(k.key == Key::Up ? -1 : 1);
      return true;
    }

    if (k.key == Key::Left && k.shift && !k.ctrl && !k.alt &&
        note_q_ < 0 && !note_perm_ && !reply_live_ && s_->agent() == "codex") {
      if (chat_.focus_question()) {
        question_focus_ = true;
        app_->set_status("question: arrows choose, Enter answers, Esc returns to your draft");
        return true;
      }
    }
    if (question_focus_) {
      if (k.key == Key::Escape) {
        question_focus_ = false;
        return true;
      }
      if (chat_.question_active() && !k.ctrl && !k.alt && chat_.question_key(k)) {
        ChatRenderer::Answer a;
        if (chat_.take_answer(a)) { question_focus_ = false; deliver_answer(a); }
        return true;
      }
      question_focus_ = false;
    }

    if (note_key(k)) return true;

    if (completion_key(k)) return true;

    // A permission dialog owns the choosing keys while the box is empty.
    // Escape is left to the agent below: it is claude's own "reject".
    // The page keys still scroll the transcript behind it.
    const bool choosing = k.key == Key::Up || k.key == Key::Down || k.key == Key::Tab ||
                          k.key == Key::BackTab || k.key == Key::Enter ||
                          (k.key == Key::Char && !k.ctrl && !k.alt && k.ch >= '1' && k.ch <= '9');
    if (perm_live_ && !note_perm_ && prompt_.empty() && choosing) {
      switch (perm_pick_.on_key(k)) {
        case Picker::Result::Chosen: answer_permission(perm_pick_.index()); return true;
        case Picker::Result::Ignored: break;  // a number past the last choice
        default: return true;
      }
    }

    if (btw_key(k)) return true;

    // A live question card owns the navigation keys while the prompt box is
    // empty, so the arrows choose an option rather than scroll the transcript.
    if (note_q_ < 0 && !reply_live_ && prompt_.empty() && chat_.question_active()) {
      if (chat_.question_key(k)) {
        ChatRenderer::Answer a;
        if (chat_.take_answer(a)) deliver_answer(a);
        return true;
      }
    }

    return prompt_key(k);
  }

  // The box when it holds something other than a message: an answer to an
  // optional question in your own words, or a note on a card. And `n`, on an
  // empty box, starts a note.
  bool note_key(const KeyEvent& k) {
    // Answering in your own words: Enter sends the box as the answer, Escape
    // goes back to the card.
    if (reply_live_) {
      if (k.key == Key::Enter && !k.alt) {
        std::string text = prompt_.text();
        while (!text.empty() && std::isspace(uint8_t(text.back()))) text.pop_back();
        if (text.empty()) return true;
        if (s_->exited()) {
          app_->set_status("agent has exited — resume the chat to answer");
          return true;
        }
        reply_.answer = std::move(text);
        send_async_reply(s_->driver().async_reply({reply_}));
        app_->set_status("sending answer");
        prompt_.reset();
        reply_live_ = false;
        restore_suspended_prompt();
        return true;
      }
      if (k.key == Key::Escape) {
        prompt_.reset();
        reply_live_ = false;
        restore_suspended_prompt();
        return true;
      }
    }

    // Writing a note: the box holds the note, Enter puts it on the card and
    // Escape leaves the card's note as it was.
    if (note_perm_) {
      if (k.key == Key::Enter && !k.alt) {
        perm_note_ = prompt_.text();
        while (!perm_note_.empty() && std::isspace(uint8_t(perm_note_.back()))) perm_note_.pop_back();
        prompt_.reset();
        note_perm_ = false;
        app_->set_status(perm_note_.empty() ? "note removed" : "note saved — it goes with your answer");
        return true;
      }
      if (k.key == Key::Escape) {
        prompt_.reset();
        note_perm_ = false;
        return true;
      }
    } else if (note_q_ >= 0) {
      if (k.key == Key::Enter && !k.alt) {
        chat_.set_question_note(note_q_, prompt_.text());
        prompt_.reset();
        note_q_ = -1;
        app_->set_status("note saved — it goes with your answer");
        return true;
      }
      if (k.key == Key::Escape) {
        prompt_.reset();
        note_q_ = -1;
        return true;
      }
    } else if (reply_live_) {
      // typing the answer
    } else if (prompt_.empty() && k.is('n') && !k.ctrl && !k.alt && perm_live_) {
      note_perm_ = true;
      note_header_ = perm_.question;
      insert_text(perm_note_);
      return true;
    } else if (prompt_.empty() && k.is('n') && !k.ctrl && !k.alt) {
      std::string header;
      const int qi = chat_.question_note_target(&header);
      if (qi >= 0) {
        note_q_ = qi;
        note_header_ = header;
        insert_text(chat_.question_note(qi));
        return true;
      }
    }
    return false;
  }

  // An open "/" or "@" menu takes the keys that move and choose in it;
  // everything else still edits the box, which narrows the menu.
  bool completion_key(const KeyEvent& k) {
    refresh_completion();
    if (comp_.visible()) {
      const bool plain = !k.ctrl && !k.alt;
      switch (k.key) {
        case Key::Up: case Key::Down: case Key::PageUp: case Key::PageDown:
          if (k.shift) break;
          comp_.picker().on_key(k);
          return true;
        case Key::Tab:
          take_completion(false);
          return true;
        case Key::Enter:
          if (k.shift || !plain) break;
          take_completion(true);
          return true;
        case Key::Escape:
          comp_.dismiss();
          return true;
        case Key::Char:
          if (k.ctrl && !k.alt && (k.ch == 'p' || k.ch == 'n')) {
            comp_.picker().on_key(k);
            return true;
          }
          break;
        default: break;
      }
    }
    return false;
  }

  // The side-question panel owns its keys while the box is empty: Esc
  // closes it, the arrows scroll it, Shift+←/→ go through earlier ones.
  bool btw_key(const KeyEvent& k) {
    if (btw_live_ && prompt_.empty() && note_q_ < 0 && !note_perm_ && !reply_live_ && !k.ctrl && !k.alt) {
      const char* seq = nullptr;
      switch (k.key) {
        case Key::Escape: seq = "\x1b"; break;
        case Key::Up: seq = k.shift ? nullptr : "\x1b[A"; break;
        case Key::Down: seq = k.shift ? nullptr : "\x1b[B"; break;
        case Key::Left: seq = k.shift ? "\x1b[1;2D" : nullptr; break;
        case Key::Right: seq = k.shift ? "\x1b[1;2C" : nullptr; break;
        default: break;
      }
      if (seq) {
        s_->pty().write(seq);
        if (k.key == Key::Escape) app_->set_status("closed the side question");
        return true;
      }
    }
    return false;
  }

  // Chat view: navigation scrolls the transcript, text goes to the prompt
  // box — except once the box has something in it (or a selection, or more
  // than one line), in which case the arrows edit the box instead. That
  // keeps an empty box's Up/Down/End behaving exactly as before.
  bool prompt_key(const KeyEvent& k) {
    switch (k.key) {
      case Key::Escape:
        // The agent's own status line offers "esc to interrupt"; that has to
        // keep working when its status line is not the thing on screen.
        if (!prompt_.empty()) { prompt_.clear(); return true; }
        s_->pty().write("\x1b");
        app_->set_status("sent esc to the agent");
        return true;
      case Key::End:
        if (prompt_.empty()) return chat_.on_key(k);
        break;
      case Key::Up:
        if (prompt_.empty() && !k.shift && unqueue_to_prompt()) return true;
        if (!k.shift && history_step(-1)) return true;
        if (prompt_.move_display_line(-1, prompt_w_, k.shift)) return true;
        return chat_.on_key(k);
      case Key::Down:
        if (!k.shift && history_step(1)) return true;
        if (prompt_.move_display_line(1, prompt_w_, k.shift)) return true;
        return chat_.on_key(k);
      case Key::PageUp: case Key::PageDown:
        return chat_.on_key(k);
      case Key::Enter: {
        // Shift+Enter, from a terminal that reports it, is a new line.
        if (k.shift) { prompt_.insert_newline(); return true; }
        submit_prompt();
        return true;
      }
      case Key::Char:
        // Ctrl+J inserts a literal newline — Enter still sends.
        if (k.ctrl && (k.ch == 'j' || k.ch == 'J')) { prompt_.insert_newline(); return true; }
        break;
      default:
        break;
    }
    // With text selected in the box, Ctrl+C and Ctrl+X copy and cut it.
    // Without, Ctrl+C still interrupts the agent.
    std::string copied;
    if (prompt_.edit_key(k, &copied)) {
      if (!copied.empty()) app_->copy_to_clipboard(std::move(copied));
      return true;
    }
    if (k.key != Key::Char) return false;
    if (k.ctrl && (k.ch == 'v' || k.ch == 'V')) { paste_clipboard(); return true; }
    // Alt+Enter arrives as ESC CR: queue rather than send.
    if (k.alt && (k.ch == '\r' || k.ch == '\n')) { queue_prompt(); return true; }
    // Every Ctrl combination the box did not take goes to the agent, so an
    // interrupt or the agent's own binding is never swallowed.
    if (k.ctrl) { s_->pty().write(encode_key(k, false)); return true; }
    prompt_.insert_char(k.ch);
    return true;
  }

  // Enter on the box: the message goes to the agent, or waits with a reason.
  void submit_prompt() {
    if (prompt_.empty()) return;
    if (chat_.question_submitted()) {
      app_->set_status("waiting for the question result — F2 to finish in the terminal");
      return;
    }
    if (s_->exited()) {
      app_->set_status("agent has exited — resume the chat to send this draft");
      return;
    }
    if (!send_prompt()) return;
    prompt_.reset();
    chat_.to_bottom();
  }

  std::string take_url() override {
    auto url = work_chat_.take_url();
    return url.empty() ? chat_.take_url() : url;
  }

  // At the strip's right end, what the agent runs in the background, as its
  // own footer says it ("2 monitors still running"): a chip that lists them.
  void draw_background(Painter& p, const Theme& th) {
    bool need = false;
    for (const auto& q : app_->workspace().side_questions()) if (q.parent == s_->serial() && q.context == s_->session_id())
      for (const auto& child : app_->workspace().live()) if (child->serial() == q.child && child->needs_input()) need = true;
    const std::string text = need ? "Work ! F11 " : s_->background().empty() ? "Work F11 " :
        background_summary(s_->background()) + " running" + background_percent(s_->background()) + " ";
    const int w = text::str_width(text) + 3;
    int used = 0;
    for (const auto& c : chips_) used = std::max(used, c.rect.x + c.rect.w);
    const int x = p.width() - w - 1;
    if (x < used + 2) return;
    p.put(x, 0, U'\u25C9', Style{th.working, th.strip_bg, attr::kBold});  // ◉
    p.text(x + 2, 0, text, Style{th.working, th.strip_bg});
    p.text(x + 2 + text::str_width(text), 0, "\xE2\x96\xBE", Style{th.accent, th.strip_bg});
    chips_.push_back(ChatRenderer::Chip{Rect{x, 0, w, 1}, "background"});
  }

  bool on_mouse(const MouseEvent& m, Point local) override {
    if (work_open_ && work_rect_.contains(local) && !showing_raw()) {
      work_focus_ = true;
      return work_mouse(m, Point{local.x - work_rect_.x, local.y - work_rect_.y});
    }
    if (m.kind == MouseKind::Press) work_focus_ = false;
    if (!showing_raw() && comp_rect_.w > 0 && comp_rect_.contains(local)) {
      const Picker::Result r =
          comp_.picker().on_mouse(m, Point{local.x - comp_rect_.x, local.y - comp_rect_.y});
      if (r == Picker::Result::Chosen) take_completion(false);
      return true;
    }
    if (!showing_raw() && chip_row_ >= 0 && local.y == chip_row_ &&
        m.kind == MouseKind::Press) {
      for (const auto& c : chips_)
        if (local.x >= c.rect.x && local.x < c.rect.x + c.rect.w) {
          const Point chip{m.pos.x - local.x + c.rect.x, m.pos.y};
          if (c.key == "background") {
            work_open_ = true; work_focus_ = true; work_tab_ = s_->background().empty() ? 0 : 1;
            return true;
          }
          open_chip_picker(app_, this, chip, chat_.state(), c.key, !s_->exited(), s_->agent());
          return true;
        }
      return true;
    }
    if (!showing_raw() && perm_live_ && perm_top_ >= 0 && local.y >= perm_top_ &&
        local.y - perm_top_ < perm_h_) {
      if (m.kind != MouseKind::Press || m.button == MouseButton::Left)
        if (perm_pick_.on_mouse(m, Point{local.x, local.y - perm_top_}) == Picker::Result::Chosen)
          answer_permission(perm_pick_.index());
      return true;
    }
    if (!showing_raw()) {
      if (local.y < body_top_) return false;
      const bool used = chat_.on_mouse(m, Point{local.x, local.y - body_top_});
      ChatRenderer::Answer answer;
      if (chat_.take_answer(answer)) deliver_answer(answer);
      if (chat_.grabbing()) app_->capture_mouse(this);
      return used;
    }

    if (m.kind == MouseKind::WheelUp || m.kind == MouseKind::WheelDown) {
      // Only hand the wheel to the agent if it asked for mouse reporting;
      // otherwise it scrolls our scrollback.
      if (s_->vt().wants_mouse()) {
        s_->pty().write(encode_mouse(m, s_->vt().sgr_mouse()));
      } else {
        raw_scroll_ += m.kind == MouseKind::WheelUp ? 3 : -3;
        raw_scroll_ = std::max(0, raw_scroll_);
      }
      return true;
    }
    if (s_->vt().wants_mouse()) {
      MouseEvent fwd = m;
      fwd.pos = local;  // the agent thinks its screen starts at our pane origin
      s_->pty().write(encode_mouse(fwd, s_->vt().sgr_mouse()));
    }
    return true;
  }

  std::vector<MenuItem> context_menu(Point local) override {
    if (work_open_ && work_rect_.contains(local)) {
      std::vector<MenuItem> items;
      if (const auto* i = work_.find(work_selected_)) {
        items.push_back(MenuItem{"Copy output", "work_copy"});
        if (!i->path.empty()) items.push_back(MenuItem{"Open conversation", "work_full"});
        else if (i->kind != "btw") items.push_back(MenuItem{"Show in chat", "work_source"});
      }
      items.push_back(MenuItem{"Close Work panel", "work", true, false, false, "", "F11"});
      return items;
    }
    // A right-click on a state chip opens that chip's menu, the same as a
    // left-click does.
    if (!showing_raw() && chip_row_ >= 0 && local.y == chip_row_) {
      for (const auto& c : chips_)
        if (local.x >= c.rect.x && local.x < c.rect.x + c.rect.w) {
          if (c.key == "background") {
            std::vector<MenuItem> items;
            for (const PickItem& it : background_items(s_->background(), app_->theme())) {
              MenuItem mi{it.label, it.id};
              mi.detail = it.detail;
              items.push_back(std::move(mi));
            }
            return items;
          }
          return chip_menu(chat_.state(), c.key, !s_->exited(), s_->agent());
        }
    }

    // What was clicked, then the chat, then stopping it. Only what applies is
    // listed: the view's own settings are Settings' and the commands'.
    std::vector<MenuItem> items;
    if (showing_raw()) {
      items.push_back(MenuItem{"Copy visible screen", "copy_screen"});
      items.push_back(MenuItem{"Chat view", "toggle_view", true, false, false, "", "F2"});
    } else {
      items = chat_.context_menu(Point{local.x, local.y - body_top_});
      if (!items.empty()) items.push_back(MenuItem::sep());
      items.push_back(MenuItem{"Outline\xE2\x80\xA6", "outline", true, false, false, "", "Ctrl+G"});
      items.push_back(MenuItem{"Work panel", "work", true, false, work_open_, "", "F11"});
    }
    // Forking a live session branches from wherever it is now. The id has to
    // be known, which for codex means waiting for its rollout to be found.
    if (s_->adapter() && !s_->session_id().empty()) {
      items.push_back(MenuItem{"Fork into a new chat", "fork_self"});
      if (s_->exited()) items.push_back(MenuItem{"Resume chat", "resume_self"});
    }
    if (!s_->exited()) {
      items.push_back(MenuItem::sep());
      items.push_back(MenuItem{"Stop agent", "stop"});
    }
    return items;
  }

  void on_action(const std::string& a) override {
    if (a == "work") { work_open_ = !work_open_; work_focus_ = work_open_; return; }
    if (a == "work_copy" || a == "work_full" || a == "work_source") {
      if (const auto* i = work_.find(work_selected_)) {
        if (a == "work_copy") app_->copy_to_clipboard(work_.detail(*i, chat_.conversation()));
        else if (a == "work_full" && !i->path.empty()) app_->open_at(i->path, 0, {});
        else { chat_.reveal(i->at, {}); work_focus_ = false; }
      }
      return;
    }
    if (a == "toggle_view") { toggle_view(); return; }
    if (a.starts_with("bg:")) {
      // Where the task was started: the call, in this chat.
      if (showing_raw()) toggle_view();
      chat_.reveal(std::strtoull(a.c_str() + 3, nullptr, 10), {});
      return;
    }
    if (a == "outline") {
      if (!showing_raw()) open_outline(app_, this, chat_);
      return;
    }
    if (handle_outline_action(app_, this, chat_, a)) return;
    if (s_->answer_sending() && a.starts_with("chip")) {
      app_->set_status("wait for answer delivery before changing agent settings");
      return;
    }
    if (a.rfind("chipkey:", 0) == 0) {
      // A raw key the agent uses to change a setting (Shift+Tab for the
      // permission cycle). No optimistic update — the next click cycles again.
      s_->pty().write(a.substr(8));
      app_->set_status("sent \xE2\x87\xA7 Tab");
      return;
    }
    if (a.rfind("chipmode:", 0) == 0) {
      // "chipmode:<key>|<value>|<shift-tab bytes>" — walk Claude's permission
      // ring to a named stop (default / accept edits / plan) and show it now.
      const std::string rest = a.substr(9);
      const size_t b1 = rest.find('|'), b2 = rest.find('|', b1 + 1);
      const std::string key = rest.substr(0, b1);
      const std::string val = rest.substr(b1 + 1, b2 - b1 - 1);
      const std::string keys = rest.substr(b2 + 1);
      if (!keys.empty()) s_->pty().write(keys);
      chat_.set_state(key, val);
      app_->set_status(val == "acceptEdits" ? "mode: accept edits"
                       : val == "plan"      ? "mode: plan"
                                            : "mode: default");
      return;
    }
    if (a.rfind("chipsteps:", 0) == 0) {
      // "chipsteps:<key>|<value>|<steps>": keys that set it, steps apart by
      // \x1f, taken one at a time as the screen settles (ChipControl::steps).
      const std::string rest = a.substr(10);
      const size_t b1 = rest.find('|'), b2 = rest.find('|', b1 + 1);
      if (b1 == std::string::npos || b2 == std::string::npos) return;
      const std::string key = rest.substr(0, b1), val = rest.substr(b1 + 1, b2 - b1 - 1);
      const std::string body = rest.substr(b2 + 1);
      std::vector<std::string> keys;
      for (size_t at = 0; at <= body.size();) {
        size_t end = body.find('\x1f', at);
        if (end == std::string::npos) end = body.size();
        keys.push_back(body.substr(at, end - at));
        at = end + 1;
      }
      if (!s_->send_answer(std::move(keys))) {
        app_->set_status("busy; try again in a moment");
        return;
      }
      chat_.set_state(key, val);
      app_->set_status("set " + key + " " + val);
      return;
    }
    if (a.rfind("chipset:", 0) == 0) {
      // A concrete value chosen from the chip menu: send it as the agent's own
      // set command, stay in the chat, and update the chip now — the agent
      // writes the change into its transcript only on its next turn.
      const std::string line = a.substr(8);
      s_->pty().write(line);
      s_->pty().write("\r");
      const size_t sp = line.find(' ');
      if (sp != std::string::npos) {
        const std::string cmd = line.substr(0, sp), val = line.substr(sp + 1);
        const char* key = cmd == "/model"      ? "model"
                          : cmd == "/effort"    ? "effort"
                          : cmd == "/permissions" ? "perm"
                                                  : nullptr;
        if (key) chat_.set_state(key, val);
      }
      app_->set_status("set " + line.substr(1));
      return;
    }
    if (a.rfind("chipcmd:", 0) == 0) {
      // Open the agent's own picker for a chip mico does not enumerate.
      const std::string cmd = a.substr(8);
      // Let its command completion settle before Enter. A single PTY read
      // containing both can be treated as pasted text by the agent's TUI.
      if (!s_->send_answer({cmd, "\r"})) {
        app_->set_status("busy; try again in a moment");
        return;
      }
      view_ = View::Raw;
      view_chosen_ = true;
      app_->set_status("opened " + cmd);
      return;
    }
    if (a == "fork_self") {
      app_->spawn_continuation(s_->agent(), s_->session_id(), s_->cwd(), true);
      return;
    }
    if (a == "resume_self") {
      app_->spawn_continuation(s_->agent(), s_->session_id(), s_->cwd(), false);
      return;
    }
    if (a == "stop") { s_->pty().terminate(); return; }
    if (a == "close") { app_->close_session(s_); return; }
    if (a == "copy_screen") {
      app_->copy_to_clipboard(visible_text());
      app_->set_status("copied screen to clipboard");
      return;
    }
    chat_action(*app_, chat_, a);
  }

  // The emulator's visible rows as plain text, trailing blanks trimmed.
  std::string visible_text() const {
    const Vt& vt = s_->vt();
    const int rows = vt.height();
    const int total = vt.total_rows();
    const int first = std::max(0, total - rows - raw_scroll_);
    std::string out;
    for (int y = first; y < std::min(total, first + rows); y++) {
      std::string line;
      for (const Cell& c : vt.row(y)) {
        if (c.width == 0) continue;
        text::encode(c.cp ? c.cp : U' ', line);
      }
      while (!line.empty() && line.back() == ' ') line.pop_back();
      out += line;
      out += '\n';
    }
    return out;
  }

 private:
  WorkSet work_;
  ChatRenderer work_chat_;
  bool work_open_ = false, work_focus_ = false;
  int work_tab_ = 0, work_scroll_ = 0, work_width_ = 0;
  std::string work_selected_, work_last_btw_, work_text_, work_laid_out_;
  PromptEditor work_prompt_;
  Rect work_rect_{}, work_body_{};
  std::vector<std::pair<int, std::string>> work_hits_;
  int64_t work_detail_ms_ = 0;
  Arena work_arena_;
  std::vector<md::Seg> work_segs_, work_inline_;
  std::vector<md::Line> work_rows_;
  std::vector<text::Span> work_spans_;
  md::Work work_layout_;

  const char* work_kind() const { return work_tab_ == 0 ? "agent" : work_tab_ == 1 ? "terminal" : "btw"; }
  std::vector<const WorkItem*> work_items() const {
    std::vector<const WorkItem*> out;
    for (const auto& i : work_.items()) if (i.kind == work_kind()) out.push_back(&i);
    return out;
  }
  void select_work(const std::string& id) {
    if (id == work_selected_) return;
    work_selected_ = id; work_scroll_ = 0; work_detail_ms_ = 0; work_text_.clear();
    work_laid_out_.clear();
  }

  void render_work(Painter p, bool focused) {
    const Theme& th = app_->theme();
    p.clear(Style{th.text, th.panel});
    p.vline(0, 0, p.height(), U'│', Style{focused ? th.border_focus : th.border, th.panel});
    p.text_clipped(2, 0, "Work", Style{th.accent, th.panel, attr::kBold}, p.width() - 4);
    if (p.width() > 25) p.text(p.width() - 13, 0, "F11 close", Style{th.dim, th.panel});
    for (int n = 0; n < 3; n++) {
      const int x = 2 + n * std::max(1, (p.width() - 3) / 3);
      p.text_clipped(x, 2, n == 0 ? "Agents" : n == 1 ? "Terminals" : "Btw",
                     Style{n == work_tab_ ? th.accent : th.dim, th.panel, n == work_tab_ ? attr::kBold : attr::kNone},
                     std::max(1, (p.width() - 3) / 3 - 1));
    }
    work_.refresh(chat_.conversation(), s_, app_->workspace());
    const auto items = work_items();
    if (std::none_of(items.begin(), items.end(), [&](const WorkItem* i) { return i->id == work_selected_; }))
      select_work(items.empty() ? std::string() : items.front()->id);
    work_hits_.clear();
    int y = 4;
    const int list_end = std::min(p.height() / 2, 14);
    // Keep the selected item in the visible part of a long list.
    size_t start = 0;
    for (size_t n = 0; n < items.size(); n++) if (items[n]->id == work_selected_) {
      const size_t visible = size_t(std::max(1, (list_end - 4) / 2));
      if (n >= visible) start = n - visible + 1;
    }
    for (size_t n = start; n < items.size() && y + 1 < list_end; n++) {
      const WorkItem& i = *items[n];
      const bool on = i.id == work_selected_;
      if (on) p.fill(Rect{1, y, p.width() - 1, 2}, Style{th.text, th.panel_alt});
      const Color fg = i.status == "failed" ? th.err : i.status == "waiting" ? th.attention : i.status == "working" ? th.working : th.dim;
      p.text_clipped(2, y, (on ? "› " : "  ") + i.title, Style{on ? th.accent : th.text, on ? th.panel_alt : th.panel}, p.width() - 4);
      p.text_clipped(4, y + 1, i.info.empty() ? (i.status == "waiting" ? "needs you" : i.status) : i.info,
                     Style{fg, on ? th.panel_alt : th.panel}, p.width() - 6);
      work_hits_.push_back({y, i.id}); y += 2;
    }
    if (items.empty()) {
      p.text_clipped(2, y++, work_tab_ == 2 ? (work_.supports_btw() ? "Ask a side question below." : "This agent has no /btw command.") : "No work here yet.", Style{th.dim, th.panel}, p.width() - 4);
    }
    p.hline(1, y, p.width() - 1, U'─', Style{th.border, th.panel});
    const bool native_btw = work_tab_ == 2 && s_->adapter() && s_->adapter()->native_side_questions();
    const int footer = work_tab_ == 2 && work_.supports_btw() ? native_btw ? 3 : 2 : 1;
    work_body_ = Rect{2, y + 1, std::max(1, p.width() - 4), std::max(1, p.height() - y - footer - 2)};
    const WorkItem* item = work_.find(work_selected_);
    if (item && item->kind == "agent" && !item->path.empty()) {
      work_chat_.open(item->path, s_->adapter());
      work_chat_.set_base_dir(s_->cwd()); work_chat_.poll_growth();
      Painter body = p.sub(work_body_);
      work_chat_.render(body, th, app_->filters());
    } else if (item) {
      const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
      if (now - work_detail_ms_ >= 500) {
        work_detail_ms_ = now;
        work_text_ = work_.detail(*item, chat_.conversation());
      }
      if (work_text_ != work_laid_out_ || work_body_.w != work_width_) {
        work_laid_out_ = work_text_; work_width_ = work_body_.w;
        work_rows_.clear(); work_segs_.clear(); work_arena_.clear();
        const Str text = work_arena_.add(work_text_);
        md::Out out{2000, &work_arena_, &work_segs_, &work_rows_, &work_spans_, &work_inline_, &work_layout_};
        if (item->kind == "terminal") md::render_output(work_text_, text.off, true, work_width_, out, nullptr);
        else md::render(work_text_, text.off, true, work_width_, out);
      }
      const int max_scroll = std::max(0, int(work_rows_.size()) - work_body_.h);
      work_scroll_ = std::clamp(work_scroll_, 0, max_scroll);
      const int first = item->kind == "terminal" ? max_scroll - work_scroll_ : work_scroll_;
      for (int row = 0; row < work_body_.h && first + row < int(work_rows_.size()); row++) {
        const md::Line& line = work_rows_[size_t(first + row)];
        int x = work_body_.x + line.indent;
        for (uint16_t n = 0; n < line.seg_count; n++) {
          const md::Seg& seg = work_segs_[line.seg_first + n];
          if (seg.ink == md::Ink::MathImage) continue;
          x += p.text(x, work_body_.y + row, work_arena_.view(Str{seg.off & ~md::kScratchBit, seg.len}),
                      ink_style(Style{th.text, th.panel}, seg.ink, th));
        }
      }
    }
    p.text_clipped(2, p.height() - 1, "Tab tabs · ↑↓ select · PgUp/Dn scroll · Esc close", Style{th.dim, th.panel}, p.width() - 4);
    if (work_tab_ == 2 && work_.supports_btw()) {
      const std::string prompt = "btw › " + work_prompt_.text();
      p.text_clipped(2, p.height() - 2, prompt, Style{focused ? th.accent : th.text, th.panel}, p.width() - 4);
      if (native_btw) p.text_clipped(2, p.height() - 3, "not saved in the chat", Style{th.dim, th.panel}, p.width() - 4);
    }
  }

  bool work_key(const KeyEvent& k) {
    if (k.key == Key::Escape) { work_open_ = false; work_focus_ = false; return true; }
    if (k.key == Key::Tab || k.key == Key::BackTab) {
      work_tab_ = (work_tab_ + (k.key == Key::Tab ? 1 : 2)) % 3; select_work({}); return true;
    }
    if (work_tab_ == 2 && btw_live_ && k.ctrl && (k.key == Key::Left || k.key == Key::Right)) {
      s_->pty().write(k.key == Key::Left ? "\x1b[1;2D" : "\x1b[1;2C"); return true;
    }
    if (work_tab_ == 2 && btw_live_ && work_prompt_.empty() && !k.ctrl && !k.alt) {
      if (k.key == Key::Up || k.key == Key::Down) {
        s_->pty().write(k.key == Key::Up ? "\x1b[A" : "\x1b[B"); return true;
      }
      if (k.shift && (k.key == Key::Left || k.key == Key::Right)) {
        s_->pty().write(k.key == Key::Left ? "\x1b[1;2D" : "\x1b[1;2C"); return true;
      }
    }
    if (work_tab_ == 2 && work_.supports_btw() &&
        (k.key == Key::Char || k.key == Key::Backspace || k.key == Key::Delete || k.key == Key::Left || k.key == Key::Right)) {
      work_prompt_.edit_key(k); return true;
    }
    if (k.key == Key::Enter && work_tab_ == 2 && !work_prompt_.empty()) {
      const std::string text = text::oneline(work_prompt_.text(), 2000);
      const std::string error = app_->workspace().ask_side_question(*s_, text);
      if (error.empty()) work_prompt_.reset();
      else app_->set_status(error);
      return true;
    }
    if (k.key == Key::Enter) if (const auto* i = work_.find(work_selected_); i && i->key && !i->path.empty()) {
      app_->open_at(i->path, 0, {}); return true;
    }
    const auto items = work_items();
    if ((k.key == Key::Up || k.key == Key::Down) && !items.empty()) {
      int at = 0;
      for (size_t n = 0; n < items.size(); n++) if (items[n]->id == work_selected_) at = int(n);
      at = std::clamp(at + (k.key == Key::Up ? -1 : 1), 0, int(items.size()) - 1);
      select_work(items[size_t(at)]->id); return true;
    }
    if (k.key == Key::PageUp || k.key == Key::PageDown || k.key == Key::Home || k.key == Key::End) {
      const WorkItem* item = work_.find(work_selected_);
      if (item && item->kind == "agent" && !item->path.empty()) work_chat_.on_key(k);
      else {
        const int step = std::max(1, work_body_.h - 2);
        const bool terminal = work_tab_ == 1;
        if (k.key == Key::Home) work_scroll_ = terminal ? 100000 : 0;
        else if (k.key == Key::End) work_scroll_ = terminal ? 0 : 100000;
        else work_scroll_ = std::max(0, work_scroll_ + ((k.key == Key::PageUp) == terminal ? step : -step));
      }
      return true;
    }
    if (k.is('s')) if (const auto* i = work_.find(work_selected_); i && i->kind != "btw") {
      chat_.reveal(i->at, {}); work_focus_ = false; return true;
    }
    return true;  // panel navigation must never type into the main task
  }

  bool work_mouse(const MouseEvent& m, Point local) {
    if (m.kind == MouseKind::Press && local.y == 0 && local.x >= work_rect_.w - 13) {
      work_open_ = false; work_focus_ = false; return true;
    }
    if (m.kind == MouseKind::Press && local.y == 2) {
      work_tab_ = std::clamp((local.x - 2) / std::max(1, (work_rect_.w - 3) / 3), 0, 2);
      select_work({}); return true;
    }
    if (m.kind == MouseKind::Press) for (const auto& hit : work_hits_)
      if (local.y == hit.first || local.y == hit.first + 1) { select_work(hit.second); return true; }
    if (work_body_.contains(local)) {
      const WorkItem* i = work_.find(work_selected_);
      if (i && i->kind == "agent" && !i->path.empty()) return work_chat_.on_mouse(m, Point{local.x - work_body_.x, local.y - work_body_.y});
      if (m.kind == MouseKind::WheelUp || m.kind == MouseKind::WheelDown) {
        const bool up = m.kind == MouseKind::WheelUp;
        work_scroll_ = std::max(0, work_scroll_ + (up == (work_tab_ == 1) ? 3 : -3));
      }
    }
    return true;
  }

  void toggle_view() {
    if (!s_->adapter()) return;
    if (s_->answer_sending()) {
      s_->cancel_answer();
      app_->set_status("finish answering in the terminal");
    }
    view_ = effective_view() == View::Raw ? View::Chat : View::Raw;
    view_chosen_ = true;
    if (view_ == View::Chat) chat_.to_bottom();
    else raw_scroll_ = 0;  // raw snaps to live tail; chat keeps its position
  }

  // Picks the emulator rows worth showing as the live tail: the last few
  // non-blank ones, with trailing blanks trimmed so the strip does not float.
  int live_range() const {
    if (s_->vt().generation() != live_gen_) {
      live_gen_ = s_->vt().generation();
      s_->driver().live_rows(s_->vt(), live_, 4);
    }
    return int(live_.size());
  }

  // One line saying what the agent is actually doing, with a turning spinner:
  // the pending tool's name and argument, or "Thinking…" between calls. It is
  // the cue minimal density leans on, since tool rows are hidden there.
  void render_activity(Painter p, const Theme& th, bool working) {
    // Idle, the row is held so the chat does not jump when work starts, but it
    // is drawn as part of the chat: a tinted empty band reads as a dead strip.
    if (!working) { p.clear(Style{th.text, th.panel}); return; }
    p.clear(Style{th.text, th.strip_bg});
    std::string action = "Thinking\xE2\x80\xA6";
    std::string_view name, summary;
    uint64_t tool_id = 0;
    // Compaction is its own kind of wait, and can run for minutes: say so,
    // rather than "Thinking".
    if (s_->vt().generation() != compact_gen_) {
      compact_gen_ = s_->vt().generation();
      compacting_ = s_->driver().compacting(s_->vt());
    }
    const bool compacting = compacting_;
    if (compacting) {
      action = "Compacting\xE2\x80\xA6";
    } else if (chat_.in_flight_tool(&name, &summary, &tool_id)) {
      action.assign(name);
      if (!summary.empty()) {
        action += ' ';
        action += text::oneline(summary, 120);
      }
    } else if (live_range() > 0) {
      // Keep the latest terminal hint in the reserved row instead of adding
      // transient rows below it. F2 still exposes the full terminal output.
      std::string hint;
      for (const Cell& c : s_->vt().row(live_.back()))
        if (c.width) text::encode(c.cp ? c.cp : U' ', hint);
      hint = text::oneline(hint, 120);
      if (!hint.empty()) action += " · " + hint;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!activity_since_ || activity_tool_ != tool_id ||
        activity_generation_ != s_->work_generation() || activity_pid_ != s_->pty().pid()) {
      activity_since_ = now;
      activity_tool_ = tool_id;
      activity_generation_ = s_->work_generation();
      activity_pid_ = s_->pty().pid();
    }
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now - *activity_since_).count();
    char timer[48];
    if (seconds >= 3600)
      snprintf(timer, sizeof timer, "%lldh %02lldm %02llds", (long long)(seconds / 3600),
               (long long)(seconds / 60 % 60), (long long)(seconds % 60));
    else if (seconds >= 60)
      snprintf(timer, sizeof timer, "%lldm %02llds", (long long)(seconds / 60), (long long)(seconds % 60));
    else snprintf(timer, sizeof timer, "%llds", (long long)seconds);
    const int timer_w = text::str_width(timer);
    const int timer_x = p.width() - timer_w - 1;
    const bool show_timer = timer_x >= 4;
    int action_end = show_timer ? timer_x - 1 : p.width();
    const int sw = p.text(0, 0, spinner_utf8(app_->anim()),
                          Style{th.working, th.strip_bg, attr::kBold});
    const int tx = sw + 1;

    // A running command's progress, read from the bottom-most line of the
    // agent's screen that reports some: a bar, how far, and the time left —
    // as the command says, else estimated from how fast it has been going.
    progress::Progress prog;
    bool has_prog = false;
    if (tool_id && render_settings().progress() == Progress::Bar) {
      const Vt& vt = s_->vt();
      std::string line;
      for (int y = vt.total_rows() - 1; y >= std::max(0, vt.total_rows() - vt.height()) && !has_prog; y--) {
        line.clear();
        for (const Cell& c : vt.row(y))
          if (c.width) text::encode(c.cp ? c.cp : U' ', line);
        has_prog = progress::parse(line, prog);
      }
      if (has_prog && progress_tool_ != tool_id) {
        progress_tool_ = tool_id;
        progress_t0_ = now;
        progress_f0_ = prog.fraction;
      }
    }
    if (has_prog) {
      int eta = prog.eta_s;
      const double ran = std::chrono::duration<double>(now - progress_t0_).count();
      if (eta < 0 && ran >= 3 && prog.fraction > progress_f0_ + 0.01 && prog.fraction < 1)
        eta = int((1 - prog.fraction) * ran / (prog.fraction - progress_f0_));
      char how[48];
      if (prog.total > 0) snprintf(how, sizeof how, "%lld/%lld", (long long)prog.done, (long long)prog.total);
      else snprintf(how, sizeof how, "%d%%", int(std::lround(prog.fraction * 100)));
      std::string label = how;
      if (eta > 0) label += " \xC2\xB7 " + progress::duration(eta) + " left";
      const int room = action_end - tx;
      const int bar_w = std::clamp(room / 5, 6, 20);
      const int lw = text::str_width(label);
      const bool with_bar = room - (bar_w + 2 + lw) >= 12;
      const int need = (with_bar ? bar_w + 1 : 0) + lw + 1;
      if (room - need >= 8) {
        int x = action_end - need + 1;
        if (with_bar) {
          p.text(x, 0, progress::bar(prog.fraction, bar_w), Style{th.working, th.panel});
          x += bar_w + 1;
        }
        p.text(x, 0, label, Style{th.working, th.strip_bg});
        action_end -= need + 1;
      }
    }
    const int aw = p.text_clipped(tx, 0, action, Style{th.working, th.strip_bg},
                                  std::max(0, action_end - tx));
    const int x = tx + aw + 1;
    if (x < action_end) p.hline(x, 0, action_end - x, U'\u2500', Style{th.border, th.strip_bg});
    if (show_timer) p.text(timer_x, 0, timer, Style{th.dim, th.strip_bg});
  }

  void render_raw(Painter& p, const Theme& th) {
    p.clear(Style{th.text, th.panel});
    const Vt& vt = s_->vt();
    int total = vt.total_rows();
    raw_scroll_ = std::clamp(raw_scroll_, 0, std::max(0, total - p.height()));
    int first = std::max(0, total - p.height() - raw_scroll_);

    for (int y = 0; y < p.height(); y++) {
      const VtRow& r = vt.row(first + y);
      for (int x = 0; x < p.width() && size_t(x) < r.size(); x++) {
        const Cell& c = r[size_t(x)];
        if (c.width == 0) continue;
        Style st = c.st;
        if (st.bg == kDefaultColor) st.bg = th.panel;
        if (st.fg == kDefaultColor) st.fg = th.text;
        p.put(x, y, c.cp, st, c.width);
      }
    }
  }

  int prompt_rows(int width) const {
    const int cols = std::max(1, width - 2);
    return std::min(int(prompt_.wrap(cols).size()), kMaxPromptRows);
  }

  // A multi-line, cursor- and selection-aware box. A long line soft-wraps
  // instead of scrolling away sideways, the box grows to fit up to
  // kMaxPromptRows, and it scrolls vertically to keep the cursor's row in
  // view. Highlight the character at the cursor without replacing it; use a
  // thin bar at the end of a line, where there is no character to highlight.
  void render_prompt(Painter outer, const Theme& th, bool focused, bool framed) {
    // Unfocused, it still reads as part of the footer strip, not the chat;
    // focused, sel_bg is the stronger cue that this is where typing goes.
    Style base{th.text, focused ? th.sel_bg : th.strip_bg};
    outer.clear(base);
    if (framed) {
      outer.box(Rect{1, 0, outer.width() - 2, outer.height()},
            Style{focused ? th.accent : th.border, base.bg});
      outer.text(3, 0, note_q_ >= 0 || note_perm_ ? " Note " : reply_live_ ? " Answer " : " Message ",
                 Style{focused ? th.accent : th.dim, base.bg});
      // While the agent works, Enter steers it; Alt+Enter waits for the turn.
      const std::string hint = note_q_ >= 0 || note_perm_ ? " Enter save note · Esc cancel "
                               : reply_live_ ? " Enter send answer · Esc cancel "
                               : s_->busy() ? " Enter steer · Alt+Enter queue "
                               : !s_->queued().empty() && prompt_.empty() ? " \xE2\x86\x91 edit queued · Enter send "
                                                                          : " Enter send · Ctrl+J newline ";
      if (outer.width() > text::str_width(hint) + 16)
        outer.text(outer.width() - text::str_width(hint) - 3, 0, hint, Style{th.dim, base.bg});
    }
    Painter p = framed ? outer.sub(Rect{2, 1, outer.width() - 4, outer.height() - 2}) : outer;

    const std::string& body_text = prompt_.text();
    const size_t cursor = prompt_.cursor();
    const int cols = std::max(1, p.width() - 2);
    const auto rows = prompt_.wrap(cols);  // display rows, tiling the bytes
    if (body_text.empty())
      p.text_clipped(3, 0, note_q_ >= 0 || note_perm_ ? "Note for \"" + text::oneline(note_header_, 60) + "\"… (empty clears it)"
                           : reply_live_ ? "Answer to \"" + text::oneline(reply_.question, 60) + "\"…"
                           : s_->exited() ? "Open this chat to resume" : "Message " + agent_label(s_->agent()) + "…",
                     Style{th.dim, base.bg}, std::max(0, p.width() - 4));

    size_t cursor_row = 0;
    for (size_t i = 0; i < rows.size(); i++)
      if (cursor <= rows[i].second) { cursor_row = i; break; }

    const int box_h = p.height();
    int first = int(cursor_row) >= box_h ? int(cursor_row) - box_h + 1 : 0;
    first = std::min(first, std::max(0, int(rows.size()) - box_h));

    // Draws a piece of a row, with each attachment shown as its label. The
    // label keeps the piece's selection or cursor styling, and is otherwise
    // set off in the accent colour so it reads as one object, not typed text.
    const auto draw = [&](int x, int y, std::string_view piece, Style st) {
      int w = 0;
      size_t run = 0;
      for (size_t i = 0; i < piece.size();) {
        const size_t at = i;
        const PromptEditor::Attachment* a = prompt_.attachment(text::decode(piece, i));
        if (!a) continue;
        w += p.text(x + w, y, piece.substr(run, at - run), st);
        Style ls = st;
        if (!(st.a & attr::kReverse) && st.bg != th.accent) ls.fg = th.accent;
        w += p.text(x + w, y, a->label, ls);
        run = i;
      }
      return w + p.text(x + w, y, piece.substr(run), st);
    };

    const bool has_sel = prompt_.has_selection();
    const size_t sel_lo = has_sel ? prompt_.sel_lo() : 0;
    const size_t sel_hi = has_sel ? prompt_.sel_hi() : 0;

    for (int row = 0; row < box_h; row++) {
      const size_t ri = size_t(first + row);
      if (ri >= rows.size()) break;
      const size_t rs = rows[ri].first, re = rows[ri].second;
      const std::string_view shown(body_text.data() + rs, re - rs);

      // The marker leads only the very first row; continuations indent to
      // match, so a wrapped message still reads as one turn.
      const int x = (row == 0 && first == 0)
                        ? p.text(0, row, "\xE2\x80\xBA ", Style{th.accent, base.bg, attr::kBold})
                        : p.text(0, row, "  ", base);

      // Selection covering this row, in row-relative byte offsets.
      size_t seg_a = shown.size(), seg_b = shown.size();
      if (has_sel && sel_lo < re && sel_hi > rs) {
        seg_a = sel_lo > rs ? std::min(sel_lo - rs, shown.size()) : 0;
        const size_t b = sel_hi < re ? sel_hi - rs : shown.size();
        seg_b = std::clamp(b, seg_a, shown.size());
      }

      if (seg_a < seg_b) {
        int cx = x;
        cx += draw(cx, row, shown.substr(0, seg_a), base);
        cx += draw(cx, row, shown.substr(seg_a, seg_b - seg_a),
                     Style{base.fg, base.bg, attr::kReverse});
        draw(cx, row, shown.substr(seg_b), base);
        continue;
      }

      if (focused && !has_sel && ri == cursor_row) {
        const size_t off = std::min(cursor - rs, shown.size());
        int cx = x + draw(x, row, shown.substr(0, off), base);
        if (off < shown.size()) {
          size_t after = off;
          text::decode(shown, after);
          cx += draw(cx, row, shown.substr(off, after - off), Style{base.bg, th.accent});
          draw(cx, row, shown.substr(after), base);
        } else {
          p.put(cx, row, U'\u258F', Style{th.accent, base.bg, attr::kDecor});  // the cursor
        }
      } else {
        draw(x, row, shown, base);
      }
    }
  }

  LiveSession* s_;
  // Preference, not what is shown: effective_view() resolves the fallback.
  View view_ = View::Chat;
  bool view_chosen_ = false;
  ChatRenderer chat_;
  std::optional<std::chrono::steady_clock::time_point> activity_since_;
  std::chrono::steady_clock::time_point draft_until_{};
  uint64_t activity_tool_ = 0;
  uint64_t activity_generation_ = 0;
  pid_t activity_pid_ = -1;
  // Where the running tool's progress was first seen, to estimate the rest.
  uint64_t progress_tool_ = 0;
  std::chrono::steady_clock::time_point progress_t0_{};
  double progress_f0_ = 0;
  std::vector<ChatRenderer::Chip> chips_;
  mutable std::vector<int> live_;
  // What the agent's screen says, read once per change of it (Vt::generation)
  // rather than every frame: the dialogs, the side panel, the reply being
  // written, the live rows, compaction.
  mutable uint64_t live_gen_ = UINT64_MAX, compact_gen_ = UINT64_MAX;
  mutable bool compacting_ = false;
  uint64_t perm_gen_ = UINT64_MAX, btw_gen_ = UINT64_MAX, draft_gen_ = UINT64_MAX;
  bool perm_seen_ = false, btw_seen_ = false;
  std::string draft_screen_;
  bool screen_permission() {
    if (s_->vt().generation() != perm_gen_) {
      perm_gen_ = s_->vt().generation();
      perm_seen_ = s_->driver().permission_prompt(s_->vt(), perm_);
    }
    return perm_seen_;
  }
  bool screen_side_panel() {
    if (s_->vt().generation() != btw_gen_) {
      btw_gen_ = s_->vt().generation();
      btw_seen_ = s_->driver().side_panel(s_->vt(), btw_);
    }
    return btw_seen_;
  }
  std::string_view screen_draft() {
    if (s_->vt().generation() != draft_gen_) {
      draft_gen_ = s_->vt().generation();
      draft_screen_ = s_->driver().screen_reply(s_->vt());
    }
    return draft_screen_;
  }
  int chip_row_ = -1;
  int body_top_ = 0;
  PromptEditor prompt_;
  std::vector<uint64_t> hist_;     // user message offsets, oldest first
  int hist_at_ = -1;               // the one the box shows; -1 not browsing
  std::string hist_shown_;         // what the recall put there, to spot edits
  Completion comp_;                // the "/" and "@" menus over the box
  Rect comp_rect_{};               // where the menu was drawn, in pane cells
  const std::vector<std::string>* files_ = nullptr;  // the "@" word's folder listing
  size_t files_from_ = size_t(-1);                   // which "@" word it was listed for
  // The question whose note the box is holding, or -1 for an ordinary draft.
  int note_q_ = -1;
  std::string note_header_;
  // The box holds an answer to an optional question, in the user's words.
  bool reply_live_ = false;
  AsyncReply reply_;
  bool question_focus_ = false;
  PromptEditor suspended_prompt_;
  bool prompt_suspended_ = false;
  void restore_suspended_prompt() {
    if (!prompt_suspended_) return;
    prompt_ = std::move(suspended_prompt_);
    suspended_prompt_ = PromptEditor{};
    prompt_suspended_ = false;
  }
  FindBar find_;
  // The agent's permission dialog, when one is up; see refresh_permission().
  BtwPanel btw_;
  bool btw_live_ = false;         // claude's /btw panel is on its screen
  PermissionPrompt perm_;
  bool perm_live_ = false;
  std::string perm_key_;          // which dialog the cursor and perm_note_ belong to
  Picker perm_pick_;               // the choices, and the cursor on them
  std::string perm_note_;
  bool note_perm_ = false;        // the box holds the permission note
  int perm_top_ = -1;             // the panel's first row, in pane rows
  int perm_h_ = 0;                // the panel's rows
  bool answering_question_ = false;  // the answer in flight is a question's
  int prompt_w_ = 78;  // wrap width of the box, from the last render
  int raw_scroll_ = 0;
};

}  // namespace

PanePtr make_session_pane(LiveSession* s) { return std::make_unique<SessionPane>(s); }

}  // namespace mico
