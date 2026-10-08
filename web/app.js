// mico's web view: a client of the state protocol (src/api/client.h).
// Everything from a transcript goes into the page as text, never as markup:
// nodes are built with createElement and textContent, and a link is followed
// only if it is http, https or mailto.
"use strict";

const $ = (id) => document.getElementById(id);
const el = (tag, cls, text) => {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text !== undefined) e.textContent = text;
  return e;
};

// Kept in the browser, for conveniences only: it may be empty or refuse.
const store = {
  get(key, fallback) {
    try {
      const v = localStorage.getItem("mico." + key);
      return v === null ? fallback : JSON.parse(v);
    } catch (_) { return fallback; }
  },
  set(key, value) { try { localStorage.setItem("mico." + key, JSON.stringify(value)); } catch (_) { /* private mode */ } },
};

const params = new URLSearchParams(location.hash.slice(1));
const token = params.get("token") || "";
// #token=…&chat=<transcript path> opens that chat: a chat can be bookmarked.
const startChat = params.get("chat");

const narrow = matchMedia("(max-width: 859px)");
const coarse = matchMedia("(pointer: coarse)");

const st = {
  ws: null,
  adapters: [],
  folders: [],
  agents: [],
  chat: null,        // {path, key, title, start}: a stored chat, or a running agent
  state: {},         // the open chat's chat_state
  waiting: new Set(),  // tool ids of questions still waiting for an answer
  optional: new Set(), // tool ids of questions answered with a message instead
  openKey: null,     // an agent just started here, to open once it is listed
  calls: new Map(),  // tool id -> its element, so a result lands under its call
  rid: 0,
  pending: new Map(),  // rid -> what to do with the result
  backoff: 500,
  filter: "",
  collapsed: new Set(store.get("collapsed", [])),
  showAll: new Set(),   // folders listed in full
  newFor: null,         // the folder whose "start an agent" row is open
  drafts: new Map(),    // chat id -> an unsent message
  atBottom: true,
  unread: 0,
  loadingOlder: false,
  view: store.get("view", "normal"),  // minimal | normal | full
  pics: new Map(),      // what an image asked for waits on, until it arrives
};

const STATUS = { working: "working", waiting: "needs you", idle: "ready", exited: "stopped" };

// ------------------------------------------------------------------ connection

function connect() {
  if (!/^[0-9a-f]{64}$/.test(token)) {
    $("empty-text").textContent = "No token, or not a whole one. Open the address mico gave you: type :web in mico, which copies it.";
    return;
  }
  // Over https (tailscale serve) the socket is wss, from the same host.
  const scheme = location.protocol === "https:" ? "wss" : "ws";
  // The token goes as a subprotocol, a header, so it is in no address a proxy logs.
  const ws = new WebSocket(`${scheme}://${location.host}/ws`, ["mico", `mico-token.${token}`]);
  st.ws = ws;
  ws.onopen = () => {
    st.backoff = 500;
    setConn(true);
    if (st.chat && st.chat.path) send({ type: "open", path: st.chat.path });
    else if (startChat && !st.chat) openChat(startChat, "");
  };
  ws.onmessage = (e) => receive(JSON.parse(e.data));
  ws.onclose = () => {
    setConn(false);
    setTimeout(connect, st.backoff);
    st.backoff = Math.min(st.backoff * 2, 10000);
  };
}

function setConn(on) {
  const c = $("conn");
  c.className = "conn " + (on ? "on" : "off");
  c.title = on ? "Connected" : "Not connected";
  $("banner").hidden = on;
}

function send(msg) {
  if (st.ws && st.ws.readyState === WebSocket.OPEN) st.ws.send(JSON.stringify(msg));
}

function command(msg, then) {
  msg.rid = String(++st.rid);
  if (then) st.pending.set(msg.rid, then);
  send(msg);
}

let toastTimer = 0;
function toast(text, bad) {
  const t = $("toast");
  t.textContent = text;
  t.className = (text ? "show" : "") + (bad ? " bad" : "");
  clearTimeout(toastTimer);
  if (text) toastTimer = setTimeout(() => { t.className = ""; }, bad ? 6000 : 3000);
}

function receive(m) {
  switch (m.type) {
    case "hello": st.adapters = m.adapters || []; renderSide(); break;
    case "folders": st.folders = m.folders; renderSide(); renderHead(); break;
    case "agents": st.agents = m.agents; agentsChanged(); break;
    case "chat": if (st.chat && m.path === st.chat.path) addEvents(m); break;
    case "chat_state": if (st.chat && m.path === st.chat.path) applyState(m); break;
    case "result": {
      const then = st.pending.get(m.rid);
      st.pending.delete(m.rid);
      if (!m.ok) toast(m.error || "that did not work", true);
      else if (then) then(m);
      break;
    }
    case "image": case "file_image": gotPicture(m); break;
    case "error": toast(m.message, true); break;
  }
}

// ----------------------------------------------------------------- navigation

// One screen at a time on a phone: the list, then a chat with a way back that
// is the browser's own back, so a phone's back gesture does the same.
function setScreen(s) { $("app").dataset.screen = s; }

function showChatScreen() {
  if (narrow.matches && $("app").dataset.screen !== "chat") history.pushState({ screen: "chat" }, "");
  setScreen("chat");
}

window.addEventListener("popstate", (e) => setScreen(e.state && e.state.screen === "chat" ? "chat" : "list"));

$("back").onclick = () => {
  if (history.state && history.state.screen === "chat") history.back();
  else setScreen("list");
};

// The address keeps the token and names the chat, so a reload comes back to it.
function keepAddress() {
  const p = new URLSearchParams();
  p.set("token", token);
  if (st.chat && st.chat.path) p.set("chat", st.chat.path);
  try { history.replaceState(history.state, "", "#" + p.toString()); } catch (_) { /* sandboxed */ }
}

if (startChat && narrow.matches) {
  history.replaceState({ screen: "chat" }, "");
  setScreen("chat");
}

// ------------------------------------------------------------------- sidebar

function ago(seconds) {
  const d = Date.now() / 1000 - seconds;
  if (d < 60) return "now";
  if (d < 3600) return `${Math.floor(d / 60)}m`;
  if (d < 86400) return `${Math.floor(d / 3600)}h`;
  return `${Math.floor(d / 86400)}d`;
}

const base = (p) => (p || "").replace(/\/+$/, "").split("/").pop() || p || "";

function findChat(path) {
  if (!path) return null;
  for (const f of st.folders)
    for (const c of f.chats) if (c.path === path) return { chat: c, folder: f };
  return null;
}

function agentFor(chat) {
  return st.agents.find((a) => a.status !== "exited" && a.transcript && a.transcript === chat.path);
}

function agentsChanged() {
  if (st.openKey) {
    const a = st.agents.find((x) => x.key === st.openKey);
    if (a) { st.openKey = null; openAgent(a); }
  }
  // A new agent writes its transcript only with its first message: follow it
  // there once it does.
  if (st.chat && st.chat.key && !st.chat.path) {
    const a = st.agents.find((x) => x.key === st.chat.key);
    if (a && a.transcript) {
      st.chat.path = a.transcript;
      $("events").replaceChildren();
      st.calls.clear();
      send({ type: "open", path: a.transcript });
      keepAddress();
    }
  }
  renderSide();
  renderHead();
}

let sideQueued = false;
function renderSide() {
  if (sideQueued) return;
  sideQueued = true;
  requestAnimationFrame(() => {
    sideQueued = false;
    renderAgents();
    renderFolders();
    renderCount();
  });
}

function renderCount() {
  const live = st.agents.filter((a) => a.status !== "exited");
  const need = live.filter((a) => a.status === "waiting").length;
  const box = $("count");
  box.replaceChildren();
  if (live.length) box.append(document.createTextNode(`${live.length} running`));
  if (need) {
    box.append(document.createTextNode(" · "));
    box.append(el("span", "need", `${need} need${need === 1 ? "s" : ""} you`));
  }
  setTitle();
}

function setTitle() {
  const need = st.agents.filter((a) => a.status === "waiting").length;
  const prefix = need ? `(${need}) ` : "";
  const a = liveAgent();
  const t = st.chat ? ((a && a.title) || st.chat.title) : "";
  document.title = `${prefix}${t ? t + " · " : ""}mico`;
}

function matches(text) {
  return !st.filter || text.toLowerCase().includes(st.filter);
}

function renderAgents() {
  const box = $("agents");
  box.replaceChildren();
  const list = st.agents.filter((a) => matches(`${a.title || ""} ${a.agent} ${a.cwd}`));
  $("running-sec").hidden = !list.length && !!st.filter;
  if (!list.length) box.append(el("div", "dim-note", "Nothing running."));
  // The ones that want you first.
  const rank = { waiting: 0, working: 1, idle: 2, exited: 3 };
  list.sort((x, y) => (rank[x.status] ?? 4) - (rank[y.status] ?? 4));
  for (const a of list) {
    const b = el("button", `row agent ${a.status}`);
    b.type = "button";
    b.append(el("span", `dot lead ${a.status}`));
    b.append(el("span", "t", a.title || `New ${a.agent} chat`));
    b.append(el("span", "w", STATUS[a.status] || a.status));
    b.append(el("span", "m", `${a.agent} · ${base(a.cwd)}`));
    b.title = a.cwd;
    if (isCurrent(a.transcript, a.key)) b.classList.add("current");
    b.onclick = () => openAgent(a);
    box.append(b);
  }
}

function isCurrent(path, key) {
  if (!st.chat) return false;
  return (key && st.chat.key === key) || (path && st.chat.path === path);
}

// The "start an agent" row of a folder or sub-project.
function newRow(path, name) {
  const row = el("div", "newrow");
  for (const a of st.adapters) {
    const b = el("button", "", a.name || a.id);
    b.type = "button";
    b.onclick = () => command({ type: "start", agent: a.id, cwd: path }, (r) => {
      toast(`Started ${a.id} in ${name}`);
      st.newFor = null;
      st.openKey = r.key;  // opened once the agents list names it
      agentsChanged();
    });
    row.append(b);
  }
  return row;
}

function renderFolders() {
  const box = $("folders");
  box.replaceChildren();
  let shown = 0;
  for (const f of st.folders) {
    const chats = f.chats.filter((c) => !c.archived);
    const hits = chats.filter((c) => matches(`${c.title || ""} ${c.agent}`));
    if (st.filter && !hits.length && !matches(f.name)) continue;
    shown++;
    const open = st.filter ? true : !st.collapsed.has(f.path);
    const div = el("div", "folder" + (open ? " open" : ""));
    const head = el("div", "folder-head");
    const toggle = el("button", "folder-toggle");
    toggle.type = "button";
    toggle.append(el("span", "chev", "▶"), el("span", "name", f.name), el("span", "n", String(chats.length)));
    toggle.title = f.path;
    toggle.setAttribute("aria-expanded", String(open));
    toggle.onclick = () => {
      if (st.collapsed.has(f.path)) st.collapsed.delete(f.path); else st.collapsed.add(f.path);
      store.set("collapsed", [...st.collapsed]);
      renderFolders();
    };
    head.append(toggle);
    const plus = el("button", "icon-btn", "+");
    plus.type = "button";
    plus.title = `Start an agent in ${f.name}`;
    plus.setAttribute("aria-label", plus.title);
    plus.onclick = () => { st.newFor = st.newFor === f.path ? null : f.path; renderFolders(); };
    head.append(plus);
    div.append(head);

    if (st.newFor === f.path) div.append(newRow(f.path, f.name));
    if (open) {
      const subs = (f.subs || []).filter((sp) => sp.path !== f.path);
      const known = new Set(subs.map((sp) => sp.name));
      const limit = st.showAll.has(f.path) || st.filter ? Infinity : 8;
      // Chats of the folder itself, then each sub-project with its own.
      const own = hits.filter((c) => !c.sub || !known.has(c.sub));
      for (const c of own.slice(0, limit)) div.append(chatRow(c));
      if (own.length > limit) {
        const more = el("button", "more", `Show ${own.length - limit} more`);
        more.type = "button";
        more.onclick = () => { st.showAll.add(f.path); renderFolders(); };
        div.append(more);
      }
      for (const sp of subs) {
        const mine = chats.filter((c) => c.sub === sp.name);
        const shownHere = hits.filter((c) => c.sub === sp.name);
        if (st.filter && !shownHere.length && !matches(sp.name)) continue;
        const sk = `${f.path}\u0000${sp.name}`;
        const sopen = st.filter ? true : !st.collapsed.has(sk);
        const sdiv = el("div", "subfolder" + (sopen ? " open" : ""));
        const shead = el("div", "folder-head sub-head");
        const stoggle = el("button", "folder-toggle");
        stoggle.type = "button";
        stoggle.append(el("span", "chev", "▶"), el("span", "name", sp.name), el("span", "n", String(mine.length)));
        stoggle.title = sp.path;
        stoggle.setAttribute("aria-expanded", String(sopen));
        stoggle.onclick = () => {
          if (st.collapsed.has(sk)) st.collapsed.delete(sk); else st.collapsed.add(sk);
          store.set("collapsed", [...st.collapsed]);
          renderFolders();
        };
        const splus = el("button", "icon-btn", "+");
        splus.type = "button";
        splus.title = `Start an agent in ${sp.name}`;
        splus.setAttribute("aria-label", splus.title);
        splus.onclick = () => { st.newFor = st.newFor === sp.path ? null : sp.path; renderFolders(); };
        shead.append(stoggle, splus);
        sdiv.append(shead);
        if (st.newFor === sp.path) sdiv.append(newRow(sp.path, sp.name));
        if (sopen) {
          const slimit = st.showAll.has(sk) || st.filter ? Infinity : 8;
          for (const c of shownHere.slice(0, slimit)) sdiv.append(chatRow(c));
          if (shownHere.length > slimit) {
            const more = el("button", "more", `Show ${shownHere.length - slimit} more`);
            more.type = "button";
            more.onclick = () => { st.showAll.add(sk); renderFolders(); };
            sdiv.append(more);
          }
          if (!shownHere.length && !st.filter) sdiv.append(el("div", "dim-note", "No chats yet."));
        }
        div.append(sdiv);
      }
      if (!hits.length && !subs.length && !st.filter) div.append(el("div", "dim-note", "No chats yet."));
    }
    box.append(div);
  }
  if (!shown) box.append(el("div", "dim-note", st.filter ? "No chat matches." : "No folders tracked yet."));
}

function chatRow(c) {
  const live = agentFor(c);
  const b = el("button", "row" + (st.chat && c.path === st.chat.path ? " current" : ""));
  b.type = "button";
  if (live) b.append(el("span", `dot lead ${live.status}`));
  else b.append(el("span", "nolead"));
  b.append(el("span", "t", c.title || "(untitled)"));
  b.append(el("span", "w", live ? STATUS[live.status] : ago(c.mtime)));
  b.append(el("span", "m", c.agent));
  b.title = c.title || "";
  b.onclick = () => openChat(c.path, c.title);
  return b;
}

const VIEWS = ["minimal", "normal", "full"];
// On a phone one chip shows the view and a tap goes on to the next.
for (const b of document.querySelectorAll("#views button")) {
  b.onclick = () => setView(narrow.matches ? VIEWS[(VIEWS.indexOf(st.view) + 1) % VIEWS.length] : b.dataset.view);
}
setView(["minimal", "normal", "full"].includes(st.view) ? st.view : "normal");

$("filter").addEventListener("input", (e) => { st.filter = e.target.value.trim().toLowerCase(); renderSide(); });
$("filter").addEventListener("keydown", (e) => {
  if (e.key === "Escape") { e.target.value = ""; st.filter = ""; renderSide(); e.target.blur(); }
});
document.addEventListener("keydown", (e) => {
  const tag = (e.target && e.target.tagName) || "";
  if (e.key === "/" && tag !== "INPUT" && tag !== "TEXTAREA" && !e.ctrlKey && !e.metaKey && !e.altKey) {
    e.preventDefault();
    if (narrow.matches) setScreen("list");
    $("filter").focus();
  } else if (e.key === "Escape" && narrow.matches && tag !== "INPUT" && tag !== "TEXTAREA") {
    $("back").click();
  }
});

// ---------------------------------------------------------------------- chat

function openChat(path, title, key) {
  // The same chat again (resumed, or an agent started in it): keep what is there.
  if (st.chat && path && st.chat.path === path) {
    st.chat.key = key || st.chat.key;
    renderHead();
    renderSide();
    showChatScreen();
    return;
  }
  if (st.chat) st.drafts.set(chatId(), $("prompt-text").value);
  if (st.chat && st.chat.path) send({ type: "close", path: st.chat.path });
  st.chat = { path, key: key || null, title: title || "", start: false };
  st.state = {};
  st.waiting = new Set();
  st.optional = new Set();
  st.unread = 0;
  st.atBottom = true;
  st.loadingOlder = false;
  st.calls.clear();
  st.pics.clear();
  $("events").replaceChildren();
  $("older").hidden = true;
  $("latest").hidden = true;
  $("prompt-text").value = st.drafts.get(chatId()) || "";
  sizePrompt();
  if (path) send({ type: "open", path });
  else $("events").append(notice("Nothing written yet: the chat starts with your first message."));
  keepAddress();
  renderHead();
  renderSide();
  showChatScreen();
}

function chatId() { return st.chat ? (st.chat.path || `key:${st.chat.key}`) : ""; }

// A running agent: its chat once it has one, or the agent itself before then.
function openAgent(a) {
  openChat(a.transcript || null, a.title, a.key);
}

function liveAgent() {
  if (!st.chat) return null;
  return st.agents.find((a) => a.status !== "exited" &&
                        (st.chat.key ? a.key === st.chat.key : a.transcript && a.transcript === st.chat.path));
}

function renderHead() {
  const open = !!st.chat;
  $("chat-head").hidden = !open;
  $("chat").hidden = !open;
  $("empty").hidden = open;
  if (!open) { $("prompt").hidden = true; $("resume-bar").hidden = true; setTitle(); return; }
  const a = liveAgent();
  const found = findChat(st.chat.path);
  if (!st.chat.title && found) st.chat.title = found.chat.title;
  $("chat-title").textContent = (a && a.title) || st.chat.title || "Chat";
  renderMeta(a, found);
  $("stop").hidden = !a;
  $("interrupt").hidden = !(a && a.status === "working");
  const wasHidden = $("prompt").hidden;
  renderPermission(a);
  // A dialog waiting on you takes the message box's place: what is typed
  // then would be typed into the dialog.
  $("prompt").hidden = !a || !!a.permission;
  $("resume-bar").hidden = !!a || !found;
  if (a) $("prompt-text").placeholder = `Message ${a.agent}…`;
  if (a && wasHidden && !st.chat.path && !coarse.matches) $("prompt-text").focus();
  renderHint(a);
  renderActivity(a);
  setTitle();
}

function renderMeta(a, found) {
  const box = $("chat-meta");
  box.replaceChildren();
  const sep = () => el("span", "sep", "·");
  if (a) {
    box.append(el("span", `dot ${a.status}`), el("span", "m-status", STATUS[a.status] || a.status));
    box.append(sep(), el("span", "", a.agent), sep(), el("span", "", base(a.cwd)));
  } else {
    box.append(el("span", "m-status", "Stored chat"));
    if (found) box.append(sep(), el("span", "", found.chat.agent), sep(), el("span", "", ago(found.chat.mtime)));
  }
  const chips = el("span", "chips");
  for (const [k, v] of Object.entries(st.state || {})) {
    if (!v) continue;
    const c = el("span", "chip", v);
    c.title = k;
    chips.append(c);
  }
  if (chips.childNodes.length) box.append(chips);
}

function renderHint(a) {
  const h = $("hint");
  if (!a) { h.textContent = ""; return; }
  const queued = (a.queued || []).length;
  const held = (st.held || []).length;
  if (queued || held) h.textContent = `${queued + held} message${queued + held === 1 ? "" : "s"} waiting for the agent to finish`;
  else if (a.status === "working") h.textContent = "The agent is working: a message now steers it.";
  else h.textContent = "";
}

// What the agent is doing, at the foot of the chat.
function renderActivity(a) {
  const box = $("activity");
  box.replaceChildren();
  box.className = "";
  if (a && a.status === "working") {
    const dots = el("span", "dots");
    dots.append(el("i"), el("i"), el("i"));
    box.append(dots, document.createTextNode("Working"));
  } else if (a && a.status === "waiting") {
    box.className = "waiting";
    box.append(document.createTextNode("Waiting for you."));
  } else {
    box.hidden = true;
    return;
  }
  box.hidden = false;
  if (st.atBottom) scrollToBottom();
}

// The dialog an agent is waiting on (a permission, a plan to approve), as its
// own screen shows it: each choice a button. What is sent is the index; mico
// walks the agent's cursor there and confirms, as its terminal panel does.
let permSig = "";
function renderPermission(a) {
  const box = $("perm");
  const p = a && a.permission;
  if (!p) { box.hidden = true; permSig = ""; box.replaceChildren(); return; }
  // Redrawn only when the dialog is another one: a half-typed note stays.
  const sig = JSON.stringify([a.key, p.title, p.question, p.options]);
  box.hidden = false;
  if (sig === permSig) return;
  permSig = sig;
  box.replaceChildren();
  const head = el("div", "perm-head", p.plan ? "Plan" : "Needs your permission");
  box.append(head);
  for (const t of p.title || []) box.append(el("div", "perm-title", t));
  if (p.question) box.append(el("div", "perm-q", p.question));
  let note = null;
  if (p.amend) {
    note = el("input", "perm-note");
    note.type = "text";
    note.placeholder = "A note for the agent (optional)";
    note.enterKeyHint = "send";
    box.append(note);
  }
  const opts = el("div", "perm-opts");
  (p.options || []).forEach((label, i) => {
    const b = el("button", "perm-opt" + (i === p.cursor ? " cursor" : ""));
    b.type = "button";
    b.append(el("span", "n", String(i + 1)), el("span", "l", label));
    const d = (p.details || [])[i];
    if (d) b.append(el("span", "d", d));
    if ((p.disabled || [])[i]) b.disabled = true;
    b.onclick = () => {
      for (const x of opts.children) x.disabled = true;
      command({ type: "answer_permission", key: a.key, index: i, note: note ? note.value.trim() : "" }, () => {
        toast("Answered");
      });
      // A refused answer leaves the dialog up: let it be tried again.
      setTimeout(() => { permSig = ""; renderHead(); }, 2500);
    };
    opts.append(b);
  });
  box.append(opts);
  box.scrollIntoView({ block: "nearest" });
}

$("interrupt").onclick = () => {
  const a = liveAgent();
  if (a) command({ type: "interrupt", key: a.key }, () => toast("Interrupted"));
};

// ------------------------------------------------------------------ markdown

// Inline: `code`, **bold**, *italic*, ~~strike~~, [text](url) and bare URLs.
// Groups: 1 escaped char, 2-3 code, 4 bold italic, 5 bold, 6 italic, 7 strike,
// 8-9 link, 10 URL.
const INLINE = /\\([\\`*_{}[\]()#+\-.!|~>])|(`+)([\s\S]*?[^`])\2(?!`)|\*\*\*([^*\n]+?)\*\*\*|\*\*(?=\S)([\s\S]+?\S)\*\*|\*(?=[^\s*])([^*\n]+?)\*(?!\*)|~~([^~\n]+)~~|\[([^\]\n]+)\]\(([^)\s]+)(?:\s+"[^"]*")?\)|(https?:\/\/[^\s<>]+)/g;

function safeUrl(u) {
  try {
    const url = new URL(u);  // relative links are not links: there is no page for them to be relative to
    if (url.protocol === "http:" || url.protocol === "https:" || url.protocol === "mailto:") return url.href;
  } catch (_) { /* not a URL */ }
  return null;
}

function link(text, href) {
  const safe = safeUrl(href);
  if (!safe) return document.createTextNode(text);
  const a = el("a", "", text);
  a.href = safe;
  a.target = "_blank";
  a.rel = "noopener noreferrer";
  return a;
}

function inline(parent, text) {
  const rx = new RegExp(INLINE.source, "g");
  let last = 0, m;
  while ((m = rx.exec(text))) {
    if (m.index > last) parent.append(text.slice(last, m.index));
    last = rx.lastIndex;
    if (m[1] !== undefined) parent.append(m[1]);
    else if (m[2] !== undefined) parent.append(el("code", "", m[3]));
    else if (m[4] !== undefined) { const e = el("strong"); const i = el("em"); inline(i, m[4]); e.append(i); parent.append(e); }
    else if (m[5] !== undefined) { const e = el("strong"); inline(e, m[5]); parent.append(e); }
    else if (m[6] !== undefined) { const e = el("em"); inline(e, m[6]); parent.append(e); }
    else if (m[7] !== undefined) { const e = el("del"); inline(e, m[7]); parent.append(e); }
    else if (m[8] !== undefined) {
      const a = link("", m[9]);
      if (a.nodeType === 1) inline(a, m[8]); else a.textContent = m[8];
      parent.append(a);
    } else {
      // A bare URL, without the punctuation that ends a sentence around it.
      let url = m[10];
      for (;;) {
        const c = url[url.length - 1];
        if (c && ".,;:!?'\"]".includes(c)) url = url.slice(0, -1);
        else if (c === ")" && !url.includes("(")) url = url.slice(0, -1);
        else break;
      }
      rx.lastIndex = m.index + url.length;
      last = rx.lastIndex;
      parent.append(link(url, url));
    }
  }
  if (last < text.length) parent.append(text.slice(last));
}

const FENCE = /^\s*(`{3,}|~{3,})\s*([\w+#.-]*)[^\n]*$/;
const HEAD = /^\s{0,3}(#{1,6})\s+(.+?)\s*#*\s*$/;
const HR = /^\s{0,3}([-*_])(?:\s*\1){2,}\s*$/;
const LIST = /^(\s*)([-*+]|\d{1,9}[.)])\s+(.*)$/;
const QUOTE = /^\s{0,3}>\s?(.*)$/;
const TABLE_SEP = /^\s*\|?\s*:?-+:?\s*(\|\s*:?-+:?\s*)*\|?\s*$/;

const indentOf = (l) => l.replace(/\t/g, "    ").match(/^ */)[0].length;

function splitRow(line) {
  let s = line.trim().replace(/\\\|/g, "\u0000");
  if (s.startsWith("|")) s = s.slice(1);
  if (s.endsWith("|")) s = s.slice(0, -1);
  return s.split("|").map((c) => c.trim().replace(/\u0000/g, "|"));
}

function isTableStart(lines, i) {
  return lines[i].includes("|") && i + 1 < lines.length && lines[i + 1].includes("-") &&
         TABLE_SEP.test(lines[i + 1]) && splitRow(lines[i]).length >= 1;
}

function startsBlock(lines, i) {
  const l = lines[i];
  return FENCE.test(l) || HEAD.test(l) || HR.test(l) || LIST.test(l) || QUOTE.test(l) || isTableStart(lines, i);
}

async function copyText(text) {
  try { await navigator.clipboard.writeText(text); return true; } catch (_) { /* not a secure page */ }
  const ta = document.createElement("textarea");
  ta.value = text;
  ta.style.position = "fixed";
  ta.style.opacity = "0";
  document.body.append(ta);
  ta.select();
  let ok = false;
  try { ok = document.execCommand("copy"); } catch (_) { ok = false; }
  ta.remove();
  return ok;
}

function codeBlock(lang, code) {
  if (lang === "chart") {
    const c = chartNode(code);
    if (c) return c;
  }
  const box = el("div", "code");
  const bar = el("div", "code-bar");
  const btn = el("button", "", "Copy");
  btn.type = "button";
  btn.onclick = async () => {
    btn.textContent = (await copyText(code)) ? "Copied" : "Press Ctrl+C";
    setTimeout(() => { btn.textContent = "Copy"; }, 1400);
  };
  bar.append(el("span", "", lang || "text"), btn);
  box.append(bar, el("pre", "block", code));
  return box;
}

function table(lines, i) {
  const head = splitRow(lines[i]);
  const align = splitRow(lines[i + 1]).map((c) => (c.startsWith(":") && c.endsWith(":") ? "c" : c.endsWith(":") ? "r" : ""));
  const wrap = el("div", "md-table");
  const t = el("table");
  const tr = el("tr");
  head.forEach((h, k) => { const th = el("th", align[k] || ""); inline(th, h); tr.append(th); });
  const thead = el("thead");
  thead.append(tr);
  t.append(thead);
  const body = el("tbody");
  i += 2;
  while (i < lines.length && lines[i].trim() && lines[i].includes("|")) {
    const row = el("tr");
    const cells = splitRow(lines[i]);
    for (let k = 0; k < head.length; k++) { const td = el("td", align[k] || ""); inline(td, cells[k] || ""); row.append(td); }
    body.append(row);
    i++;
  }
  t.append(body);
  wrap.append(t);
  return [wrap, i];
}

function list(lines, i) {
  const first = LIST.exec(lines[i]);
  const baseIndent = indentOf(lines[i]);
  const ordered = /\d/.test(first[2]);
  const ul = el(ordered ? "ol" : "ul");
  if (ordered && parseInt(first[2], 10) !== 1) ul.start = parseInt(first[2], 10);
  while (i < lines.length) {
    // A blank line between items keeps one list.
    if (!lines[i].trim()) {
      let j = i + 1;
      while (j < lines.length && !lines[j].trim()) j++;
      const next = j < lines.length ? LIST.exec(lines[j]) : null;
      if (next && indentOf(lines[j]) === baseIndent && /\d/.test(next[2]) === ordered) { i = j; continue; }
      break;
    }
    const m = LIST.exec(lines[i]);
    if (!m || indentOf(lines[i]) !== baseIndent) break;
    const li = el("li");
    let text = m[3];
    const task = /^\[( |x|X)\]\s+/.exec(text);
    if (task) {
      li.className = "task" + (task[1] === " " ? "" : " done");
      li.append(el("span", "box", task[1] === " " ? "☐" : "☑"));
      text = text.slice(task[0].length);
    }
    inline(li, text);
    i++;
    // What is indented under it belongs to it: more paragraphs, a nested list.
    const content = baseIndent + m[2].length + 1;
    const sub = [];
    while (i < lines.length) {
      if (!lines[i].trim()) {
        let j = i + 1;
        while (j < lines.length && !lines[j].trim()) j++;
        if (j < lines.length && indentOf(lines[j]) > baseIndent) { sub.push(""); i++; continue; }
        break;
      }
      if (indentOf(lines[i]) > baseIndent) {
        sub.push(lines[i].replace(new RegExp(`^[ \\t]{0,${content}}`), ""));
        i++;
        continue;
      }
      break;
    }
    if (sub.length) li.append(markdown(sub.join("\n")));
    ul.append(li);
  }
  return [ul, i];
}

function markdown(text) {
  const root = el("div", "md");
  const lines = text.replace(/\r\n?/g, "\n").split("\n");
  let i = 0;
  while (i < lines.length) {
    const line = lines[i];
    if (!line.trim()) { i++; continue; }
    let m;
    if ((m = FENCE.exec(line))) {
      const mark = m[1];
      const body = [];
      i++;
      // Unclosed (a reply still streaming): the rest is the block.
      while (i < lines.length) {
        const t = lines[i].trim();
        if (/^(`{3,}|~{3,})$/.test(t) && t[0] === mark[0] && t.length >= mark.length) { i++; break; }
        body.push(lines[i]);
        i++;
      }
      root.append(codeBlock(m[2], body.join("\n")));
    } else if ((m = /^\s*!\[([^\]]*)\]\(\s*([^)\s]+)[^)]*\)\s*$/.exec(line))) {
      root.append(markdownImage(m[1], m[2]));
      i++;
    } else if ((m = /^\s*\$\$(.*)$/.exec(line))) {
      // A display equation: its LaTeX, set apart; there is no typesetter here.
      let tex = m[1];
      i++;
      if (!/\$\$\s*$/.test(tex)) {
        const body = [tex];
        while (i < lines.length && !/\$\$\s*$/.test(lines[i])) { body.push(lines[i]); i++; }
        if (i < lines.length) body.push(lines[i++]);
        tex = body.join("\n");
      }
      root.append(el("div", "math", tex.replace(/\$\$\s*$/, "").trim()));
    } else if ((m = HEAD.exec(line))) {
      const h = el("h" + m[1].length);
      inline(h, m[2]);
      root.append(h);
      i++;
    } else if (HR.test(line)) {
      root.append(el("hr"));
      i++;
    } else if (isTableStart(lines, i)) {
      const [t, next] = table(lines, i);
      root.append(t);
      i = next;
    } else if (QUOTE.test(line)) {
      const inner = [];
      while (i < lines.length && QUOTE.test(lines[i])) { inner.push(QUOTE.exec(lines[i])[1]); i++; }
      const bq = el("blockquote");
      bq.append(markdown(inner.join("\n")));
      root.append(bq);
    } else if (LIST.test(line)) {
      const [l, next] = list(lines, i);
      root.append(l);
      i = next;
    } else {
      const para = [];
      while (i < lines.length && lines[i].trim() && (para.length === 0 || !startsBlock(lines, i))) { para.push(lines[i]); i++; }
      const p = el("p");
      inline(p, para.join("\n"));
      root.append(p);
    }
  }
  return root;
}

// ------------------------------------------------------------------ pictures

const MEDIA = new Set(["image/png", "image/jpeg", "image/gif", "image/webp"]);

function picture(alt) {
  const fig = el("figure", "pic loading");
  fig.append(el("span", "cap", alt ? `${alt} …` : "Loading image…"));
  return fig;
}

function showPicture(fig, alt, media, data) {
  fig.classList.remove("loading");
  fig.replaceChildren();
  if (!MEDIA.has(media)) { fig.append(el("span", "cap", alt || "image")); return; }
  const img = el("img");
  img.alt = alt || "image";
  img.src = `data:${media};base64,${data}`;
  img.onclick = () => fig.classList.toggle("zoom");
  img.title = "Click to enlarge";
  fig.append(img);
  if (alt) fig.append(el("figcaption", "", alt));
}

function ask(key, fig, alt, msg) {
  if (!st.chat || !st.chat.path) { fig.firstChild.textContent = alt || "image"; return; }
  const list = st.pics.get(key) || [];
  list.push({ fig, alt });
  st.pics.set(key, list);
  if (list.length === 1) send({ ...msg, path: st.chat.path });
}

// An image the transcript holds: asked for when it is drawn, kept off the
// wire until then.
function pictureNode(ev) {
  const n = el("div", "ev assistant");
  const [, off, len] = (ev.summary || "").split(" ");
  const fig = picture("");
  n.append(fig);
  if (off === undefined || len === undefined) { fig.firstChild.textContent = "image"; return n; }
  ask(`t:${ev.at}:${off}`, fig, "", { type: "image", at: ev.at, off: Number(off), len: Number(len) });
  return n;
}

// ![alt](file) in a message: a file on the machine, or only its link.
function markdownImage(alt, target) {
  let file = null;
  if (target.startsWith("file://")) { try { file = decodeURIComponent(new URL(target).pathname); } catch (_) { /* odd */ } }
  else if (target.startsWith("/")) file = target;
  if (!file) {
    const p = el("p");
    p.append(link(alt || target, target));
    return p;
  }
  const fig = picture(alt);
  ask(`f:${file}`, fig, alt, { type: "file_image", file });
  return fig;
}

function gotPicture(m) {
  if (!st.chat || m.path !== st.chat.path) return;
  const key = m.type === "image" ? `t:${m.at}:${m.off}` : `f:${m.file}`;
  const list = st.pics.get(key) || [];
  st.pics.delete(key);
  for (const { fig, alt } of list) {
    if (m.error) { fig.classList.remove("loading"); fig.replaceChildren(el("span", "cap", `${alt || "image"} (${m.error})`)); }
    else showPicture(fig, alt, m.media, m.data);
  }
  if (st.atBottom) scrollToBottom();
}

// ------------------------------------------------------------------- charts

const SVGNS = "http://www.w3.org/2000/svg";
const svg = (tag, attrs, text) => {
  const e = document.createElementNS(SVGNS, tag);
  for (const k in attrs) e.setAttribute(k, attrs[k]);
  if (text !== undefined) e.textContent = text;
  return e;
};
const PALETTE = ["#2d6cb5", "#d9822b", "#3d8b4f", "#b23b3b", "#7b5ea7", "#2a9d9d", "#a67c00", "#d6578c"];

function niceTicks(lo, hi, n) {
  if (!(hi > lo)) { hi = lo + 1; }
  const step0 = (hi - lo) / n, mag = Math.pow(10, Math.floor(Math.log10(step0)));
  const step = [1, 2, 5, 10].map((k) => k * mag).find((v) => v >= step0) || mag * 10;
  const out = [];
  for (let v = Math.ceil(lo / step) * step; v <= hi + step * 1e-9; v += step) out.push(+v.toPrecision(12));
  return out;
}
const fmt = (v) => (Math.abs(v) >= 1e5 || (v !== 0 && Math.abs(v) < 1e-3) ? v.toExponential(1) : String(+v.toPrecision(4)));

function chartNode(code) {
  let spec;
  try { spec = JSON.parse(code); } catch (_) { return null; }
  if (!spec || typeof spec !== "object") return null;
  const box = el("div", "chart");
  if (Array.isArray(spec.subplots)) {
    const cols = Math.max(1, Math.min(4, spec.columns | 0 || 1));
    box.style.display = "grid";
    box.style.gridTemplateColumns = `repeat(${cols}, minmax(0, 1fr))`;
    for (const sp of spec.subplots) box.append(chartOne(sp));
  } else box.append(chartOne(spec));
  return box;
}

function chartOne(spec) {
  const wrap = el("div", "chart-one");
  if (spec.title) wrap.append(el("div", "chart-title", String(spec.title)));
  if (spec.file) { wrap.append(el("div", "dim-note", `Chart of ${spec.file}: drawn from a file in the terminal view.`)); return wrap; }
  const W = Math.max(280, Math.min(560, ($("events").clientWidth || 560) - 4)), H = Math.max(120, Math.min(480, (spec.height | 0 || 12) * 20)), L = 48, R = 12, T = 8, B = 30;
  const root = svg("svg", { viewBox: `0 0 ${W} ${H}`, class: "chart-svg", role: "img", "aria-label": spec.title || "chart" });
  const type = spec.type || "line";
  const num = (a) => (Array.isArray(a) ? a.map(Number).filter(Number.isFinite) : []);
  const frame = (xlo, xhi, ylo, yhi, xl, yl) => {
    const X = (v) => L + ((v - xlo) / (xhi - xlo || 1)) * (W - L - R);
    const logy = spec.log_y && ylo > 0;
    const Y = (v) => T + (1 - ((logy ? Math.log10(v) : v) - (logy ? Math.log10(ylo) : ylo)) / ((logy ? Math.log10(yhi) - Math.log10(ylo) : yhi - ylo) || 1)) * (H - T - B);
    for (const t of niceTicks(ylo, yhi, 4)) {
      if (t < ylo || t > yhi) continue;
      root.append(svg("line", { x1: L, x2: W - R, y1: Y(t), y2: Y(t), stroke: "currentColor", "stroke-opacity": ".12" }));
      root.append(svg("text", { x: L - 6, y: Y(t) + 4, "text-anchor": "end", "font-size": 11, fill: "currentColor", "fill-opacity": ".7" }, fmt(t)));
    }
    if (xl !== null) for (const t of niceTicks(xlo, xhi, 5)) {
      if (t < xlo || t > xhi) continue;
      root.append(svg("text", { x: X(t), y: H - B + 15, "text-anchor": "middle", "font-size": 11, fill: "currentColor", "fill-opacity": ".7" }, fmt(t)));
    }
    root.append(svg("line", { x1: L, x2: L, y1: T, y2: H - B, stroke: "currentColor", "stroke-opacity": ".4" }));
    root.append(svg("line", { x1: L, x2: W - R, y1: H - B, y2: H - B, stroke: "currentColor", "stroke-opacity": ".4" }));
    if (spec.xlabel) root.append(svg("text", { x: (L + W - R) / 2, y: H - 3, "text-anchor": "middle", "font-size": 11, fill: "currentColor" }, String(spec.xlabel)));
    if (spec.ylabel) root.append(svg("text", { x: 4, y: T + 8, "font-size": 11, fill: "currentColor" }, String(spec.ylabel)));
    return { X, Y };
  };
  const series = (Array.isArray(spec.series) ? spec.series : []).map((s) => ({ name: s.name || "", y: num(s.y), x: s.x ? num(s.x) : null }));
  let legend = series.length > 1;
  if (type === "heatmap" && Array.isArray(spec.z)) {
    const z = spec.z.map(num), rows = z.length, cols = Math.max(1, ...z.map((r) => r.length));
    const flat = z.flat(), lo = Math.min(...flat), hi = Math.max(...flat);
    const cw = (W - L - R) / cols, ch = (H - T - B) / rows;
    z.forEach((r, i) => r.forEach((v, j) => {
      const t = (v - lo) / (hi - lo || 1);
      root.append(svg("rect", { x: L + j * cw, y: T + i * ch, width: cw + .5, height: ch + .5, fill: `hsl(${220 - 200 * t} 70% ${35 + 25 * t}%)` }));
    }));
    root.append(svg("rect", { x: L, y: T, width: W - L - R, height: H - T - B, fill: "none", stroke: "currentColor", "stroke-opacity": ".4" }));
    legend = false;
  } else if (type === "bar") {
    const labels = (spec.labels || []).map(String), all = series.flatMap((s) => s.y);
    const hi = Math.max(0, ...all), lo = Math.min(0, ...all), n = Math.max(labels.length, ...series.map((s) => s.y.length), 1);
    const { X, Y } = frame(0, n, lo, hi === lo ? lo + 1 : hi, null);
    const slot = (W - L - R) / n, bw = slot * 0.8 / Math.max(1, series.length);
    series.forEach((s, k) => s.y.forEach((v, i) => {
      root.append(svg("rect", { x: L + i * slot + slot * 0.1 + k * bw, y: Math.min(Y(v), Y(0)), width: bw, height: Math.abs(Y(v) - Y(0)), fill: PALETTE[k % PALETTE.length] }));
    }));
    labels.slice(0, n).forEach((t, i) => root.append(svg("text", { x: L + (i + .5) * slot, y: H - B + 15, "text-anchor": "middle", "font-size": 11, fill: "currentColor", "fill-opacity": ".7" }, t.length > 12 ? t.slice(0, 11) + "…" : t)));
  } else if (type === "hist") {
    const v = num(spec.values);
    if (!v.length) return wrap;
    const lo = Math.min(...v), hi = Math.max(...v), bins = Math.max(5, Math.min(40, Math.ceil(Math.sqrt(v.length) * 1.5)));
    const counts = new Array(bins).fill(0);
    for (const x of v) counts[Math.min(bins - 1, Math.floor(((x - lo) / (hi - lo || 1)) * bins))]++;
    const { X, Y } = frame(lo, hi === lo ? lo + 1 : hi, 0, Math.max(...counts), 1);
    counts.forEach((c, i) => root.append(svg("rect", { x: X(lo + (i / bins) * (hi - lo)), y: Y(c), width: (W - L - R) / bins - 1, height: H - B - Y(c), fill: PALETTE[0] })));
  } else {
    // line, scatter, spark: the series against x, or their index.
    const pts = series.map((s) => s.y.map((y, i) => [s.x && s.x.length > i ? s.x[i] : (spec.x && num(spec.x).length > i ? num(spec.x)[i] : i), y]));
    const xs = pts.flat().map((p) => p[0]), ys = pts.flat().map((p) => p[1]).filter((y) => !spec.log_y || y > 0);
    if (!xs.length || !ys.length) return wrap;
    const ylo = Math.min(...ys), yhi = Math.max(...ys);
    const { X, Y } = frame(Math.min(...xs), Math.max(...xs), ylo, yhi === ylo ? ylo + 1 : yhi, 1);
    pts.forEach((p, k) => {
      const color = PALETTE[k % PALETTE.length];
      const ok = p.filter((q) => !spec.log_y || q[1] > 0);
      if (type === "scatter") for (const q of ok) root.append(svg("circle", { cx: X(q[0]), cy: Y(q[1]), r: 2.6, fill: color }));
      else root.append(svg("polyline", { points: ok.map((q) => `${X(q[0])},${Y(q[1])}`).join(" "), fill: "none", stroke: color, "stroke-width": 1.8, "stroke-linejoin": "round" }));
    });
  }
  wrap.append(root);
  if (legend) {
    const lg = el("div", "chart-legend");
    series.forEach((s, k) => {
      const item = el("span", "key", s.name || `series ${k + 1}`);
      item.style.setProperty("--c", PALETTE[k % PALETTE.length]);
      lg.append(item);
    });
    wrap.append(lg);
  }
  return wrap;
}

// ------------------------------------------------------------------- events

// Minimal, normal or full: how much of the work shows. Minimal keeps the
// conversation and folds every run of steps to its line; normal opens short
// runs; full opens everything, thinking and tool output too.
function applyView(root) {
  const view = st.view;
  const scope = root || $("events");
  for (const g of scope.querySelectorAll("details.tools")) {
    const calls = g.querySelectorAll("details.tool").length;
    g.open = view === "full" || (view === "normal" && (calls <= 4 || !!g.querySelector(".running")));
    if (g.querySelector(".running") && view !== "minimal") g.open = true;
  }
  for (const d of scope.querySelectorAll("details.tool")) if (view === "full") d.open = true; else if (!d.classList.contains("failed")) d.open = false;
  for (const d of scope.querySelectorAll("details.thinking")) d.open = view === "full";
  $("events").dataset.view = view;
}

function setView(v) {
  st.view = v;
  store.set("view", v);
  for (const b of document.querySelectorAll("#views button")) b.setAttribute("aria-pressed", String(b.dataset.view === v));
  applyView();
}

const TOOL_ICON = {
  Bash: "$", Read: "≡", Write: "✎", Edit: "✎", MultiEdit: "✎", NotebookEdit: "✎", Grep: "⌕", Glob: "⌕",
  WebFetch: "↗", WebSearch: "↗", Task: "◈", Agent: "◈", TodoWrite: "☑", exec: "$", apply_patch: "✎",
};

function notice(text, bad) {
  const d = el("div", "ev notice" + (bad ? " bad" : ""));
  d.append(el("span", "", text));
  return d;
}

function toolCall(ev) {
  const d = el("details", "tool");
  const sum = el("summary");
  sum.append(el("span", "ico", TOOL_ICON[ev.name] || "·"), el("span", "name", ev.name || "tool"));
  sum.append(el("span", "sum", ev.summary || ""), el("span", "st"));
  d.append(sum);
  if (ev.detail) d.append(el("pre", "detail block", ev.detail));
  if (ev.tool) {
    st.calls.set(ev.tool, d);
    d.classList.add("running");
  }
  return d;
}

// A run of tool calls is one group, folded to a line: "4 tool calls".
function groupHeader(g) {
  const calls = g.querySelectorAll("details.tool");
  const failed = g.querySelectorAll("details.tool.failed").length;
  const running = g.querySelectorAll("details.tool.running").length;
  const sum = g.firstChild;
  sum.replaceChildren(document.createTextNode(`${calls.length} tool call${calls.length === 1 ? "" : "s"}`));
  if (running) sum.append(el("span", "flag run", ` · ${running} running`));
  if (failed) sum.append(el("span", "flag bad", ` · ${failed} failed`));
}

function newGroup() {
  const g = el("details", "ev tools");
  g.open = true;
  g.append(el("summary"), el("div", "tools-body"));
  return g;
}

function toolResult(ev) {
  const call = ev.tool && st.calls.get(ev.tool);
  const text = ev.text || "";
  if (!call) {
    const d = el("details", "tool" + (ev.ok === false ? " failed" : " ok"));
    const sum = el("summary");
    sum.append(el("span", "ico", "·"), el("span", "name", "result"), el("span", "sum", text.split("\n")[0].slice(0, 120)), el("span", "st"));
    d.append(sum, el("pre", "out block", text || "(no output)"));
    return { node: d, standalone: true };
  }
  if (call.classList.contains("question")) {
    call.classList.remove("waiting");
    for (const x of call.querySelectorAll(".hint, .submit, .multi")) x.remove();
    for (const ul of call.querySelectorAll("ul.pick")) ul.classList.remove("pick");
    call.querySelector(".badge").textContent = "Question · answered";
    if (text) call.append(el("div", "answer", text));
    return { node: null };
  }
  call.classList.remove("running");
  call.classList.add(ev.ok === false ? "failed" : "ok");
  const out = el("pre", "out block" + (ev.ok === false ? " err" : ""), text || "(no output)");
  call.append(out);
  if (ev.ok === false) call.open = true;
  const g = call.closest("details.tools");
  if (g) groupHeader(g);
  return { node: null };
}

function questionNode(ev) {
  const d = el("div", "ev question");
  d.append(el("div", "badge", "Question"));
  let qs = [];
  try { qs = JSON.parse(ev.detail || "[]"); } catch (_) { qs = []; }
  d._q = { tool: ev.tool, qs: [], lists: [] };
  if (!Array.isArray(qs) || !qs.length) {
    d.append(el("div", "q", ev.text || "A question"));
  } else {
    for (const q of qs) {
      const head = el("div", "q");
      if (q.header) head.append(el("span", "qh", q.header));
      head.append(document.createTextNode(q.question || q.title || ""));
      d.append(head);
      const ul = el("ul");
      for (const o of q.options || []) {
        const li = el("li");
        li.append(document.createTextNode(typeof o === "string" ? o : o.label || ""));
        if (o && o.description) li.append(el("span", "d", o.description));
        ul.append(li);
      }
      d._q.qs.push({ multi: !!q.multiSelect, picked: new Set() });
      d._q.lists.push(ul);
      d.append(ul);
    }
  }
  if (ev.tool) {
    st.calls.set(ev.tool, d);
    if (st.waiting.has(ev.tool)) markWaiting(d);
    else if (st.optional.has(ev.tool)) markOptional(d);
  }
  return d;
}

// A question waiting on you: its options become buttons, and Send answers it
// the way mico's terminal card does.
function markWaiting(d) {
  d.classList.add("waiting");
  d.querySelector(".badge").textContent = "Question · waiting for you";
  const hint = d.querySelector(".hint");
  if (hint) hint.remove();
  const q = d._q;
  if (!q || !q.qs.length || d.querySelector(".submit")) return;
  q.qs.forEach((spec, qi) => {
    const ul = q.lists[qi];
    ul.classList.add("pick");
    [...ul.children].forEach((li, oi) => {
      li.tabIndex = 0;
      li.setAttribute("role", spec.multi ? "checkbox" : "radio");
      const toggle = () => {
        if (!spec.multi) { spec.picked.clear(); for (const x of ul.children) x.classList.remove("sel"); }
        if (spec.picked.has(oi)) { spec.picked.delete(oi); li.classList.remove("sel"); }
        else { spec.picked.add(oi); li.classList.add("sel"); }
        li.setAttribute("aria-checked", String(spec.picked.has(oi)));
        send.disabled = !q.qs.every((s) => s.picked.size > 0);
      };
      li.onclick = toggle;
      li.onkeydown = (e) => { if (e.key === " " || e.key === "Enter") { e.preventDefault(); toggle(); } };
    });
    if (spec.multi) ul.before(el("div", "multi", "Choose any"));
  });
  const send = el("button", "submit primary", "Send answer");
  send.type = "button";
  send.disabled = true;
  send.onclick = () => {
    const a = liveAgent();
    if (!a) return;
    send.disabled = true;
    command({ type: "answer", key: a.key, tool: q.tool, chosen: q.qs.map((s) => [...s.picked].sort((x, y) => x - y)) }, () => {
      send.textContent = "Answer sent…";
      for (const ul of q.lists) ul.classList.remove("pick");
    });
    // A refused answer leaves the question up: let it be sent again.
    setTimeout(() => { if (d.classList.contains("waiting")) send.disabled = !q.qs.every((s) => s.picked.size > 0); }, 2500);
  };
  d.append(send);
}

// Answered with a message, not through the agent's menu (codex's optional questions).
function markOptional(d) {
  d.classList.add("waiting");
  d.querySelector(".badge").textContent = "Question · reply with a message";
  if (!d.querySelector(".hint")) d.append(el("div", "hint", "Type your reply below."));
}

// Builds the nodes for a batch of events. `last` is the group the page ends in,
// which a following tool call joins.
function buildEvents(events, last) {
  const frag = document.createDocumentFragment();
  let group = last || null;
  let visible = 0;
  for (const ev of events) {
    if (ev.k === "tool_call") {
      if (!group) { group = newGroup(); frag.append(group); }
      group.querySelector(".tools-body").append(toolCall(ev));
      groupHeader(group);
      visible++;
      continue;
    }
    if (ev.k === "tool_result") {
      const r = toolResult(ev);
      if (r.node) { frag.append(r.node); group = null; visible++; }
      continue;
    }
    if (ev.k === "turn_end" || ev.k === "meta" || ev.k === "queue_add" || ev.k === "queue_take") continue;
    group = null;
    let n = null;
    switch (ev.k) {
      case "user": {
        n = el("div", "ev user");
        n.append(el("div", "bubble", ev.text || ""));
        break;
      }
      case "assistant": {
        n = el("div", "ev assistant");
        n.append(markdown(ev.text || ""));
        break;
      }
      case "thinking": {
        n = el("details", "ev thinking");
        n.append(el("summary", "", "Thinking"), el("div", "body", ev.text || ""));
        break;
      }
      case "question": n = questionNode(ev); break;
      case "notice": case "task": n = notice(ev.text || "", ev.ok === false); break;
      case "peer": {
        n = el("div", "ev peer");
        n.append(el("div", "from", (ev.name || "agent") + (ev.summary ? " \u2192 " + ev.summary : "")));
        n.append(markdown(ev.text || ""));
        break;
      }
      case "chart": n = el("div", "ev assistant"); n.append(markdown(ev.text || "")); break;
      case "image": n = pictureNode(ev); break;
      default: break;
    }
    if (n) { frag.append(n); visible++; }
  }
  return { frag, visible };
}

function lastGroup() {
  const kids = $("events").children;
  const l = kids[kids.length - 1];
  return l && l.classList.contains("tools") ? l : null;
}

const box = () => $("chat");
function scrollToBottom() { box().scrollTop = box().scrollHeight; }

function addEvents(m) {
  const list = $("events");
  const chat = box();
  const wasBottom = st.atBottom;
  if (m.where === "older") {
    const { frag } = buildEvents(m.events, null);
    // Long histories fold their long runs of tool calls; what is live stays open.
    for (const g of frag.querySelectorAll("details.tools")) {
      if (g.querySelectorAll("details.tool").length > 4 && !g.querySelector(".running")) g.open = false;
    }
    // Keep what is on screen where it is as history goes in above it.
    const before = chat.scrollHeight;
    applyView(frag);
    list.prepend(frag);
    chat.scrollTop += chat.scrollHeight - before;
    st.loadingOlder = false;
  } else {
    const { frag, visible } = buildEvents(m.events, m.where === "newer" ? lastGroup() : null);
    if (m.where === "tail") {
      for (const g of frag.querySelectorAll("details.tools")) {
        if (g.querySelectorAll("details.tool").length > 4 && !g.querySelector(".running")) g.open = false;
      }
    }
    applyView(frag);
    list.append(frag);
    if (m.where === "tail" || wasBottom) { scrollToBottom(); st.atBottom = true; }
    else if (visible) { st.unread += visible; showLatest(); }
  }
  if (m.where !== "newer") st.chat.start = m.start;
  $("older").hidden = st.chat.start;
  renderActivity(liveAgent());
}

function showLatest() {
  const b = $("latest");
  b.hidden = false;
  b.textContent = st.unread ? `↓ ${st.unread} new` : "↓ Latest";
}

$("chat").addEventListener("scroll", () => {
  const c = box();
  st.atBottom = c.scrollHeight - c.scrollTop - c.clientHeight < 80;
  if (st.atBottom) { st.unread = 0; $("latest").hidden = true; }
  else if (c.scrollHeight - c.scrollTop - c.clientHeight > 600) showLatest();
  // Reaching the top reads the next slab of history.
  if (c.scrollTop < 80 && st.chat && st.chat.path && !st.chat.start && !st.loadingOlder) loadOlder();
}, { passive: true });

$("latest").onclick = () => { scrollToBottom(); st.unread = 0; $("latest").hidden = true; };

function loadOlder() {
  if (!st.chat || !st.chat.path || st.loadingOlder) return;
  st.loadingOlder = true;
  send({ type: "older", path: st.chat.path });
  setTimeout(() => { st.loadingOlder = false; }, 4000);  // a lost answer must not stop it for good
}
$("older").onclick = loadOlder;

function applyState(m) {
  st.state = m.state || {};
  st.held = m.held || [];
  st.waiting = new Set(m.waiting || []);
  st.optional = new Set(m.optional || []);
  // Cards already drawn: waiting, or not any more.
  for (const [id, node] of st.calls)
    if (node.classList && node.classList.contains("question") && !node.querySelector(".answer")) {
      if (st.waiting.has(id)) markWaiting(node);
      else if (st.optional.has(id)) markOptional(node);
    }
  renderHead();
}

// -------------------------------------------------------------------- prompt

function sizePrompt() {
  const t = $("prompt-text");
  t.style.height = "auto";
  t.style.height = Math.min(t.scrollHeight, 200) + "px";
  $("send").disabled = !t.value.trim();
}

$("prompt-text").addEventListener("input", sizePrompt);

$("prompt").onsubmit = (e) => {
  e.preventDefault();
  const a = liveAgent();
  const text = $("prompt-text").value.trim();
  if (!a || !text) return;
  $("send").disabled = true;
  command({ type: "send", key: a.key, text }, () => {
    $("prompt-text").value = "";
    st.drafts.delete(chatId());
    sizePrompt();
    st.atBottom = true;
    scrollToBottom();
  });
  // A refused message leaves the text where it was.
  setTimeout(sizePrompt, 600);
};

// Enter sends with a keyboard; on a touch screen it is a new line, and Send is the button.
$("prompt-text").addEventListener("keydown", (e) => {
  if (e.key === "Enter" && !e.shiftKey && !e.isComposing && !coarse.matches) {
    e.preventDefault();
    $("prompt").requestSubmit();
  }
});

$("stop").onclick = () => {
  const a = liveAgent();
  if (!a) return;
  if (!confirm(`Stop ${a.agent}?\n\nIts chat stays; you can resume it.`)) return;
  command({ type: "stop", key: a.key }, () => toast("Stopped"));
};

function continueChat(fork) {
  const found = st.chat && findChat(st.chat.path);
  if (!found) return;
  command({ type: "resume", agent: found.chat.agent, id: found.chat.id, fork }, (r) => {
    toast(fork ? "Forked into a new chat" : "Resuming…");
    st.openKey = r.key;
    agentsChanged();
  });
}
$("resume").onclick = () => continueChat(false);
$("fork").onclick = () => continueChat(true);

// The keyboard on a phone: the page is the height of what is left above it.
if (window.visualViewport) {
  const fit = () => {
    document.documentElement.style.setProperty("--vh", window.visualViewport.height + "px");
    if (st.atBottom) scrollToBottom();
  };
  window.visualViewport.addEventListener("resize", fit);
  fit();
}

setInterval(renderSide, 60000);  // "3m" moves on its own
connect();
