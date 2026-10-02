# mico

A visibility and interaction layer over terminal coding agents. Not a harness:
mico never touches a model API, never owns an agent loop, and never writes into
`~/.claude`, `~/.codex`, `~/.pi`, or `~/.omp` (it does make one narrow, opt-in
exception — see **New sessions in a new folder** below). If mico dies, your
agents keep running. Supported agents: **Claude Code**, **Codex**, **pi**
(`@earendil-works/pi-coding-agent`), and **Oh My Pi** (`omp`) — pi and omp
share one adapter, since omp is built on pi's engine and writes the same
session format.

## Status: M3 — daemon, live agents, raw + chat views

Agents run in a background daemon and survive the client exiting, an ssh drop,
or a closed laptop. Each is shown as its real terminal or as a chat rendered
from its transcript, switchable per pane; stored sessions are browsable and
forkable.

```
cmake -S . -B build -G Ninja && cmake --build build
./build/mico            # attach; starts a daemon if none is running
```

| command | |
|---|---|
| `mico` | attach, starting a daemon if needed |
| `mico --attach` | attach only; fail if no daemon is running |
| `mico --daemon` | run the daemon in the foreground |
| `mico kill` | stop the daemon **and every agent it owns** |
| `mico --local` | single process; agents die with it |

Detaching is just closing the client, or pressing `q`. Agents keep running.
Nothing bound to a keystroke stops them: that needs `mico kill` or the
"Stop all agents and quit" menu item.

| key | |
|---|---|
| `alt-1` · `alt-2` · `alt-3` | focus Projects · Chats · the chat and its message box, from anywhere (over a raw pane too) |
| `tab` / `shift-tab` | cycle pane focus |
| `↑ ↓ j k`, `pgup/pgdn`, `end` | move / scroll |
| `d` | cycle density · `a` show/hide archived chats |
| `space` | select a chat · `⇧↑`/`⇧↓` extend · `ctrl-a` all · `esc` clear |
| left click | open/resume a chat, expand/collapse a tool call (opens on release), open a link |
| left drag | select text and copy it to the clipboard |
| `↑ ↓ ← →`, `enter`, `space`, `1`-`9` | answer a live question card (click an option or its header too); optional Codex cards take only arrows and `enter` |
| drag a seam | resize the split either side of it |
| drag the scrollbar | seek anywhere in the transcript; click the track to jump |
| right click | context menu |
| `:`, `alt-x` or `F1` | mico's command line — `:help` lists everything; `alt-x` and `F1` work while typing a message, and over a raw pane |
| `q` | detach — agents keep running (not while a raw pane has focus) |
| `F2` | toggle raw ↔ chat for the focused agent |
| `F3` | next pane · `F4` new Claude · `F5` new Codex · `F6` fork |
| `F7` | new agent — pick `claude`, `codex`, `pi`, `omp` or type any command, then pick the folder |
| `ctrl-k` · `F12` | go to any chat, in any folder, by name (`F12` works over a raw pane too) |
| `ctrl-g` | outline of the chat on screen: go to any message, edit, failure or question in it |
| `alt-↑` / `alt-↓` | step to the previous / next message you wrote |
| `F8` | selection mode: hands the mouse back so your terminal can copy |
| `F10` | detach (works even over a raw pane) |

Function keys stay global even over a raw pane, which forwards every other
keystroke to the agent. Without them there is no way back out.

Testing hooks, so the TUI can be exercised without a tty:

```
mico --dump [w h] [--project N --session N --density 0|1|2 --scroll N]
mico --vt FILE [--dump w h]     replay captured pty bytes through the emulator
mico --spawn 'CMD'              run any command as a pane (no adapter)
mico --spawn-agent claude|codex|pi|omp  start an agent at launch
mico --bench                    time scan, open, frame and scroll paths
mico --selftest                 JSON reader, wrapper, markdown, both adapters
mico --math 'TEX' out.png [--inline] [--cell W H]   draw one equation to a PNG
mico --chart 'JSON' out.png [--cols N] [--cell W H]   draw one chart to a PNG
```

The self-test also covers session identity, resume, draft isolation, tool
completion, transcript tailing, and terminal input backpressure using temporary
fixtures. `python3 tests/daemon_input.py` checks fragmented paste delivery and
Escape against an isolated daemon; neither test starts a real coding agent.
`python3 tests/claude_questions.py` checks question choices, multi-select,
review submission, cancellation, and stalled delivery against a simulated
Claude terminal that delays rendering and rejects batched keystrokes.
`ctest --test-dir build --output-on-failure` runs all of these, each with its
own HOME and XDG dirs; GitHub CI runs it on every push to `main` or `ci/**` and
on every pull request, built with both GCC 13 and Clang 18.
`python3 tests/render_preview.py artifacts/design/preview` renders the actual
terminal cells at several sizes using synthetic chats (requires Pillow).

`--vt` is how the emulator is regression-tested: capture real agent output with
`script`, replay it, diff the grid.

## Projects

mico tracks a **curated list of folders**, not every directory an agent has ever
run in. The list lives in `$XDG_CONFIG_HOME/mico/folders` (one absolute path per
line) and seeds with mico's own working directory on first run.

The Projects pane ends with a `+ add folder` row — select it and press Enter,
or right-click for **Add folder…** / **Remove this folder** (or `:folder`).
Adding opens a folder browser in mico itself, starting at `~/`: typing
narrows the folders listed, `Tab` goes into the one under the cursor, `Enter`
tracks it, and the last row tracks exactly the path typed. It works the same
over ssh. (It used to open `zenity`/`kdialog`, which blocked the daemon — every
agent's screen — for as long as that window was open.) A tracked folder shows
even with no sessions yet (its count reads `—`); sessions whose working
directory is not a tracked folder are simply not shown.

A tracked folder can go missing — a disconnected network drive, a deleted
checkout. Its chats still list from the transcripts, and resuming one runs in a
folder that exists (the selected folder, another tracked one, or `$HOME`) with
the status saying `(original folder unavailable)`, rather than leaving a dead
pane that never started.

Each folder carries the same mark as its chats (below) when it has a live
agent. With several, it shows the one that wants you most: `!` needs you, then
`●` a new reply, then the working spinner, then `○` ready. Nothing running means
no mark, so the sidebar still reads as a list of folders when the daemon is idle.

### Sub-projects

A folder can be split into sub-projects: right-click it → **Add
sub-project…**, pick the folder the sub-project runs in, then name it. The
Projects pane shows them as a tree, each under its folder (`└ web`), with where
it runs (`./web`, or `same folder`) and how many chats it has. Selecting one
narrows the chat list to its chats, a new chat started there runs in its
folder, and on Usage, Search, Tools and Diff it is one more level of the filter:
All folders → a folder → a sub-project → a chat.

Which chats a sub-project holds depends on its folder:

- **A folder of its own** (anywhere under the project): every chat run in
  that folder or below it. Such chats are listed under the project even though
  their folder is not the project's own — a tracked folder used to show only
  chats run exactly there.
- **The project's own folder**: the folder cannot tell its chats from the
  project's, so it holds the chats started from it, and the ones you put in it
  with right-click on a chat → **Move to sub-project…**.

A move overrides the folder either way, including "No sub-project". Right-click
a sub-project to rename or remove it; removing one sends its chats back to the
folder. It is all mico's own bookkeeping, in `$XDG_CONFIG_HOME/mico/subprojects`
— the agents' files are never touched.

The selected folder is remembered by its path, not its place in the list: the
list re-sorts as chats move, and a remembered position used to land on
whichever folder moved into it.

## Client and daemon

The daemon owns everything stateful — ptys, terminal emulators, transcript
tailing, layout — and it also does the rendering. The client owns its terminal
and nothing else: it forwards the bytes its terminal produced and writes the
bytes the daemon sends back. Neither side models the other's state, which is
why the protocol is six messages.

```
client                          daemon
  raw stdin bytes  ──Input──▶     InputDecoder ─▶ App
  cell-diff ANSI   ◀──Frame──     encode_frame(back, per-client front)
  size on attach   ──Hello──▶     surface sized to the smallest client
```

Each attached client keeps its own front buffer inside the daemon, so a client
joining late gets a full repaint without disturbing anyone already attached, and
several terminals can watch the same session at once. An idle daemon with no
clients renders nothing at all.

The client cannot decode input and does not try: sending raw bytes keeps one
implementation of escape-sequence parsing, in one place, whether mico is running
locally or across an ssh connection.

## Web view

`:web on` has the daemon serve a web page on `127.0.0.1:7311` (`:web on PORT`
for another port; `:web off` stops it). `:web` puts the page's address on the
clipboard. The address carries a token, `#token=…`, which the page needs to
connect; add `&chat=<transcript path>` to open a chat directly.

The page lists the tracked folders and the running agents, shows a chat as it
grows, loads older history on request, starts agents, and sends a running one a
message. Questions and permission dialogs are still answered in mico itself.

It is a client of mico's state protocol (`src/api/client.h`): JSON over a
WebSocket, carrying chats and agents rather than terminal frames, so any program
can be another client. It is for this machine only:

- the daemon listens on 127.0.0.1, never on a network address;
- the WebSocket needs the token, kept in `$XDG_CONFIG_HOME/mico/web-token`,
  readable by you alone, and it never travels in a request: it rides in the
  page address's fragment;
- a request naming any other host is refused, so a web page that points its own
  name at 127.0.0.1 gets nowhere, and so is a WebSocket opened from any other
  origin;
- a client can open only chats mico lists, and start only agents mico has an
  adapter for, in folders you track.

Reaching it from another machine means TLS and a login in front of the same
protocol; until then, forward the port over ssh.

## Three data planes

An observer of agents it does not control has exactly three sources:

1. **PTY** — universal, works with any agent, but yields only a cell grid. You
   cannot semantically filter a rendered TUI (60% of Claude Code's output stream
   is escape sequences, and word gaps are `CSI n G` cursor jumps, not spaces).
2. **Transcript JSONL** — structured: roles, thinking, tool calls, results.
   Per-agent adapter, format drifts.
3. **Hooks** — `SessionStart`/`Stop` push events for working/blocked/idle.

M2 runs planes 1 and 2 together. Both are always live for a known agent; the
toggle only chooses which one is painted, so nothing is missed while you are
looking at the other. Plane 3 is not wired yet.

**Correlating a pane with its transcript** is the whole problem behind the
toggle. Claude and pi are exact: mico generates a uuid, spawns with
`--session-id <uuid>` (pi's file carries a timestamp prefix mico doesn't know
in advance, so it matches on the `_<uuid>.jsonl` suffix instead of the whole
name). Codex and omp offer no such flag. On Linux, new Codex runs match a rollout
opened by the agent's own process session, with matching `cwd` and fork origin
where applicable; this prevents simultaneous runs from swapping transcripts.
Codex and omp resumes match the original session id, including existing files.
New omp runs still discover the newest matching session created after spawn. No
adapter, or correlation not yet resolved, degrades to raw-only.

## Performance

`mico --bench` measures the paths that matter against the largest transcript on
this machine (a 205 MB codex rollout), `mico --selftest` checks the hand-written
JSON reader and the wrapper.

|                                   | start | now |
|-----------------------------------|------:|----:|
| `Store::scan` (list 202 sessions)  | 135 ms | **13 ms** |
| open a 205 MB transcript            |  39 ms | **0.3 ms** |
| first frame                        |  44 ms | **0.3 ms** |
| 60 idle frames, scrolled back 5000 rows | 1048 ms | **4.4 ms** |
| jump to scroll 5000                | 630 ms | **53 ms** |
| peak RSS                           | 214 MB | **92 MB** |

The order of the work was set by profiles, not intuition, and the guesses were
wrong more than once.

**Structure.** Layout is cached and invalidated only by what it depends on —
pane width, density, tool expansion — so scrolling extends it rather than
rebuilding it, and recolouring does not touch it at all. Indexing starts at the
tail of the file and walks back in slabs only when a reader scrolls past what it
has.

**Ownership.** `Event` is a 40-byte trivially-copyable POD; it was 152 bytes
holding four `std::string`s, so every event cost up to four allocations. All
text lives in one append-only arena, rows are `{offset, length}` spans into it,
wrapping computes break points instead of copying substrings, and indentation is
a draw-time x-offset instead of a prefix of spaces. A steady frame allocates
nothing.

**Recycled storage.** When agent output scrolls the emulator, the row leaving
the top does not free its cell buffer: it is either carried down to become the
new blank bottom row, or archived to scrollback while the line that just aged
out of scrollback is reused instead. Steady-state scrolling — the common case
during a long build log or file dump — allocates nothing.

**Doing less.** Codex writes `type` before `payload`, so the 60% of a rollout
that is token counts and event mirrors is rejected after three members and its
payload — often 100 KB — is never read. Tool call ids are hashed at parse time
rather than stored as strings.

**Nothing depends on where a read was split.** The daemon, the client and the
emulator all receive whatever the last read produced. A multi-byte character or
an escape sequence that straddles a boundary has to decode the same as one that
does not, and a line the writer has not finished is not a line. These are tested
as properties — the same bytes fed in every chunk size must produce the same
events — because the failures are silent and only appear under load.

**Seeking by byte.** The scrollbar reports true position in the file, so
dragging it seeks by byte offset — which means extending the tail-first index
backwards until it reaches that far, then re-anchoring the window there. Asking
for a position and reading the thumb back agree to under half a percent. A seek
near the start also has to be able to fill the screen *forwards*: the opening
lines of a rollout are preamble that renders nothing, and a window that only
grows backwards has nothing to show.

**Read what the data says, not what it looks like.** Both listings are
two-tier: a prefix that answers almost every file, and a deeper read only for
the ones it did not. The sizes come from measuring the corpus — a claude
`ai-title` lands by 54 KB, and a codex `session_meta` reaches 21 KB because it
carries the whole system preamble, which is why two thirds of rollouts need
more than a token-sized first read.

**No quadratic paths.** Scrolling back laid an event out on the end of the row
deque and then `std::rotate`d it to the front, which touches every row already
there — once per event, so the cost grew with the square of the window. It is
moved instead. That alone was 40% of a long scroll back.

**Bounded memory.** Reading is what costs memory in a 200 MB transcript, not
keeping: most of a rollout is rejected without contributing a byte of retained
text, so reclamation is driven by bytes *scanned* rather than bytes kept. The
mapping is released back to the kernel every 24 MB scanned, and the parse window
re-anchors on the viewport once retained text passes its budget. Scrolling all
the way back through the largest transcript on this machine holds ~67 MB, flat,
instead of accumulating the file.

**Primitives.** The JSON reader jumps quote to quote with `memchr` instead of
walking bytes, unescaping copies whole runs between backslashes, glyph width has
an ASCII fast path, filling a pane writes cell runs directly instead of going
through the per-cell put path, and the terminal writer formats integers by hand
rather than through `snprintf`. Built with LTO and eager symbol binding: lazy
PLT resolution was ~10% of samples.

**Idle costs nothing.** The daemon renders only when something actually
changed — new agent output, input, a status transition, or the one-per-second
tick that keeps relative timestamps honest. An attached client watching an idle
agent measures 0.0% CPU on both processes, and an agent emitting 20 lines a
second costs about 1.2 KB/s on the wire.

**A short chat reads from the top.** A conversation shorter than the pane
starts at the first line and grows down; only once it fills the pane does it
scroll from the bottom like a terminal.

**Cells are written once.** Every cell used to be cleared up to three times
before any content reached it — the surface clear, the pane's chrome clear and
the pane's own clear. The chrome only paints its border ring now. Text draws
straight into the grid for single-width ASCII, which is nearly all of it, rather
than through a bounds-checked call per glyph.

**Frames are small.** A style change used to emit a full SGR reset plus both
truecolor channels — 34 bytes, and over half of every frame. Only the delta is
sent now, and a short hop between changed cells is walked rather than paid for
with a cursor address. A one-line scroll of the chat repaints the pane in
roughly 3 KB, under a byte per changed cell.

There are no third-party dependencies.

**Session names.** Codex keeps its own thread names in
`~/.codex/session_index.jsonl`, refined as a session goes on. mico reads that
one small file rather than scraping two hundred rollouts, so the chat list shows
"Analyze loss causes" instead of `01a07284-7640-…`. Claude's equivalent is the
`ai-title` record inside its transcript; omp writes its own `title`/
`title_change` records straight into the session. Where none of that exists
(plain pi), the first real user turn is used, and a bare id is the last resort.

## Design constraints that shaped the code

- **Transcripts reach 200 MB.** Nothing is parsed on open, and most of the file
  is never even indexed. See Performance above.
- **Schemas drift.** The same field is a string in one release and an array in
  the next. Every field read goes through `str_field()`/`join_content()`, so an
  unexpected type renders empty instead of aborting.
- **A spawned agent must not inherit its launcher's identity.** Running mico
  from inside a coding agent leaks `CLAUDECODE`, `CLAUDE_CODE_SESSION_ID` and
  friends into the child, which then runs as a nested sub-session and stops
  writing a transcript entirely — silently breaking the chat view. `Pty::spawn`
  scrubs those prefixes.
- **Never resize the pty when switching views.** Both agents render inline, so a
  resize makes them repaint and rewrites captured scrollback. The pty is pinned
  to the pane rect and only follows the pane, never the view.
- **Matching the agent's own rendering is an anti-goal.** If the chat looked
  identical to `claude`, the view would have added nothing. Density and the
  markdown renderer are the product.

## Source layout

Each directory under `src/` is one library, and each may use only the ones
above it in this list; `tools/check_layers.py` (the `layers` test) fails on an
`#include` that reaches the other way.

| | |
|---|---|
| `base/` | text, JSON, paths, logging: nothing mico-specific |
| `vt/` | the cell grid, the terminal emulator, key encoding |
| `math/` | equation and image layout and rasterizing |
| `model/` | mico's plain types: `Event`, `SessionRef`, `Launch`, changes, usage, commands |
| `adapters/` | the `Adapter` interface, and one directory per agent |
| `core/` | sessions, the store, the indexes: everything but drawing |
| `term/` | the host terminal: output, input, kitty and sixel |
| `views/`, `ui/` | the TUI |
| `net/` | daemon and client |

Everything mico knows about one agent is in its adapter (`adapters/adapter.h`):
how to start, resume and fork it, where its transcripts are and how to read
them, what its screen means, and how its menus take keys. Nothing outside
`adapters/` asks which agent it is dealing with, so supporting another agent is
a new `adapters/<agent>/` directory and one line in `adapters/registry.cpp`.
The existing ones split it the same way: `transcript.cpp` (chat events),
`session.cpp` (launch, discovery, liveness), `screen.cpp`, `list.cpp`,
`changes.cpp`, `tools.cpp`, `usage.cpp`, `commands.cpp`.

## What the agent is running

Under the chat is a strip of what the session reports about itself, read out of
the transcript rather than guessed:

```
 model opus-5 ▾ · effort medium ▾ · mode normal ▾ · permissions auto ▾
```

The fields are whatever that agent actually wrote, so they differ by agent —
claude reports a mode and a permission mode, codex reports an approval policy,
a sandbox, a personality and a summary setting, pi and omp report a model,
provider and thinking level. Nothing is fabricated: a field absent from the
transcript is absent from the strip.

Each chip opens a picker sitting on top of the chip itself, with the cursor
on the value in effect and a check beside it. Typing narrows; a list longer
than eight rows starts with the filter row showing (`hai` + Enter sets
haiku). Right-click keeps the small popup at the pointer. Claude's model
chip lists the account's actual models — read from the same `initialize`
answer as the "/" commands, so it carries claude's own names, descriptions
and the exact `/model` values, with no screen-scraping — and the effort chip
lists the levels those models report (`low` through `max`) rather than a
hardcoded three. Codex lists its effort values inline; both send the agent's
own set command. The **mode** chip walks Claude's
permission ring — `default`, `accept edits`, `plan mode` — sending exactly the
number of Shift+Tab presses needed to reach the entry you pick, so plan mode
is one click away. pi and omp switch both model and thinking level through a
fuzzy-search picker rather than a one-shot command, so their chips open that
picker (`/model`, `/thinking`) instead of listing values mico would have to
guess at. Chips with no known command show their value and can be copied — "copy
value" stays in the picker whatever is typed. A
stored transcript is read-only: there is nothing running to command.

## New sessions in a new folder

Claude Code asks "do you trust this folder?" the first time it runs anywhere.
Since a folder only gets into mico's list because you added it, mico writes the
trust flag into `~/.claude.json` when you add it — the same effect as clicking
"Yes, I trust this folder" — so the dialog never appears. (If `~/.claude.json`
has no project store yet, mico falls back to answering the dialog once.) Any
*later* prompt is yours to answer; use F2 to open the agent's terminal when
its prompt cannot be answered through a question card.

## Logs

mico writes a line-per-event log to `$XDG_STATE_HOME/mico/mico.log`
(`~/.local/state/mico/mico.log`). It records daemon/client attach, every agent
spawn (argv, resolved binary, cwd, pid), transcript linking, and — when an
agent exits — its code and the last screen it drew. `:log` shows the path.
The file resets itself once it passes ~4 MB.

## Commands

A line under the chat takes mico's own commands, opened with `:` — or `F1`,
which works even over a raw pane, where every ordinary key belongs to the agent.
Keeping mico behind `:` is what lets `/model` still mean the agent's slash
command.

```
:help            open the palette — every command, clickable
:go [name]       jump to any chat (Ctrl+K)
:new [command]   start an agent; no argument asks which
:folder          track another folder
:outline         the chat's outline (Ctrl+G)
:claude [dir]    :codex [dir]   :pi [dir]   :omp [dir]
:fork  :resume   act on the selected chat
:usage  :tools  :diff  :sessions   switch tabs
:density m|n|f   :select   :redraw   :rescan   :close
:detach          leave; agents keep running
:quit            stop every agent and quit
```

How the view was left comes back after a restart: the density, the tab, the
folder, sub-project and chat selected, the sidebar's "All" filters, the Diff
tab's span and grouping, and the last hundred command lines. They are kept in
`~/.config/mico/view`, beside the split sizes in `~/.config/mico/layout`.

`:help` opens the same list as a palette, so a command can be run by clicking
it rather than remembering it; typing narrows it (`se` finds `search`,
`select`, `sessions`), and Enter runs the top match. Up and Down on the
command line walk the command history. Feedback
appears in the status bar and takes precedence over the standing counts —
otherwise a message loses a fight for space on a narrow terminal and vanishes.

The Projects and Chats column is on the left on every tab; the tabs switch
the rest of the screen — click one, or `[` / `]` to cycle, and the keys go to
what the tab shows. `Sessions` is the working view; `Usage` adds up what the
agents spent, `Search` searches chats, `Tools` shows where their time went,
`Diff` what they changed in files.

On `Usage`, `Search`, `Tools` and `Diff` the column is the filter. Each list gains a
row at the top — **All folders** above the folders, **All chats** above the
chats — and what is lit is what the tab covers:

| lit | the tab covers |
|---|---|
| All folders | every tracked folder |
| a folder, All chats | that folder |
| a folder, a chat | that one chat |

Clicking a chat there only narrows the tab — nothing is opened or resumed for
a look at its usage; `Enter` on it opens it in `Sessions`. `↑` from the first
row reaches the "All" row and `↓` leaves it. Each tab names its scope in its
heading ("Usage  gamedev · Kin open source readiness"), and `Search` runs its
query again when the selection changes. A search asked with a query — a
chat's find bar's "all chats", or `:search <text>` — starts at All folders.
The accounts' usage limits are the account's, so they show whatever the
filter: Codex's from its transcripts, and Claude's subscription limits (the
5-hour session and the week) from Claude itself. Claude hands its status line
command the limits it reads off the API's replies; mico gives Claude itself as
that command (`mico --claude-status`, in the settings it passes on the command
line, never written to `~/.claude`), keeps the latest in
`~/.local/state/mico/claude-limits`, and runs your own status line, if you set
one, on the same input. So the reading is as fresh as Claude's last request;
an old one says how old, and a window that has started over since is not
shown. Settings → Agents → Usage limits turns it off for agents started after.

Chrome yields on small screens: the tab strip goes below six rows, the
command line below four.

## Usage

The `Usage` tab reads the usage the agents already write into their transcripts
— no hooks, no writes, and nothing guessed. It shows the account's codex rate
limit when a session has reported a still-open window, the grand total, and a
breakdown by project and by model:

```
 Usage  just now
 codex  ████████████░░░░░░░░░░░░  47.5%  resets in 3d 4h
 total  $423.29 · 817M in / 1.8M out · 98% cached · 35 sessions
 project   sessions      cost         in       out   cached
 mico            35   $423.29     817.2M      1.8M      98%
 model              cost         in       out   cached
 claude-opus-5  $184.59     247.0M      772k      98%
 gpt-5.6-sol    $146.88     189.6M      326k      95%
```

The four formats name the same quantities differently, so each is normalized:
claude's `cost-state` gives exact per-model tokens and USD, pi and omp report a
per-message cost that is summed, and codex reports cumulative tokens (its
cached input is split out of its input) plus the account's `rate_limits`.
Claude and pi report money; codex is a subscription and shows a dash. A cache
percentage is a cache-read share of prompt tokens, so it means the same thing
for every agent.

Files are read once and cached by mtime and size; a pass is restarted every few
seconds and only the transcripts the agents actually touched are parsed again,
so the tab follows a running session without costing anything while idle. The
first frame reads with a larger budget; after that each frame reads a slice, so
a slow disk never stalls the UI. `r` forces a rescan.

## Settings

The Settings tab (`:settings`) chooses the theme, how each part of the chat
is drawn, and what agents are given. Every setting lists its ways side by
side, the one in force filled in. A change applies at once (every chat lays
itself out again) and is kept in `~/.config/mico/render`, one `name way` per
line; a file from when each part was `on` or `off` still reads as it meant.

- **Appearance:** the theme — `dark` (mico's own), `light`, `high contrast`,
  `warm`. Also `:theme <name>`. The terminal's own background follows it.
- **Rendering**, the plainest way first:

  | | |
  |---|---|
  | Pictures | off · small (12 rows) · medium (24) · large (48) |
  | Equations | source (the LaTeX as written) · unicode · typeset |
  | Charts | source (the JSON) · text (in characters) · pictures |
  | Diagrams | source · drawn |
  | Code colours | off · blocks (fenced code) · everywhere (also files a tool printed) |
  | JSON results | raw · laid out |
  | Notebooks | raw · cells |
  | Output colours | plain · kept |
  | Links | off · urls · urls and paths |
  | Progress | off · bar |

  Typeset equations and charts as pictures need a terminal that shows
  pictures; elsewhere they are drawn in text.
- **Agents:** the agent hints (the note on what mico draws, added to their
  system prompt) and the plot tool (mico's MCP server), off or on. These apply
  to agents started from then on.

Pictures are their own axis, not part of density: shown, a screenshot or plot
appears at every density, even among the folded steps of a finished turn;
density decides how much of the conversation is shown, the settings how it
is drawn. `↑`/`↓` (or `j`/`k`) choose a setting, `←`/`→` (or `h`/`l`) or a
click choose a way, Space or Enter take the next.

## Diff

The `Diff` tab shows what the agents changed in files, and which chat changed
each: every file edited, created, rewritten or deleted in the span, newest
first, with its lines added and removed and the chat (or chats) that did it.
Below the list is the selected file's history, one change at a time, newest
first, each headed by its agent, chat and time:

```
 Diff  mico · last 7 days          t time range · g by chat · a all folders
 12 files changed +1,204 −311 · by 3 chats (Claude 2, Codex 1)
 ❯   +40    −12  src/core/edits.cpp        Claude · Diff tab               2m ago
     +3     −1   src/ui/app.cpp            2 chats · Claude, Codex        1h ago
 ─ src/core/edits.cpp · 3 changes ─ n/p change · enter open the chat there
  Claude · Diff tab · edited · 2m ago                                  +12 −3
   @@ -106 +106 @@
   106   }
   107 + int add_row() const { … }
```

`g` groups by chat instead: each chat, and every file it touched. `↑`/`↓`
choose a row; `PgUp`/`PgDn` (or the wheel over the diff) scroll it, `←`/`→`
scroll it sideways, `n`/`p` jump between changes. `Enter`, or a click on a
change's heading, opens the chat at the call that made it. `t` cycles today,
7 days, 30 days and all time; the sidebar picks the folder, sub-project or chat.

The diffs are the agents' own records, so nothing is compared or guessed:
Claude's `structuredPatch`, Codex's `FileChange` items (or, in older rollouts,
the `apply_patch` call once its result says it applied), pi's and omp's edit
results and their `write` calls. A change that failed is left out. Only the
counts are kept in memory — the index behind `Tools` gathers them in the same
pass — and a change's lines are read again from its transcript line when it
scrolls into view. The index's first pass runs as soon as the daemon starts,
in 25 ms slices between frames (about a second for a gigabyte of
transcripts), so both tabs open on numbers; later passes re-read only the
transcripts that grew.

What it cannot see is a change made through the shell (`sed -i`, a script
that writes a file): the transcript holds the command, not the diff.

## Continuing a chat

Right-click a chat in the sidebar (or press `F6`) to **fork** it into a new
branch, or **resume** it in place. Right-click a live agent pane to fork
whatever it is doing right now into a separate session.

mico never rewrites a transcript to do this — it hands the session id back to
the agent that wrote it and lets the agent do the forking:

```
claude   --resume <id> --fork-session --session-id <new-uuid>
codex    fork <id>
pi       --fork <id> --session-id <new-uuid>
omp      --resume <id>   (no fork flag — mico resumes in place instead)
```

Claude and pi honour `--session-id` even when forking, so a branch is
correlated exactly from the moment it exists. Codex and omp have no such flag
(and omp has no fork flag at all, so mico resumes it in place and says so in
the status bar); their new session is discovered afterwards by cwd and
creation time — the same path a fresh session of that agent takes.

None of the four CLIs fork from anywhere but the **end** of a conversation.
None exposes a way to branch from a chosen message, so mico does not pretend
to: rewinding would mean writing a truncated transcript into the agent's own
store, which is exactly the line this project does not cross.

## The chat list

The sidebar's second pane is one list of every chat in the selected folder —
running sessions and stored transcripts together, no separate "agents" view.
Each chat is two lines tall, so it is easy to tap from a phone terminal: its
name, then its state, agent and when it last moved:

| glyph | state | |
|---|---|---|
| `!` | needs you | a question, a permission or a dialog is waiting on you |
| `●` | new reply | finished a turn you have not looked at yet (its title is bold) |
| `⠋` | working | the agent is working; the spinner turns |
| `○` | ready | live and idle, and you have seen its last reply |
| `·` | saved / stopped | nothing running; opens with automatic resume |

A turn counts as finished once the agent has stayed idle for a moment, so the
gaps between its steps are not taken for a reply. A chat is read as soon as it
is on screen, whether or not its pane has the focus.

**The order is when you last used a chat**, newest first: started or resumed
it, or sent it a message (a saved chat: when its transcript last changed).
What an agent is doing does not move it — the dot says that — so a chat
stays put while its agent works. While the list is in use, focused, touched in
the last few seconds or with its menu open, nothing moves at all; a chat
started from it goes on top, and the rest catch up once you leave it.

**A single click or Enter opens a chat and resumes it automatically.** A running
chat takes focus without starting another process. An exited chat restarts in
place, keeping its unsent draft and avoiding duplicate rows. Arrow keys only
preview a row, so browsing the list does not launch agents. A saved preview also
has an **Open chat** button and accepts Enter. The `+ New chat` button,
pinned to the bottom of the pane, starts a fresh Claude session. Right-click for
new claude / new codex / new pi / new omp / resume / fork / stop. The main pane
is whatever the list has selected — a live session's own view, or a read-only
browse of a stored transcript.

The Projects pane above it works the same way: each folder shows its name,
then its path, chat count and last activity, with `+ Add folder` pinned below.
On a narrow sidebar the path, then the agent, give way to keep the title.
The chat view adds speaker labels, a clear focus accent, and a framed composer
with send/newline hints. Live conversations keep their saved or custom titles.

**Selecting several.** `space` gathers the cursor row and steps down, `⇧↑`/`⇧↓`
extend a run, `ctrl-a` takes every row, `esc` lets go; gathered rows carry a `✓`
and a tint, and the pane title names the count. With more than one gathered, the
context menu switches to bulk actions (archive / unarchive the set). Ctrl-click
toggles a row, shift-click extends.

**Rename and archive** are mico's own marks, kept beside the folder list in
`$XDG_CONFIG_HOME/mico` — they never rewrite an agent's transcript. A rename
overrides the title mico shows; an archive hides the row. `a` shows archived
chats (marked as such) so they can be brought back, and the cursor row's menu
offers both. A running chat cannot be hidden while its pane is open: archiving
it marks its transcript, which takes effect once the agent exits.


## Copying text

Dragging with the left mouse button over any pane selects the text under it and
copies it to the clipboard the moment you let go. The copy goes through OSC 52,
so it reaches your local clipboard even across ssh, and the range stays
highlighted until the next click or keypress. A plain click is untouched: it
still activates whatever it landed on — a row, a chip, a tool call — and a tool
call now opens on release rather than on press, so a drag that happens to start
on one selects text instead of also toggling it.

Mouse capture means the terminal's own selection is disabled, so `F8` gives it
back when you want it anyway (to copy across mico's scrollback, say): mico
releases the mouse and **stops repainting**, because a redraw mid-drag clears
the selection out from under you. Drag and copy as you normally would, then
`F8` to resume.

For a single message, right-click it and "Copy this message" — that goes through
OSC 52 too. Raw panes offer "Copy visible screen".

## The chat view

Prose is rendered, not printed. Headings, bold, italics, strikethrough,
inline code, fenced code blocks, lists, blockquotes, rules, links and tables
all get their markers stripped and their own ink. Lists nest (by indent, with
`•`, `◦`, `▪` bullets; ordered lists keep their numbers) and wrap under their
item; task items become `☐`/`☑`, a done one struck through. GitHub callouts
(`> [!NOTE]`, `TIP`, `IMPORTANT`, `WARNING`, `CAUTION`) get a coloured bar and
title, and `<details><summary>…</summary>` shows its summary as a `▾` heading
with the contents open beneath it.
Tables become aligned columns with a ruled header, honouring `:---:`
alignment, right-aligning columns of numbers, and, when the pane is too narrow,
wrapping the widest cells onto more lines rather than cutting them:

```
 metric                   │ before │  after
──────────────────────────┼────────┼────────
 scan                     │ 135 ms │  13 ms
 open a 205 MB transcript │  39 ms │ 0.3 ms
```

**Diffs.** Edit-shaped tool calls (`Edit`, `Write`, codex `apply_patch`) and
diff output expand into a diff rather than a wall of arguments: removed and
added lines on red and green tints, coloured as the file's language (from its
name or the diff's header), with removed and added lines paired up so the
characters that actually changed stand out on a stronger tint.

**Diagrams.** A ```` ```mermaid ```` block is drawn with box-drawing
characters, in any terminal, and copies as text: flowcharts (`graph` /
`flowchart`, top-down or left-right; `[ ]`, `( )`, `{ }` shapes; solid,
dotted `-.->` and thick `==>` edges; `|labels|` and `-- label -->`; `&`
groups; `subgraph` blocks as dashed, titled boxes round their members),
state diagrams (`[*]` start and end, `A --> B : event`, composite
`state X { … }` as boxes), sequence
diagrams (participants and aliases, `->>` / `-->>` messages, notes, `loop` /
`alt` / `opt` blocks, self-messages) and pies (as a bar chart). Flowcharts are
laid out in layers: a cycle's closing edge is turned to run back up from a
port of its own, nodes are ordered to cut crossings and placed under their
neighbours so edges run straight, and each node's edges bend in a lane of
their own between layers, so lines that join really do meet. A diagram too
wide for the pane one way is tried the other way; one that fits neither, or a
kind not drawn (gantt, class, ER…), shows as code.

**Images.** In a terminal that shows images, pictures are drawn in the chat:
a screenshot a browser tool returns, an image file the agent read, one you
pasted, and a markdown `![alt](plot.png)` on a line of its own (relative to the
agent's folder, and watched, so a plot the agent saves again is redrawn).
Transcripts keep their images as base64; mico notes where each is and reads
it from the line only when it draws it. PNG, JPEG, GIF and BMP are decoded by
stb_image in a throwaway child process, so a malformed image can only kill the
child, never the daemon the agents run under; they are scaled down (never up)
to fit the pane and 24 rows. Elsewhere an image is a line saying there is one.
In kitty a PNG is not decoded by mico at all: the terminal is sent it as it is
and fits it to its cells. Other formats are decoded and compressed on worker
threads, never in the frame: a picture shows as soon as it is ready, and those
a screen above and below the view are made ready before they scroll in.

**Tool output.** Command output keeps its ANSI colours (mapped onto a
sixteen-colour palette tuned to the theme, bold and dim included), and a
progress bar's carriage-return redraws settle to their last state. A file's
contents — a `Read` result, or `cat`/`head`/`sed -n` of one file — are
coloured as that file's language, with `cat -n`/Read line numbers set apart.

**JSON results.** A tool result that is one JSON object or array (an MCP
tool's answer, an API response) is laid out and coloured: collapsed, it is a
one-line outline of the top level, each member's contents counted
(`"labels": […2 items]`, long strings shortened); expanded, the whole of it,
indented, with short arrays of plain values kept on one line. Every object
and array opens with a `▾`: a click on that line closes it to `▸ {…4 keys}`
and a click opens it again, without collapsing the result. A big document
(over 80 lines) opens with what lies below its top two levels closed.

**Notebooks.** A Jupyter notebook the agent reads is shown as a notebook:
markdown cells as prose, code cells as code in the kernel's language, each
cell's outputs under it in an `out` block (tracebacks without their colour
codes), and its plots as pictures in place. That works from Claude's reading of
one (its `<cell id=…>` blocks and the images between them, read again from
the transcript line so each plot lands under its own cell) and from the
notebook's own JSON (`cat analysis.ipynb`). DataFrames become real tables:
from their HTML when the notebook has it, else from pandas' text, read back
into columns (a Series, a wrapped frame or a plain `print` stays text).

**Progress.** While a command runs, the activity row reads its progress off
the agent's screen — tqdm, ninja's `[123/456]`, make's `[ 37%]`, cargo's and
pip's bars, anything with a bar and a count or percentage, never a bare
percentage — and draws it as a bar with how far it is and the time left: the
command's own estimate when it gives one, otherwise worked out from how fast
it has been going since mico first saw it.

**Links.** URLs are links: a markdown `[label](url)` shows its label and
opens its target, and bare `https://…`, `www.…` and `<https://…>` are found
in prose, inline code, code blocks and tool output (trailing punctuation left
out, a Wikipedia URL's brackets kept). A click on one opens it — on the
machine the terminal is on: the daemon hands the URL to the client the click
came from, which runs `xdg-open` (or `open`), detached, with no shell. Only
http, https, file and mailto links open. File paths are links too, when the
file exists (relative to the agent's folder): `src/app.py`, `app.py:42`,
`app.py:42:7`, `app.py#L42`. A click opens `$VISUAL`/`$EDITOR` at that line in
a new pane, on the daemon's machine, where the file is. The cells are also written as OSC 8
hyperlinks, so the terminal itself knows them: it underlines a whole wrapped
link on hover, and its own modifier-click works in selection mode (F8) and
over ssh.

**Code.** A fenced block sits on its own surface, coloured by a small lexer
per language family — C/C++, Rust, Go, JS/TS, Java/Kotlin/C#/Swift, Python,
shell, Ruby, Lua, SQL, JSON, YAML, TOML/INI, HTML/XML, CSS, Dockerfile,
Makefile/CMake, diff — that finds comments (block comments and multi-line
strings carry across lines), strings, numbers, keywords, types, calls,
decorators and macros. The fence's language is shown at the right of the
first line. Tabs become four-column stops. A long line is wrapped at the
code's own width, at a space or after a comma or bracket when one is near,
and continues under its own indentation behind a `↪`; selecting it copies the
line back whole, without the marks or padding. Inline `code` is a chip of the
same surface.

**LaTeX.** In a terminal that can show images, mico typesets equations itself:
no TeX installation, no subprocess. `$$ … $$`, `\[ … \]` and bare
`\begin{align} … \end{align}` blocks are drawn as pictures, and so is inline
math that has structure — a fraction, radical, matrix, accent, stretched
delimiter, `\sum` with limits, `\mathbb`, or a script Unicode has no
characters for. Simple inline math (`$x^2 + \alpha$`) stays text: it matches
the font around it and copies as characters.

The renderer is a small version of TeX's own: a parser into atoms,
fractions, radicals, scripts, delimiters, arrays and accents; TeX's layout
rules (inter-atom spacing by class, the math axis, limits, delimiters that
grow with what they enclose, assembled from top, extender and bottom pieces
when they get tall); and glyphs pre-rendered from Latin Modern Math into a
600 KB atlas linked into the binary (`tools/gen_math_atlas.py` regenerates
it). Glyphs are resampled with a summed-area table, which is an exact box
filter at any scale, so the same atlas serves every cell size, script size
and stretched delimiter. Script sizes never drop below about 11 px, because a
legible subscript matters more than TeX's exact ratios.

An equation is sized to a whole number of cells and laid out as that many rows
of image cells, so scrolling, clipping at a pane edge, folding and frame
diffing treat it exactly like text. Inline math that is taller than a line
gets rows of its own above and below the line it sits on, with the text kept
on the equation's baseline. Selecting over an equation copies its LaTeX.

How the picture reaches the terminal depends on what the terminal says it can
do when mico starts:

- **kitty (0.28+) and Ghostty:** kitty's graphics protocol with Unicode
  placeholders. Each image is sent once, compressed, and from then on the
  cells themselves name it; the terminal moves it with the text. A terminal
  is left holding at most 192 MB of pictures: past that, the ones it showed
  longest ago are freed, and sent again if they scroll back into view. An older
  kitty, which would draw placeholders as stray glyphs, is detected by its
  version and left on Unicode.
- **Inside tmux:** the same, wrapped for tmux's passthrough, when the terminal
  outside is kitty or Ghostty, `allow-passthrough` is on, and tmux keeps
  truecolour (`terminal-features` includes `RGB`): the placeholders' colour is
  the image's id.
- **Sixel terminals** (foot, WezTerm, Windows Terminal, `xterm -ti vt340`…):
  a sixel picture is painted pixels, not an image the terminal can move, so
  mico redraws it, cropped to what is visible, whenever its cells change, and
  blends it onto the cell background itself.

If any attached terminal can show neither, everyone gets the Unicode
rendering, because the layout is shared (so does a sixel terminal whose cells
are a different pixel size from the others'). `MICO_GRAPHICS=off` turns
images off; `=kitty` or `=sixel` forces one.

Everywhere else, rendering means turning the symbols into the Unicode that
already means the same thing. `$x^2$` and `\(x^2\)` render inline as `x²`;
display blocks render indented. Greek letters, relations, arrows, set
theory, `\frac`, `\sqrt`, and `^`/`_` scripts all convert; a command mico does
not recognise still shows its name rather than vanishing, and a script it
cannot represent in Unicode (most letters, as opposed to digits) falls back to
`^(...)` / `_(...)` instead of silently dropping it:

```
 The quadratic formula is x = (-b ± √(b² - 4ac))⁄2a.

 Euler's identity: e^(iπ) + 1 = 0.

   ∑ᵢ₌₁ⁿ xᵢ = α + β · γ
```

**Charts.** A ```` ```chart ```` block (a small JSON spec: line, scatter,
bar, hist, spark or heatmap; inline data or a CSV/JSONL file) and a call to mico's `plot` MCP tool
draw the same chart. In a terminal that shows images, the plot itself —
grid, axes, anti-aliased lines, dots or bars — is a picture, and everything
written on it is text in the terminal's own font: the title, the legend, tick
labels, axis names, a bar's value. Ticks are placed a whole number of cells
apart, so each label sits on its grid line; a bar takes only as many picture
cells as it is long, and its value follows it as text. `"font": "math"` draws
the whole chart as one picture instead, labels typeset like the equations.
Without images it is drawn in cells (braille by default). A histogram
(`"values"`, optional `"bins"`) bins its data itself; `spark` draws each
series as a one-line sparkline with its last value; a heatmap (`"z"` rows,
`"labels"`, `"ylabels"`) colours its cells on viridis.

Several charts make a figure: `{"title": "…", "subplots": [chart, chart, …]}`
lays them side by side (`"columns": n` wraps them into a grid; a chat too
narrow for them stacks them). Each subplot is a chart of its own and may read
its own file. A chart read from a file is redrawn when the file changes, so it
follows a running job; its new frame replaces the old picture rather than
adding one.

Rows are runs of styled segments drawn from one flat pool, and a segment's
*ink* is separate from its row's *base* colour — so a bold run inside a user
turn still reads as the user, and changing the theme does not invalidate a
single laid-out row. Text with no markup at all takes a zero-copy path that
references the arena directly.

**User turns.** A turn you typed is drawn as a block: a gutter bar down its
left edge and a background tint across the full width of the pane, for its whole
height. Three cues rather than one, because a fast scroll defeats any of them
alone — and the tint runs the full width because a background that stops at the
end of the text reads as ragged highlighting rather than as a surface. Claude also records slash-commands,
their output, and injected reminders as *user* messages; on this machine that
was 8 of 28 apparent user rows. Command output and bare reminders are demoted to
machinery, a slash-command collapses to the one line it was (`/model`), and a
reminder injected into a real turn is stripped out of it.

**Answers and the work before them.** What an agent writes between its tool
calls ("Let me check the tests first") and what it answers at the end look
alike in a transcript, but every agent records which is which: Claude marks
the final message `end_turn`, codex `final_answer`, pi and omp `stop`. mico
reads those turn ends. The text after a turn's last step is its **answer**,
drawn at full brightness with an accent bar down its left edge; the text
before it is **commentary**, drawn dim. Once a turn has ended, its steps and
commentary fold into one line above the answer:

```
▌ why is the build slow

  ▸ 18 steps · 2 edits · 1 failed · 4 comments

▎ It spends most of its time linking: …
```

A click on that line (or right-click → Show these steps) opens it in place;
a search match or a jump into a folded turn opens it too. A turn still
running is never folded, and nothing in it is an answer yet, since only its
end says which text was the answer. A cancelled turn has no answer and keeps
its steps. Questions, charts and notices stay out of the fold: they were said
to you. Full density shows every step.

Steps loaded while folded keep no text, only what the fold line counts; opening
a fold reads them again. Scrolling back past a long autonomous turn then costs
a few bytes per step instead of filling the window with text that is never
drawn.

**Chat is the default view**, even for a brand-new session. A recognised startup
trust dialog before the transcript exists temporarily shows the agent's
terminal; an agent that exits without a transcript also shows its terminal
output. Normal messages and prompt-like output never switch an ongoing chat
to raw. F2 toggles the view and pins that choice. Prompts needing the agent's
terminal show a `Needs you · F2 terminal` hint.

**The prompt box** is a real single-field text editor, not an append-only
line: Left/Right/Home/End move the cursor by codepoint (never splitting a
multi-byte character), and Up/Down move within the box once it holds more
than one line.

**`↑` on an empty box recalls what you sent**, newest first; `↑` again steps
older, `↓` steps back and past the newest empties the box. The history is
read out of the transcript, so it survives a daemon restart, covers turns
typed into the agent's own terminal, and is per chat. Editing a recalled
message turns it into an ordinary draft: the arrows go back to moving in it,
and sending or `Esc` ends the browsing. A queued message still comes back
first (see below), and a chat with nothing said yet keeps `↑` scrolling the
transcript; otherwise scrolling is PgUp/PgDn, the wheel, and `Alt+↑`/`Alt+↓`
between your messages.
Shift with any of those extends a selection; typing or Backspace/Delete over
one replaces or removes it, and a bare arrow afterwards collapses it to
whichever edge it points at rather than also taking its usual step. **Ctrl+J**
inserts a real newline — Enter still sends — which grows the box up to six
lines, scrolling vertically to keep the cursor's line on screen and
soft-wrapping a line longer than the pane onto the next row rather than
scrolling it sideways. This logic lives in
`PromptEditor` (`views/prompt_editor.h`), split out from the pane itself
specifically so the cursor and selection arithmetic can be unit-tested
without a live pty.

The caret highlights the character at the cursor in a contrasting color,
keeping the character visible and preserving its width. At the end of a line
it uses a thin bar. Moving the cursor never shifts or hides the text.

While the box holds text, the classical Ctrl editing keys apply: `Ctrl+A`/`E`
line ends, `Ctrl+B`/`F` and `Ctrl+←`/`→` (or `Alt+B`/`F`) by character and by
word, `Ctrl+W` (and `Ctrl+Backspace` where the terminal distinguishes it)
plus `Alt+D`/`Ctrl+Delete` delete a word, and
`Ctrl+U`/`Ctrl+K` kill to the line start/end. An **empty** box is left alone:
every Ctrl combination still reaches the agent, so an interrupt is never
swallowed while there is nothing to edit — except `Ctrl+K`, which on an empty
box opens the chat switcher.

**What the agent is doing.** One reserved line above the message controls has a turning
braille spinner and the current action: the pending tool's name and argument
("Edit src/foo.cpp", "Bash cargo test"), or "Thinking…" between calls. Claude's
visible working footer determines whether it is active: idle redraws and
completion notices cannot start the spinner, and quiet work does not stop it.
The tool name comes from pending calls in the transcript. The same pending call turns its `▸` bullet into the
spinner and tints its row, which is what makes an executing tool block
unmistakable in the denser views. The spinner only advances while some live
agent is busy (about eight frames a second, a few bytes each), so an idle
daemon still renders nothing. Question forms and answer delivery suppress the
thinking indicator. Codex is working while its "Working (… esc to interrupt)"
line shows above its input box, or while its transcript has a turn open
(`task_started` without `task_complete` or `turn_aborted`), which covers the
moments that line is off screen; typing into it or its startup redraws are
not work. It needs you when a dialog (a command to approve, the folder trust
question) has taken its input box's place, not when its reply happens to ask
something. Other agents currently use recent terminal output to estimate
activity.

The right side of the activity row shows elapsed time for the current thinking
phase or tool call (`8s`, `1m 05s`, `1h 02m 03s`). A new tool call resets it,
including repeated calls with identical commands. Switching views preserves
the timer; idle clears it along with the rest of the row.

**The activity row stays reserved when idle.** The latest useful terminal hint
shares this row instead of changing the transcript's height. The input box,
the status line, everything at or above a completed-message bullet, and any
indented row — a wrapped continuation or nested tool detail the transcript will
capture — are all dropped. When in doubt it shows nothing; the pane title and
the chip bar already say the agent is working.

**The pty is spawned at the size it will be drawn at.** `LiveSession::start`
only prepares the launch; the fork happens on the pane's first render, when the
real geometry is known. Spawning at a provisional 80×24 and resizing a frame
later made the agent paint its banner twice and leave torn cells behind.

**Live terminal output.** Messages not yet written to the transcript remain
available through F2. Chat keeps a stable viewport and uses its single activity
row for hints while waiting for the completed records.

Only the work is spliced in, never the agent's own furniture: an agent parks a
bordered input box and a status line at the bottom of its screen, and pasting
those under the chat stacks a second prompt and a second status line on top of
mico's own. See "The live strip is a two-line hint" above: every furniture row is dropped
individually rather than by cutting at the first border, an inline-rendering
agent can briefly hold two copies of its box mid-repaint, and indented
continuations of a completed message are the subtle leak. pi and omp render a
chat of their own into the terminal, so their views are transcript-only: no
tail is spliced at all, because any row of their screen is their rendering of
a turn mico renders itself — the streaming reply, the thinking, the tool
output. Claude and Codex keep the strip. `Esc` in the chat prompt still
reaches the agent, because interrupting has to keep working when the agent's
own "esc to interrupt" hint is not the thing on screen. This is why both planes are always running even though only one
is painted.

## Model questions

When an agent asks the user a multiple-choice question — Claude's
`AskUserQuestion`, Codex's `request_user_input` and `request_user_input_async`,
omp/pi's `ask` — the chat renders it as a card rather than a bare tool row: a
header chip, the question, and each option with its description. Nothing is
guessed from the screen; the card is parsed from the tool call in the
transcript. A stored transcript shows the card with the recorded answer beneath
it.

Claude (2.1.x) does **not** write a pending `AskUserQuestion` call to its
transcript: the call stays in memory while its dialog is up and is written
together with the answer, so without help the chat shows nothing and the agent
simply looks stopped. Claude does flush the transcript before running a hook,
since hooks are handed `transcript_path`. So every `claude` mico launches gets

```
--settings '{"hooks":{"PreToolUse":[{"matcher":"AskUserQuestion","hooks":[{"type":"command","command":"true"}]}]}}'
```

— a do-nothing hook on that one tool. It lands the call in the transcript
before the dialog opens, and the card appears the moment the question exists.
Flag settings merge with your own and are never written anywhere. A launch
that already passes `--settings` is left alone (Claude reads only one), and
then its questions only appear once answered; answer them with F2.

A **live** card is answerable without leaving chat, however many questions it
has. While the prompt box is empty it owns the navigation keys. `←`/`→` (or
`tab`) move between the questions and the trailing `Submit` step, `↑`/`↓` move
the option cursor on the current question, `Enter` selects the current option
and advances — answer the first, land on the second — and `Space` toggles a
multi-select. Enter advances a multi-select without changing its selections.
`1`-`9` pick an option directly, and a left-click picks an option
or switches question. A single-question single-select card commits on `Enter`;
everything else goes through `Submit`. Submission focuses any unanswered
single-select question instead of silently sending its default option.

mico answers by driving the agent's own menu. For Claude, it toggles the chosen
options, navigates to the separate Next/Submit button for multi-select, and
confirms the review screen when present. Each key waits for the previous
terminal update to settle; delivery continues if you switch chats. Selected
answers remain visible until the tool result arrives. If delivery stalls, F2
opens the terminal and cancels any remaining automatic keys so you can finish
there. A draft is kept until the pending question finishes. The transcript is
never edited by mico.

The transcript is also polled when a raw pane is waiting for input. Question
cards keep the conversation in chat; F2 opens the terminal for other dialogs.

### Side questions (Claude's `/btw`)

`/btw <question>` asks Claude something on the side without interrupting
what it is doing — including while it works. Claude answers in a panel that
takes its input box's place and **writes nothing to the transcript**: the
answer exists only on its screen. mico reads that panel off the screen every
frame, the way it reads the permission dialog, and draws it at the foot of
the chat as an aside — `◇ btw  <question>`, the answer as markdown (its
wrapping undone, bold and lists kept), and a key line — until the panel
closes. In a chat with nothing sent yet it takes the empty page instead.

While the box is empty the panel's own keys go through to it: `Esc` closes
it, `↑`/`↓` scroll a long answer (mico shows what Claude's panel shows, so
this is how the rest of it comes into view), and `Shift+←`/`→` step through
the session's earlier side questions. Typing into the box is still yours —
and sending a message while the panel is open closes it first with an Esc,
since a panel that took the typing would read `c`, `f` and `x` as its own
copy, fork and clear keys. An open panel does not count as needing you: the
answer is already in the chat.

### Optional questions (Codex)

Codex's `request_user_input_async` asks without stopping: the call returns
`{"accepted":true}` at once and the agent keeps working. Its reply is not a
tool result but an ordinary user message quoting the question —
`> question` then a blank line then the answer — whenever the user gets to it.
mico shows it as a card marked **optional**, hides the receipt, and leaves the
agent's status as Working (`Working · question for you` in the pane header).

- The newest open optional card takes `↑`/`↓`/`Enter` and clicks while the
  prompt box is empty — but never typed characters: `1`-`9` and `Space` go to
  the prompt box, so a message starting "1." stays a message. A blocking
  question always takes precedence.
- Picking an option sends that quoted message, byte for byte what Codex's own
  UI writes, so the agent reads it the same way.
- **Answer in your own words** (always offered, and the only choice when the
  question has no options) opens `> question` plus a blank line in the prompt
  box. Nothing is sent until you press Enter.
- A card stays open across turns. It closes when a message quoting it arrives
  (the answer is drawn on the card) or when you send anything else, which
  marks it *not answered — the conversation moved on*.

## Pickers

Every list of choices — right-click menus, the `:help` palette, the chip
menus, Claude's permission panel — is one component, `Picker`
(`ui/picker.h`). A new one is a list of items and what to do with the answer;
the keys, the mouse, filtering, scrolling and drawing come with it, so they
behave the same everywhere.

| | |
|---|---|
| `↑ ↓`, `ctrl-p/n`, `tab`/`shift-tab` | move; one step wraps round, a page does not |
| `pgup pgdn home end` | by page, to either end |
| `enter` · click | pick (single) or confirm the marked set (multi) |
| `space` · `tab` while filtering · click | mark an item in a multi-select; `⇧↑`/`⇧↓` mark as they go, `ctrl-a` marks all or none |
| `1`-`9` | pick (or mark) the nth item, when the picker numbers them |
| typing · paste | narrow the list, where filtering is on |
| `esc` | clear the query, then close |
| wheel · hover | scroll without moving the cursor · move the cursor |

An item has a label, a dim detail (after the label, aligned in a column, or
wrapped under it), a right-aligned hint, a lead glyph, a group heading,
disabled and separator states, and a checked flag. Two frames: **Popup**, a
bordered box with its title in the border and the key hint along the bottom,
and **Panel**, a borderless strip on the footer surface with a title, a
subtitle, a note and a hint row.

Filtering is fuzzy and ranked: the query as a run at a word start ranks
first, then its characters in order where each continues a run or starts a
word (`tc` finds "tool calls"), then a run inside a word ("fetch"). Letters
scattered through the middle of a word do not match at all. A query with
spaces that does not match as a whole is taken a word at a time, each word
matching the label or the detail, in any order. Matched characters are underlined, and a detail counts only when it
contains the query as a run, since a subsequence scattered over a sentence of
help text matches nearly anything. A list refreshed every frame (a dialog
read off the agent's screen) keeps its cursor where it was.

### Going places

**`Ctrl+K`** (or `F12`, or `:go`) opens a switcher over every chat in every
tracked folder: running agents first — the ones that need you at the top,
with the same marks as the sidebar — then every saved chat, newest first. Each
row says its folder and agent, so `mico codex` finds the Codex chats in mico.
`Enter` opens the chat exactly as a click in the sidebar does: a running one
is focused, a saved one resumes. It needs no trip through the Projects pane,
and it works from a raw pane (`F12`), where every other key goes to the agent.

**`F7`** (or `:new`) starts an agent in two steps. First *what*: the four
agents, then commands you ran this way before (kept in
`$XDG_CONFIG_HOME/mico/commands`), and whatever you are typing as a command of
its own; `Tab` puts an entry in the query so arguments can be added. Then
*where*: the tracked folders, the selected one first, or type `~/` or `/` to
browse anywhere, `Tab` going into a folder. `Backspace` on an empty query goes
back to the first step with the command still there.

**`Ctrl+G`** (or `:outline`, or the chat's right-click menu) opens the chat's
outline: every message you wrote (`›`), every file the agent edited (`✎`,
in-place `sed -i` included), every tool call that failed (`✗`, named after
the call), every question it asked (`?`) and every compaction (`◆`), newest
first, the cursor on where the view is now. Type to narrow — `arrow.cpp`
finds the edits to that file — and `Enter` scrolls the chat there, loading
older history as needed. `Alt+↑`/`Alt+↓` still step one message at a time.

The outline is read from the transcript file, not from the part of it laid
out on screen, and kept: opening it again reads only what the agent wrote
since. The first read is bounded — about 0.7 s, which covers a 490 MB Codex
rollout whole and most of a 527 MB Claude transcript — and when it stops short
the last row, *Read older history…*, reads the rest.

The switcher and the new-agent and folder pickers belong to mico rather than to a pane, so an agent starting or
exiting underneath — which rebuilds the layout — does not close them.

### "/" and "@" in the prompt box

Typing `/` at the start of a message opens the agent's command menu above
the box; `@` anywhere opens its files. The box stays the query field — keep
typing to narrow, `↑`/`↓` move, `Tab` completes, `Esc` closes the menu for
that word. `Enter` on a command completes it and runs it, as the agents' own
menus do, unless it needs an argument (`/compact <instructions>` waits for
you). `Enter` on a file puts `@path` in the message; on a folder it puts
`@folder/` and the menu goes on inside it.

- **Claude's commands come from claude.** Its commands, skills, plugins and
  your own `.claude/commands` change with every release and install, so mico
  does not keep a list: it asks. `claude -p` answers the SDK's `initialize`
  request with the list its own menu draws from, without a conversation, a
  transcript, or a model call. mico asks once per folder in a throwaway
  process (again after five minutes, if the menu is used), shows the
  interactive-only commands (`/resume`, `/rewind`, `/theme` …) from a short
  table, and merges the two. Like any claude run, the probe runs your
  SessionStart hooks and leaves an empty `~/.claude/session-env/<id>` folder.
- **Codex, pi and omp** list their built-in commands from tables taken from
  their current releases; pi and omp add prompt templates (`/name`) and skills
  (`/skill:name`) from `~/.pi/agent`, `~/.omp/agent` and the project's
  `.pi`/`.omp`.
- **Files** are what `git ls-files` lists (tracked plus untracked, not
  ignored) in a work tree, and otherwise a breadth-first walk that skips
  hidden and dependency folders, capped at 60,000 entries. A match in the
  file's name ranks above one spread over its folders; a query with a `/` in
  it matches the whole path. The listing is refreshed when an `@` word starts
  and it is more than five seconds old, so a file the agent just wrote can be
  named.

Menus match names only, not descriptions, and a character has to continue
the run before it or start a word: `/co` finds `/compact` and `/remote-control`,
not `/doctor`.

Question cards are not on it yet: they are laid out inside the chat's own
rows, with notes and a Submit step, and remain their own code.

## Layout

```
src/term/    surface.h   Cell / Surface / Painter (clipped, translated)
             term.h      raw mode, key + SGR mouse decoding, diffing renderer
             text.h      UTF-8, display width, wrapping
src/term/    vt.h        terminal emulator: bytes -> cell grid + scrollback
             keys.h      re-encodes decoded input back out to a child pty
src/core/    fs.h        the little filesystem walking both agents need
             json.h      scanning JSON reader: no tree, no allocation
             pty.h       forkpty spawn, non-blocking io, env scrubbing
             session.h   a running agent: pty + emulator + transcript link
             jsonl.h     mmap + newline index, lazy line views
             event.h     40-byte POD Event + the arena that owns all text
             adapter.h   per-agent line -> 0..N Events
             claude.cpp  codex.cpp  pi.cpp  store.cpp (project/session discovery)
             commands.h  what "/" offers per agent; claude's asked of claude
             files.h     what "@" can name in a folder
src/ui/      pane.h      the Pane interface: render / key / mouse / menu
             layout.h    split tree; places panes and hit-tests them
             picker.h    every list of choices: menus, palette, panels
             app.h       theme, density, focus, context-menu overlay, loop
src/views/   project_list  chat_list
             chat_render   the chat renderer, shared by both consumers below
             chat_view     browses stored transcripts
             session_pane  a live agent: raw view, chat view, prompt box
             markdown.cpp  latex.cpp  chips.cpp  prompt_editor.h
             code.cpp      syntax colouring for fenced code
src/math/    tex.h       math parser: atoms, fractions, scripts, arrays…
             layout.h    TeX's box rules, simplified: spacing, axis, limits
             atlas.h     pre-rendered Latin Modern Math glyphs (atlas.bin)
             math.h      equation -> cell-sized image, cached per config
             kitty.h     sends images to a terminal once, frees them
             sixel.h     redraws images as sixel pictures when they move
             deflate.h   zlib streams for image data and --math PNGs
             raster.h    coverage masks: glyphs, rules, strokes, text
             picture.h   images from files and transcripts: decode, fit
src/third_party/ stb_image.h (public domain / MIT), image decoding
             completion    the "/" and "@" menus over the prompt box
```

`Surface`/`Cell` are deliberately the types a terminal emulator needs, so M2's
PTY plane reuses them rather than replacing them.

**Draggable splits.** `Node::place()` emits the seam between adjacent children
as a `Divider` alongside the pane rectangles, so hit-testing and layout stay in
one place. A drag moves the boundary by reweighting only the two neighbouring
fractions, leaving every other child untouched. Dividers are collected
deepest-first, so a nested seam wins a hit test against the outer one it sits
inside. Mouse mode 1002 reports motion only while a button is held — enough to
drag, but there is no hover state, so each seam draws a permanent handle as the
affordance. Resizing a pane holding an agent resizes its pty through the normal
pane-geometry path, with no special case. The fractions are written to
`$XDG_CONFIG_HOME/mico/layout` when a drag ends and re-applied whenever the tree
is rebuilt or mico restarts, so switching chats or folders — or quitting — does
not snap the columns back.

## Next

Done through M3. What is left is hardening rather than new structure: a bounded
parse window, an accurate scrollback scrollbar, the codex correlation race,
and named daemons.

### Known gaps

- Sessions die with the *daemon*: `mico kill`, a reboot, or a daemon crash takes
  the agents with it. Nothing is re-attached on daemon restart.
- Only one daemon per user (`default.sock`); named sessions are not implemented.
- Forking branches from the end of a conversation only; see above.
- Markdown covers what agents actually emit; nested lists are not special-cased
  and fall through as plain text.
- New Codex correlation requires Linux `/proc` access to identify the rollout
  opened by the agent process. New OMP sessions still use cwd/time discovery;
  resumes use the known session id.
