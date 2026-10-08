#!/usr/bin/env python3
"""The web view's page, in a real browser (headless Chrome), against a real daemon.

Markdown becomes structure, tool calls are grouped, a question is a card, a
phone gets one screen at a time, and what a transcript says is never markup:
an <img onerror>, a javascript: link and a <script> in a message stay text.
Skipped where there is no Chrome or Chromium.
Run: python3 tests/web_ui.py build/mico
"""
import base64
import json
import os
import struct
import urllib.request
from html.parser import HTMLParser
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time

binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else 'build/mico').resolve())
chrome = next((p for p in (shutil.which(n) for n in ('google-chrome', 'google-chrome-stable', 'chromium',
                                                      'chromium-browser', 'chrome')) if p), None)
if not chrome:
    print('web ui: skipped, no Chrome or Chromium here')
    sys.exit(0)

SESSION = '4d3c2b1a-9f8e-4d7c-8b6a-1a2b3c4d5e6f'
MARKDOWN = """## What changed

I rewrote the **notification path** with *care*, in `notify_seq`, and ~~dropped~~ the old one.

- kitty gets OSC 99
- Ghostty gets OSC 777
  - and WezTerm too
- everything else rings the bell

1. Read the version
2. Pick the escape

> A terminal that has focus gets nothing.

| terminal | escape |
|---|---|
| kitty | OSC 99 |
| iTerm2 | OSC 9 |

```cpp
std::string notify_seq() { return "\\x07"; }
```

See [the docs](https://example.com/docs) or https://example.com/bare.

Hostile: <img src=x onerror="document.title='pwned'"> and <script>document.title='pwned'</script>
and [click](javascript:document.title='pwned') and [data](data:text/html,<b>x</b>).
"""


class Node:
    def __init__(self, tag, attrs, parent):
        self.tag, self.attrs, self.parent, self.kids, self.text = tag, dict(attrs), parent, [], ''

    def classes(self):
        return set(self.attrs.get('class', '').split())

    def walk(self):
        yield self
        for k in self.kids:
            yield from k.walk()

    def all_text(self):
        return self.text + ''.join(k.all_text() for k in self.kids)


class Tree(HTMLParser):
    VOID = {'meta', 'link', 'br', 'hr', 'img', 'input', 'col'}

    def __init__(self):
        super().__init__()
        self.root = Node('#root', [], None)
        self.cur = self.root

    def handle_starttag(self, tag, attrs):
        n = Node(tag, attrs, self.cur)
        self.cur.kids.append(n)
        if tag not in self.VOID:
            self.cur = n

    def handle_endtag(self, tag):
        n = self.cur
        while n and n.tag != tag:
            n = n.parent
        if n and n.parent:
            self.cur = n.parent

    def handle_data(self, data):
        self.cur.text += data


def parse(dom):
    t = Tree()
    t.feed(dom)
    return t.root


def find(root, tag=None, cls=None):
    return [n for n in root.walk() if (tag is None or n.tag == tag) and (cls is None or cls in n.classes())]


def free_port():
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Page:
    """One page of a headless Chrome, spoken to over the DevTools protocol in real time: the
    page's socket to the daemon is a network round trip, which a virtual clock outruns."""

    def __init__(self, port, url):
        req = urllib.request.Request(f'http://127.0.0.1:{port}/json/new?about:blank', method='PUT')
        target = json.load(urllib.request.urlopen(req, timeout=10))
        host, _, path = target['webSocketDebuggerUrl'][len('ws://'):].partition('/')
        self.sock = socket.create_connection((host.split(':')[0], int(host.split(':')[1])), timeout=30)
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall((f'GET /{path} HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
                           f'Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n').encode())
        self.buf = b''
        while b'\r\n\r\n' not in self.buf:
            self.buf += self.sock.recv(65536)
        self.buf = self.buf.split(b'\r\n\r\n', 1)[1]
        self.next_id = 0
        self.call('Page.enable')

    def _frame(self):
        def need(n):
            while len(self.buf) < n:
                chunk = self.sock.recv(1 << 20)
                if not chunk:
                    raise EOFError('the browser went away')
                self.buf += chunk
        message = b''
        while True:
            need(2)
            fin, op, n, at = self.buf[0] & 0x80, self.buf[0] & 0x0F, self.buf[1] & 0x7F, 2
            if n == 126:
                need(4)
                n, at = struct.unpack_from('>H', self.buf, 2)[0], 4
            elif n == 127:
                need(10)
                n, at = struct.unpack_from('>Q', self.buf, 2)[0], 10
            need(at + n)
            payload, self.buf = self.buf[at:at + n], self.buf[at + n:]
            if op in (0, 1, 2):
                message += payload
                if fin:
                    return message

    def call(self, method, **params):
        self.next_id += 1
        data = json.dumps({'id': self.next_id, 'method': method, 'params': params}).encode()
        mask, n = os.urandom(4), len(data)
        head = bytes([0x81]) + (bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack('>H', n)
                                if n < 65536 else bytes([0x80 | 127]) + struct.pack('>Q', n))
        self.sock.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))
        while True:
            m = json.loads(self._frame())
            if m.get('id') == self.next_id:
                return m.get('result', {})

    def eval(self, expression):
        r = self.call('Runtime.evaluate', expression=expression, returnByValue=True, awaitPromise=True)
        return r.get('result', {}).get('value')

    def open(self, url, width, height, touch):
        self.call('Emulation.setDeviceMetricsOverride', width=width, height=height, deviceScaleFactor=1, mobile=touch)
        self.call('Emulation.setEmulatedMedia', features=[{'name': 'pointer', 'value': 'coarse' if touch else 'fine'}])
        self.call('Page.navigate', url=url)

    def wait(self, expression, seconds=15):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            if self.eval(expression):
                return True
            time.sleep(0.1)
        return False

    def dom(self):
        return self.eval('document.documentElement.outerHTML')


failures = []


def check(ok, what):
    if not ok:
        failures.append(what)
        print(f'FAIL web ui: {what}')


# Chrome may still be writing its profile as the folder goes.
with tempfile.TemporaryDirectory(prefix='mico-webui-', ignore_cleanup_errors=True) as directory:
    root = Path(directory)
    port = free_port()
    project = root / 'project'
    project.mkdir()
    (root / 'mico').mkdir(mode=0o700)
    (root / 'mico/folders').write_text(f'{project}\n')
    (root / 'mico/web').write_text(f'on {port}\n')
    cwd = str(project)
    ts = '2026-10-03T10:00:00.000Z'
    rec = lambda kind, content, **kw: {'type': kind, 'cwd': cwd, 'sessionId': SESSION, 'timestamp': ts,
                                       'message': content, **kw}
    chat = root / '.claude/projects/-project' / f'{SESSION}.jsonl'
    chat.parent.mkdir(parents=True)
    records = [
        {'type': 'ai-title', 'aiTitle': 'Markdown fixture chat', 'sessionId': SESSION},
        rec('user', {'role': 'user', 'content': 'Explain the notification path <b>please</b>'}),
        rec('assistant', {'role': 'assistant', 'content': [
            {'type': 'tool_use', 'id': 't1', 'name': 'Bash', 'input': {'command': 'grep -rn notify src'}},
            {'type': 'tool_use', 'id': 't2', 'name': 'Edit',
             'input': {'file_path': cwd + '/a.cpp', 'old_string': 'a', 'new_string': 'b'}}]}),
        rec('user', {'role': 'user', 'content': [{'type': 'tool_result', 'tool_use_id': 't1', 'content': 'src/a.cpp:1: notify'}]}),
        rec('user', {'role': 'user', 'content': [{'type': 'tool_result', 'tool_use_id': 't2', 'is_error': True,
                                                  'content': 'String to replace not found in file.'}]}),
        rec('assistant', {'role': 'assistant', 'content': [{'type': 'text', 'text': MARKDOWN}]}),
        rec('assistant', {'role': 'assistant', 'content': [
            {'type': 'tool_use', 'id': 't3', 'name': 'AskUserQuestion', 'input': {'questions': [
                {'question': 'Where should it live?', 'header': 'Placement', 'multiSelect': False,
                 'options': [{'label': 'Settings tab', 'description': 'next to Away'},
                             {'label': 'A file', 'description': 'by hand'}]}]}}]}),
    ]
    chat.write_text(''.join(json.dumps(r, separators=(',', ':')) + '\n' for r in records))

    # A sub-project, and a chat in it with a picture, a chart, a picture file and an equation.
    inner = project / 'inner'
    inner.mkdir()
    (root / 'mico/subprojects').write_text(f'sub\t{project}\tinner\t{inner}\n')
    SESSION2 = '5e4d3c2b-0a9f-4e8d-9c7b-2b3c4d5e6f70'
    import zlib

    def png(w, h):
        def chunk(t, d):
            return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
        rows = b''.join(b'\x00' + bytes([200, 60, 60, 255]) * w for _ in range(h))
        return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))
    (inner / 'pic.png').write_bytes(png(8, 8))
    shot = base64.b64encode(png(6, 6)).decode()
    chart = json.dumps({'type': 'line', 'title': 'loss', 'x': [0, 1, 2], 'series': [{'name': 'a', 'y': [3, 2, 1]},
                                                                                       {'name': 'b', 'y': [2, 2, 2]}]})
    cwd2 = str(inner)
    rec2 = lambda kind, content, **kw: {'type': kind, 'cwd': cwd2, 'sessionId': SESSION2, 'timestamp': ts,
                                        'message': content, **kw}
    chat2 = root / '.claude/projects/-project-inner' / f'{SESSION2}.jsonl'
    chat2.parent.mkdir(parents=True)
    pics = [
        {'type': 'ai-title', 'aiTitle': 'Pictures fixture', 'sessionId': SESSION2},
        rec2('user', {'role': 'user', 'content': 'show me'}),
        rec2('assistant', {'role': 'assistant', 'content': [{'type': 'thinking', 'thinking': 'let me think'}]}),
        rec2('assistant', {'role': 'assistant', 'content': [
            {'type': 'tool_use', 'id': 'u1', 'name': 'Bash', 'input': {'command': 'ls'}}]}),
        rec2('user', {'role': 'user', 'content': [{'type': 'tool_result', 'tool_use_id': 'u1', 'content': 'out'}]}),
        rec2('user', {'role': 'user', 'content': [{'type': 'image', 'source': {'type': 'base64', 'media_type': 'image/png',
                                                                                'data': shot}}]}),
        rec2('assistant', {'role': 'assistant', 'content': [{'type': 'text', 'text':
            f'A file:\n\n![the file]({inner}/pic.png)\n\nA chart:\n\n```chart\n{chart}\n```\n\n$$x^2 + y^2$$\n\nDone.'}]}),
    ]
    chat2.write_text(''.join(json.dumps(r, separators=(',', ':')) + '\n' for r in pics))

    env = dict(os.environ, HOME=directory, XDG_CONFIG_HOME=directory, XDG_STATE_HOME=directory,
               XDG_RUNTIME_DIR=directory)
    env.pop('DISPLAY', None)
    daemon = subprocess.Popen([binary, '--daemon'], cwd=directory, env=env, stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL)

    debug_port = free_port()
    profile = tempfile.mkdtemp(prefix='chrome-', dir=directory)  # never the user's own browser
    browser = subprocess.Popen([chrome, '--headless=new', '--no-sandbox', '--disable-gpu', '--disable-extensions',
                                f'--user-data-dir={profile}', f'--remote-debugging-port={debug_port}', 'about:blank'],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    for _ in range(300):  # Chrome takes a moment to open its debugging port
        try:
            socket.create_connection(('127.0.0.1', debug_port), timeout=0.1).close()
            break
        except OSError:
            assert browser.poll() is None, 'the browser exited at startup'
            time.sleep(0.05)

    def dom_of(url, width, height, touch=False, ready="document.querySelectorAll('.row').length > 0"):
        page = Page(debug_port, url)
        page.open(url, width, height, touch)
        check(page.wait(ready), f'the page was ready at {width}px')
        dom = page.dom()
        page.call('Page.close')
        return dom

    try:
        for _ in range(300):
            try:
                socket.create_connection(('127.0.0.1', port), timeout=0.1).close()
                break
            except OSError:
                assert daemon.poll() is None, 'daemon exited at startup'
                time.sleep(0.02)
        token = (root / 'mico/web-token').read_text().strip()
        base = f'http://127.0.0.1:{port}/#token={token}'

        # A desk: the list beside the chat.
        dom = dom_of(f'{base}&chat={chat}', 1200, 900, ready="document.querySelectorAll('#events .ev').length >= 4")
        tree = parse(dom)
        check('<title>Markdown fixture chat' in dom, 'the tab is titled by the chat')
        check(len(find(tree, 'h2')) >= 3 and any(h.all_text() == 'What changed' for h in find(tree, 'h2')),
              'a heading is a heading')
        md = next(iter(find(tree, 'div', 'assistant')), None)
        check(md is not None, 'the reply is shown')
        uls = [u for u in find(tree, 'ul') if 'tool' not in u.parent.classes()]
        check(any(len([k for k in u.kids if k.tag == 'li']) == 3 for u in uls), 'a bullet list has its three items')
        check(any(find(li, 'ul') for u in uls for li in u.kids if li.tag == 'li'), 'a nested list nests')
        check(any(len(find(o, 'li')) == 2 for o in find(tree, 'ol')), 'a numbered list is an ol')
        check(len(find(tree, 'blockquote')) == 1, 'a quote is a blockquote')
        tables = find(tree, 'table')
        check(len(tables) == 1 and len(find(tables[0], 'th')) == 2 and len(find(tables[0], 'td')) == 4,
              'a table has its header and cells')
        blocks = find(tree, 'pre', 'block')
        check(any('notify_seq' in b.all_text() for b in blocks), 'a fenced block is a code block')
        check(any(c.all_text().strip() == 'cpp' for c in find(tree, 'div', 'code-bar') for c in c.kids if c.tag == 'span'),
              'and says its language')
        check(find(tree, 'strong') and find(tree, 'em') and find(tree, 'del'), 'bold, italic and strike')
        check(any(c.all_text() == 'notify_seq' for c in find(tree, 'code')), 'inline code')
        links = [a for a in find(tree, 'a') if a.attrs.get('href', '').startswith('https://example.com')]
        check(len(links) == 2 and all(a.attrs.get('target') == '_blank' and 'noopener' in a.attrs.get('rel', '')
                                      for a in links), 'links, and a bare URL, open in a new tab without the opener')

        # What a transcript says is never markup.
        check('<img' not in dom and not find(tree, 'img'), 'an <img onerror> in a message is text, not an image')
        check(len(find(tree, 'script')) == 1, 'a <script> in a message is text, not a script')
        check('&lt;img src=x' in dom and '&lt;script&gt;' in dom, 'and is shown as the text it was')
        check('pwned' not in find(tree, 'title')[0].all_text(), 'nothing it said ran')
        check(not [a for a in find(tree, 'a') if a.attrs.get('href', '').split(':')[0] in ('javascript', 'data')],
              'javascript: and data: links are not links')
        check(any('<b>please</b>' in b.all_text() for b in find(tree, 'div', 'bubble')),
              'your own message with markup in it is text')

        groups = find(tree, 'details', 'tools')
        check(len(groups) == 1 and '2 tool calls' in groups[0].kids[0].all_text() and
              '1 failed' in groups[0].kids[0].all_text(), 'tool calls are one group, with how many failed')
        check(len(find(tree, 'details', 'failed')) == 1 and
              any('String to replace' in o.all_text() for o in find(tree, 'pre', 'err')),
              'a failed call shows its error')
        qs = find(tree, 'div', 'question')
        check(len(qs) == 1 and len(find(qs[0], 'li')) == 2 and 'Placement' in qs[0].all_text(),
              'a question is a card with its options')
        check(len(find(tree, 'div', 'folder')) == 1 and any('current' in r.classes() for r in find(tree, 'button', 'row')),
              'the folder lists its chat, this one marked')
        subs = find(tree, 'div', 'subfolder')
        check(len(subs) == 1 and 'inner' in subs[0].all_text() and 'Pictures fixture' in subs[0].all_text(),
              'a sub-project is listed under its folder, with its chat')
        check(find(tree, 'div', 'app') == [] and any(n.attrs.get('id') == 'app' for n in tree.walk()),
              'the page is its app')

        # A phone: one screen at a time. With a chat in the address, the chat; without, the list.
        phone = parse(dom_of(f'{base}&chat={chat}', 390, 800, True, "document.querySelectorAll('#events .ev').length >= 4"))
        app = next(n for n in phone.walk() if n.attrs.get('id') == 'app')
        check(app.attrs.get('data-screen') == 'chat', 'a phone opened on a chat shows the chat')
        phone = parse(dom_of(base, 390, 800, True))
        app = next(n for n in phone.walk() if n.attrs.get('id') == 'app')
        check(app.attrs.get('data-screen') == 'list' and len(find(phone, 'button', 'row')) == 2,
              'a phone opened on nothing shows the list')

        # Pictures, a chart, an equation, and how much of the work shows.
        page = Page(debug_port, '')
        page.open(f'{base}&chat={chat2}', 1200, 900, False)
        check(page.wait("document.querySelectorAll('#events figure.pic img').length >= 2"),
              'a transcript image and a picture file are drawn as images')
        check(page.eval("[...document.querySelectorAll('#events img')].every(i => i.src.startsWith('data:image/png;base64,') && i.naturalWidth > 0)"),
              'and they decode')
        check(page.eval("document.querySelectorAll('#events svg.chart-svg polyline').length") == 2, 'a chart is drawn, a line per series')
        check(page.eval("!!document.querySelector('#events .chart-legend')"), 'with its legend')
        check(page.eval("document.querySelector('#events .math')?.textContent") == 'x^2 + y^2', 'an equation keeps its LaTeX')
        check(page.eval("document.querySelector('#events details.thinking')?.open") is False, 'normal view: thinking is folded')
        page.eval("document.querySelector('#views button[data-view=full]').click()")
        check(page.eval("document.querySelector('#events details.thinking').open && document.querySelector('#events details.tool').open"),
              'the full view opens thinking and every step')
        page.eval("document.querySelector('#views button[data-view=minimal]').click()")
        check(page.eval("!document.querySelector('#events details.tools').open && getComputedStyle(document.querySelector('#events .ev.thinking')).display === 'none'"),
              'the minimal view folds the steps and hides the thinking')
        check(page.eval("document.querySelector('#views button[aria-pressed=true]').dataset.view") == 'minimal', 'the choice is marked')
        check(page.eval("getComputedStyle(document.body).backgroundColor") in ('rgb(250, 249, 247)', 'rgb(20, 20, 22)'),
              'a theme of the system\'s own to begin with')
        page.eval("(() => { const s = document.getElementById('theme'); s.value = 'dracula'; s.dispatchEvent(new Event('change')); })()")
        check(page.eval("getComputedStyle(document.body).backgroundColor") == 'rgb(40, 42, 54)', 'Dracula is chosen from the list')
        check(page.eval("document.documentElement.dataset.theme") == 'dracula', 'and kept on the page')
        page.eval("(() => { const s = document.getElementById('theme'); s.value = 'light'; s.dispatchEvent(new Event('change')); })()")
        check(page.eval("getComputedStyle(document.body).backgroundColor") == 'rgb(250, 249, 247)', 'Light overrides a dark system')
        page.eval("(() => { const s = document.getElementById('theme'); s.value = 'auto'; s.dispatchEvent(new Event('change')); })()")
        check(page.eval("document.documentElement.dataset.theme") is None, 'Auto goes back to the system')
        page.call('Page.close')

        if not failures:
            print('web ui: markdown, tool groups, questions, phone screens and inert transcripts passed')
    finally:
        browser.terminate()
        try:
            browser.wait(timeout=10)
        except subprocess.TimeoutExpired:
            browser.kill()
        daemon.terminate()
        try:
            daemon.wait(timeout=5)
        except subprocess.TimeoutExpired:
            daemon.kill()
            daemon.wait()
    sys.exit(1 if failures else 0)
