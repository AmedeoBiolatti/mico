// mico's web view: a client of the state protocol (src/api/client.h).
// Everything from a transcript goes into the page as text, never as markup.
"use strict";

const $ = (id) => document.getElementById(id);
const el = (tag, cls, text) => {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text !== undefined) e.textContent = text;
  return e;
};

const params = new URLSearchParams(location.hash.slice(1));
const token = params.get("token") || "";
// #token=…&chat=<transcript path> opens that chat: a chat can be bookmarked.
const startChat = params.get("chat");
const st = {
  ws: null,
  adapters: [],
  folders: [],
  agents: [],
  chat: null,        // {path, key, title, start}: a stored chat, or a running agent
  openKey: null,     // an agent just started here, to open once it is listed
  calls: new Map(),  // tool id -> its <details>, so a result lands under its call
  rid: 0,
  pending: new Map(),  // rid -> what to do with the result
  backoff: 500,
};

// ------------------------------------------------------------------ connection

function connect() {
  if (!token) {
    status("No token: open the URL mico gave you (:web in mico).");
    return;
  }
  const ws = new WebSocket(`ws://${location.host}/ws?token=${encodeURIComponent(token)}`);
  st.ws = ws;
  ws.onopen = () => {
    st.backoff = 500;
    $("conn").className = "conn on";
    status("");
    if (st.chat && st.chat.path) send({ type: "open", path: st.chat.path });
    else if (startChat) openChat(startChat, "");
  };
  ws.onmessage = (e) => receive(JSON.parse(e.data));
  ws.onclose = () => {
    $("conn").className = "conn off";
    status("Disconnected from mico; trying again…");
    setTimeout(connect, st.backoff);
    st.backoff = Math.min(st.backoff * 2, 10000);
  };
}

function send(msg) {
  if (st.ws && st.ws.readyState === WebSocket.OPEN) st.ws.send(JSON.stringify(msg));
}

function command(msg, then) {
  msg.rid = String(++st.rid);
  if (then) st.pending.set(msg.rid, then);
  send(msg);
}

function status(text) { $("status").textContent = text; }

function receive(m) {
  switch (m.type) {
    case "hello": st.adapters = m.adapters || []; renderFolders(); break;
    case "folders": st.folders = m.folders; renderFolders(); renderHead(); break;
    case "agents": st.agents = m.agents; agentsChanged(); break;
    case "chat": if (st.chat && m.path === st.chat.path) addEvents(m); break;
    case "chat_state": if (st.chat && m.path === st.chat.path) applyState(m); break;
    case "result": {
      const then = st.pending.get(m.rid);
      st.pending.delete(m.rid);
      if (!m.ok) status(m.error || "that did not work");
      else if (then) then(m);
      break;
    }
    case "error": status(m.message); break;
  }
}

// ------------------------------------------------------------------- sidebar

function ago(seconds) {
  const d = Date.now() / 1000 - seconds;
  if (d < 60) return "now";
  if (d < 3600) return `${Math.floor(d / 60)}m`;
  if (d < 86400) return `${Math.floor(d / 3600)}h`;
  return `${Math.floor(d / 86400)}d`;
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
      send({ type: "open", path: a.transcript });
    }
  }
  renderAgents();
  renderHead();
}

function renderAgents() {
  const ul = $("agents");
  ul.replaceChildren();
  if (!st.agents.length) ul.append(el("li", "sub", "nothing running"));
  for (const a of st.agents) {
    const li = el("li");
    li.append(el("span", `dot ${a.status}`), document.createTextNode(a.title || a.agent));
    li.title = `${a.agent} · ${a.status} · ${a.cwd}`;
    if (st.chat && (st.chat.key === a.key || (a.transcript && a.transcript === st.chat.path)))
      li.classList.add("current");
    li.onclick = () => openAgent(a);
    ul.append(li);
  }
}

function renderFolders() {
  const box = $("folders");
  box.replaceChildren();
  for (const f of st.folders) {
    const div = el("div", "folder");
    const head = el("div", "folder-head");
    head.append(el("span", "name", f.name));
    head.title = f.path;
    for (const a of st.adapters) {
      const b = el("button", "", `+ ${a.id}`);
      b.title = `start ${a.name} in ${f.path}`;
      b.onclick = () => command({ type: "start", agent: a.id, cwd: f.path }, (r) => {
        status(`started ${a.id} in ${f.name}`);
        st.openKey = r.key;  // opened once the agents list names it
        agentsChanged();
      });
      head.append(b);
    }
    div.append(head);
    const ul = el("ul", "list");
    for (const c of f.chats.filter((c) => !c.archived).slice(0, 40)) {
      const li = el("li");
      li.append(document.createTextNode(c.title || "(untitled)"), el("span", "sub", ` · ${c.agent} · ${ago(c.mtime)}`));
      li.title = c.title;
      if (st.chat && c.path === st.chat.path) li.classList.add("current");
      li.onclick = () => openChat(c.path, c.title);
      ul.append(li);
    }
    div.append(ul);
    box.append(div);
  }
}

// ---------------------------------------------------------------------- chat

function openChat(path, title, key) {
  if (st.chat && st.chat.path) send({ type: "close", path: st.chat.path });
  st.chat = { path, key: key || null, title: title || "", start: false };
  st.calls.clear();
  $("events").replaceChildren();
  $("older").hidden = true;
  if (path) send({ type: "open", path });
  else $("events").append(el("div", "ev notice", "Nothing written yet: the chat starts with your first message."));
  renderHead();
  renderAgents();
  renderFolders();
}

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
  if (!st.chat) return;
  const a = liveAgent();
  if (!st.chat.title)
    for (const f of st.folders)
      for (const c of f.chats) if (c.path === st.chat.path) st.chat.title = c.title;
  $("chat-title").textContent = (a && a.title) || st.chat.title || st.chat.path;
  $("chat-meta").textContent = a ? `${a.agent} · ${a.status} · ${a.cwd}` : "stored chat";
  const wasHidden = $("prompt").hidden;
  $("prompt").hidden = !a;
  if (a && wasHidden && !st.chat.path) $("prompt-text").focus();
}

// A paragraph with **bold** and `code` marked, each piece as text.
function inline(text) {
  const div = el("div", "text");
  for (const piece of text.split(/(\*\*[^*\n]+\*\*|`[^`\n]+`)/)) {
    if (/^\*\*.+\*\*$/.test(piece)) div.append(el("strong", "", piece.slice(2, -2)));
    else if (/^`.+`$/.test(piece)) div.append(el("code", "", piece.slice(1, -1)));
    else if (piece) div.append(document.createTextNode(piece));
  }
  return div;
}

// Text with ``` fences shown as code blocks; the rest as paragraphs.
function prose(text) {
  const frag = document.createDocumentFragment();
  let code = false;
  for (const part of text.split(/(^```[^\n]*$)/m)) {
    if (/^```/.test(part)) { code = !code; continue; }
    if (!part) continue;
    if (code) frag.append(el("pre", "", part.replace(/^\n/, "").replace(/\n$/, "")));
    else frag.append(inline(part.replace(/^\n+|\n+$/g, "")));
  }
  return frag;
}

function eventNode(ev) {
  switch (ev.k) {
    case "user": return el("div", "ev user", ev.text || "");
    case "assistant": {
      const d = el("div", "ev assistant");
      d.append(prose(ev.text || ""));
      return d;
    }
    case "thinking": {
      const d = el("details", "ev thinking");
      d.append(el("summary", "", "thinking"), el("div", "text", ev.text || ""));
      return d;
    }
    case "tool_call": {
      const d = el("details", "ev tool");
      const sum = el("summary");
      sum.append(el("span", "name", ev.name || "tool"), document.createTextNode(ev.summary ? ` ${ev.summary}` : ""));
      d.append(sum);
      if (ev.detail) d.append(el("pre", "", ev.detail));
      if (ev.tool) {
        st.calls.set(ev.tool, d);
        d.classList.add("running");
      }
      return d;
    }
    case "tool_result": {
      const call = ev.tool && st.calls.get(ev.tool);
      const body = el("pre", "", ev.text || "(no output)");
      if (call) {
        call.classList.remove("running");
        if (ev.ok === false) call.classList.add("failed");
        call.append(body);
        return null;
      }
      const d = el("details", "ev tool" + (ev.ok === false ? " failed" : ""));
      d.append(el("summary", "", "result"), body);
      return d;
    }
    case "question": return questionNode(ev);
    case "notice": return el("div", "ev notice", ev.text || "");
    case "task": return el("div", "ev notice", ev.text || "");
    case "chart": {
      const d = el("div", "ev");
      d.append(prose(ev.text || ""));
      return d;
    }
    case "image": return el("div", "ev notice", "[image]");
    default: return null;  // turn_end, queue records, meta
  }
}

function questionNode(ev) {
  const d = el("div", "ev question");
  let qs = [];
  try { qs = JSON.parse(ev.detail || "[]"); } catch (_) { qs = []; }
  if (!Array.isArray(qs) || !qs.length) {
    d.append(el("div", "q", ev.text || "a question"));
    return d;
  }
  for (const q of qs) {
    d.append(el("div", "q", q.question || q.title || ""));
    const ul = el("ul");
    for (const o of q.options || []) ul.append(el("li", "", typeof o === "string" ? o : o.label || ""));
    d.append(ul);
  }
  if (ev.tool) st.calls.set(ev.tool, d);
  return d;
}

function addEvents(m) {
  const box = $("chat");
  const atBottom = box.scrollHeight - box.scrollTop - box.clientHeight < 40;
  const frag = document.createDocumentFragment();
  for (const ev of m.events) {
    const n = eventNode(ev);
    if (n) frag.append(n);
  }
  const list = $("events");
  if (m.where === "older") {
    // Keep what is on screen where it is as history goes in above it.
    const before = box.scrollHeight;
    list.prepend(frag);
    box.scrollTop += box.scrollHeight - before;
  } else {
    list.append(frag);
    if (m.where === "tail" || atBottom) box.scrollTop = box.scrollHeight;
  }
  if (m.where !== "newer") st.chat.start = m.start;
  $("older").hidden = st.chat.start;
}

function applyState(m) {
  const meta = Object.entries(m.state || {}).filter(([, v]) => v).map(([k, v]) => `${k} ${v}`);
  const a = liveAgent();
  const parts = [];
  if (a) parts.push(a.agent, a.status, a.cwd);
  else parts.push("stored chat");
  if (meta.length) parts.push(meta.join(" · "));
  if (m.waiting && m.waiting.length) parts.push("waiting on a question — answer it in mico");
  if (m.held && m.held.length) parts.push(`${m.held.length} message(s) held`);
  $("chat-meta").textContent = parts.join(" · ");
}

// -------------------------------------------------------------------- prompt

$("older").onclick = () => { if (st.chat && st.chat.path) send({ type: "older", path: st.chat.path }); };

$("prompt").onsubmit = (e) => {
  e.preventDefault();
  const a = liveAgent();
  const text = $("prompt-text").value.trim();
  if (!a || !text) return;
  command({ type: "send", key: a.key, text }, () => { $("prompt-text").value = ""; status(""); });
};

$("prompt-text").addEventListener("keydown", (e) => {
  if (e.key === "Enter" && !e.shiftKey && !e.isComposing) {
    e.preventDefault();
    $("prompt").requestSubmit();
  }
});

setInterval(renderFolders, 60000);  // "3m ago" moves on its own
connect();
