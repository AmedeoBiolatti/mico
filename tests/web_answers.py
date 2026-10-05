#!/usr/bin/env python3
"""Answering from the web: a permission dialog, a question card, an interrupt.

Against an isolated daemon and a stand-in for claude that draws Claude's
permission dialog and an AskUserQuestion menu, and records every key it gets:
the protocol's `permission` field shows the dialog, `answer_permission` and
`answer` walk its cursor to the choice and confirm, `interrupt` sends Esc, and
what is not asked for is refused. A browser is not involved.
Run: python3 tests/web_answers.py build/mico
"""
import base64
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time

binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else 'build/mico').resolve())
SESSION = '5e4d3c2b-1a09-4f8e-9d7c-6b5a4f3e2d1c'
TOOL = 'toolu_01Question'

FAKE_CLAUDE = r'''#!/usr/bin/env python3
import json, os, select, sys, time, tty
if '--resume' not in sys.argv:
    sys.exit(0)
tty.setraw(0)
root = os.environ['FIXTURE_ROOT']
mode = os.environ['FIXTURE_MODE']
transcript = os.environ['FIXTURE_TRANSCRIPT']
def log(key):
    with open(os.path.join(root, 'keys'), 'ab') as f:
        f.write(repr(key).encode() + b'\n')
cursor = 0

def permission():
    options = ['Yes', 'Yes, and always allow access to /tmp/x from this project', 'No']
    def draw():
        out = '\x1b[2J\x1b[H' + '─' * 60 + '\r\n Bash command\r\n   touch fixture.txt\r\n\r\n Do you want to proceed?\r\n'
        for i, label in enumerate(options):
            out += f' {"❯" if i == cursor else " "} {i + 1}. {label}\r\n'
        out += '\r\n Esc to cancel · Tab to amend\r\n'
        os.write(1, out.encode())
    draw()
    return draw, len(options)

def question():
    options = ['Settings tab', 'A config file']
    def draw():
        out = '\x1b[2J\x1b[H' + 'Where should the setting live?\r\n'
        for i, label in enumerate(options):
            out += f' {"❯" if i == cursor else " "} {i + 1}. {label}\r\n'
        os.write(1, out.encode())
    with open(transcript, 'a') as f:
        f.write(json.dumps({'type': 'assistant', 'timestamp': '2026-10-03T10:00:00.000Z', 'message': {'content': [
            {'type': 'tool_use', 'id': os.environ['FIXTURE_TOOL'], 'name': 'AskUserQuestion', 'input': {'questions': [
                {'question': 'Where should the setting live?', 'header': 'Placement', 'multiSelect': False,
                 'options': [{'label': o, 'description': ''} for o in options]}]}}]}}, separators=(',', ':')) + '\n')
    draw()
    return draw, len(options)

draw, n = permission() if mode == 'permission' else question() if mode == 'question' else (lambda: os.write(1, b'> '), 0)
open(os.path.join(root, 'ready'), 'w').close()
while True:
    key = os.read(0, 1024)
    log(key)
    if key == b'\x1b[B' and n:
        cursor = min(cursor + 1, n - 1)
    elif key == b'\x1b[A' and n:
        cursor = max(cursor - 1, 0)
    elif key == b'\r' and n:
        with open(os.path.join(root, 'result.json'), 'w') as f:
            json.dump({'choice': cursor}, f)
        if mode == 'question':
            with open(transcript, 'a') as f:
                f.write(json.dumps({'type': 'user', 'timestamp': '2026-10-03T10:00:01.000Z', 'message': {'content': [
                    {'type': 'tool_result', 'tool_use_id': os.environ['FIXTURE_TOOL'], 'content': f'answered {cursor}'}]}},
                    separators=(',', ':')) + '\n')
        os.write(1, b'\x1b[2J\x1b[HANSWERED\r\n')
        n = 0
        continue
    time.sleep(0.06)
    if n:
        draw()
'''


class WebSocket:
    def __init__(self, port, token):
        host = f'127.0.0.1:{port}'
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=5)
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall((f'GET /ws?token={token} HTTP/1.1\r\nHost: {host}\r\nOrigin: http://{host}\r\n'
                           f'Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n'
                           f'Sec-WebSocket-Version: 13\r\n\r\n').encode())
        self.buf = bytearray()
        while b'\r\n\r\n' not in self.buf:
            self.buf.extend(self.sock.recv(65536))
        head, _, rest = bytes(self.buf).partition(b'\r\n\r\n')
        assert b' 101 ' in head.split(b'\r\n')[0], head
        self.buf = bytearray(rest)
        self.seen = []

    def send(self, obj):
        import struct
        data = json.dumps(obj).encode()
        mask, n = os.urandom(4), len(data)
        head = bytes([0x81]) + (bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack('>H', n))
        self.sock.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

    def pump(self, seconds):
        import struct
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            while len(self.buf) >= 2:
                n, at = self.buf[1] & 0x7F, 2
                if n == 126:
                    if len(self.buf) < 4:
                        break
                    n, at = struct.unpack_from('>H', self.buf, 2)[0], 4
                elif n == 127:
                    if len(self.buf) < 10:
                        break
                    n, at = struct.unpack_from('>Q', self.buf, 2)[0], 10
                if len(self.buf) < at + n:
                    break
                if self.buf[0] & 0x0F == 1:
                    self.seen.append(json.loads(self.buf[at:at + n].decode()))
                del self.buf[:at + n]
            self.sock.settimeout(max(0.01, end - time.monotonic()))
            try:
                data = self.sock.recv(1 << 20)
            except socket.timeout:
                break
            if not data:
                break
            self.buf.extend(data)

    def wait(self, what, pred, seconds=15):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            self.pump(0.2)
            for m in reversed(self.seen):
                r = pred(m)
                if r:
                    return m
        raise AssertionError(f'timed out waiting for {what}; last: {self.seen[-3:]}')

    def ask(self, **msg):
        rid = f'r{len(self.seen)}{time.monotonic_ns()}'
        self.send({**msg, 'rid': rid})
        return self.wait('the result', lambda m: m.get('type') == 'result' and m.get('rid') == rid)


def run(mode):
    directory = tempfile.mkdtemp(prefix=f'mico-answers-{mode}-')
    if True:
        root = Path(directory)
        work = root / 'work'
        work.mkdir()
        (root / 'bin').mkdir()
        fake = root / 'bin/claude'
        fake.write_text(FAKE_CLAUDE)
        fake.chmod(0o755)
        transcript = root / '.claude/projects/-work' / f'{SESSION}.jsonl'
        transcript.parent.mkdir(parents=True)
        transcript.write_text(''.join(json.dumps(r, separators=(',', ':')) + '\n' for r in [
            {'type': 'user', 'cwd': str(work), 'sessionId': SESSION, 'timestamp': '2026-10-03T09:00:00.000Z',
             'message': {'role': 'user', 'content': 'Do it'}},
            {'type': 'ai-title', 'aiTitle': 'Answers fixture', 'sessionId': SESSION}]))
        port = None
        with socket.socket() as s:
            s.bind(('127.0.0.1', 0))
            port = s.getsockname()[1]
        (root / 'mico').mkdir(mode=0o700)
        (root / 'mico/folders').write_text(f'{work}\n')
        (root / 'mico/web').write_text(f'on {port}\n')
        env = dict(os.environ, HOME=directory, XDG_CONFIG_HOME=directory, XDG_STATE_HOME=directory,
                   XDG_RUNTIME_DIR=directory, PATH=f'{root / "bin"}:{os.environ.get("PATH", "")}',
                   FIXTURE_ROOT=str(root), FIXTURE_MODE=mode, FIXTURE_TRANSCRIPT=str(transcript), FIXTURE_TOOL=TOOL)
        env.pop('DISPLAY', None)
        daemon = subprocess.Popen([binary, '--daemon'], cwd=work, env=env, stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL)
        try:
            for _ in range(300):
                try:
                    socket.create_connection(('127.0.0.1', port), timeout=0.1).close()
                    break
                except OSError:
                    assert daemon.poll() is None, 'daemon exited at startup'
                    time.sleep(0.02)
            token = (root / 'mico/web-token').read_text().strip()
            ws = WebSocket(port, token)
            ws.wait('the folders', lambda m: m.get('type') == 'folders')
            r = ws.ask(type='resume', agent='claude', id=SESSION, fork=False)
            assert r['ok'], r
            key = r['key']
            (root / 'ready').exists() or ws.wait('the agent to draw', lambda m: (root / 'ready').exists())
            return root, ws, key, str(transcript), daemon
        except Exception:
            daemon.kill()
            raise


def keys_of(root):
    path = root / 'keys'
    return path.read_text().splitlines() if path.exists() else []


failures = []


def check(ok, what):
    if not ok:
        failures.append(what)
        print(f'FAIL web answers: {what}')


# ---- a permission dialog
root, ws, key, chat, daemon = run('permission')
try:
    m = ws.wait('the dialog in agents', lambda m: m.get('type') == 'agents' and
                any(a.get('permission') for a in m['agents']))
    agent = next(a for a in m['agents'] if a.get('permission'))
    p = agent['permission']
    check(agent['status'] == 'waiting' and p['question'] == 'Do you want to proceed?' and len(p['options']) == 3 and
          p['options'][2] == 'No' and p['cursor'] == 0 and p['disabled'] == [False] * 3 and p['amend'] is True,
          'the agent lists its dialog: question, options, cursor, amend')
    check(ws.ask(type='answer_permission', key=key, index=7)['ok'] is False, 'a choice that is not there is refused')
    check(ws.ask(type='answer_permission', key=key + 99, index=0)['ok'] is False, 'so is an agent that is not there')
    r = ws.ask(type='answer_permission', key=key, index=2, note='')
    check(r['ok'], 'answering is accepted')
    end = time.monotonic() + 15
    while not (root / 'result.json').exists() and time.monotonic() < end:
        ws.pump(0.2)
    check((root / 'result.json').exists() and json.loads((root / 'result.json').read_text()) == {'choice': 2},
          'the cursor was walked to "No" and confirmed: ' + repr(keys_of(root)))
    check(keys_of(root) == [repr(b'\x1b[B'), repr(b'\x1b[B'), repr(b'\r')], 'by the keys the terminal panel sends')
    m = ws.wait('the dialog to go', lambda m: m.get('type') == 'agents' and not any(a.get('permission') for a in m['agents']))
    check(ws.ask(type='answer_permission', key=key, index=0)['ok'] is False, 'once it is answered there is nothing to answer')
    r = ws.ask(type='interrupt', key=key)
    ws.pump(0.8)
    check(r['ok'] and keys_of(root)[-1] == repr(b'\x1b'), 'an interrupt is Esc to the agent')
finally:
    daemon.kill()
    daemon.wait()
    shutil.rmtree(root, ignore_errors=True)

# ---- a question card
root, ws, key, chat, daemon = run('question')
try:
    ws.send({'type': 'open', 'path': chat})
    m = ws.wait('the question waiting', lambda m: m.get('type') == 'chat_state' and m.get('waiting'))
    tool = m['waiting'][0]
    check(ws.ask(type='answer', key=key, tool=tool, chosen=[])['ok'] is False, 'no choice is refused')
    check(ws.ask(type='answer', key=key, tool=tool, chosen=[[0, 1]])['ok'] is False, 'two choices of one are refused')
    check(ws.ask(type='answer', key=key, tool='00000000000000ff', chosen=[[0]])['ok'] is False,
          'a question that is not waiting is refused')
    check(not (root / 'result.json').exists(), 'and nothing reached the agent')
    r = ws.ask(type='answer', key=key, tool=tool, chosen=[[1]])
    check(r['ok'], 'a good answer is accepted')
    end = time.monotonic() + 15
    while not (root / 'result.json').exists() and time.monotonic() < end:
        ws.pump(0.2)
    check((root / 'result.json').exists() and json.loads((root / 'result.json').read_text()) == {'choice': 1},
          'the second option was chosen: ' + repr(keys_of(root)))
    ws.wait('the question to be answered', lambda m: m.get('type') == 'chat_state' and not m.get('waiting'))
    check(ws.ask(type='answer', key=key, tool=tool, chosen=[[0]])['ok'] is False, 'an answered question cannot be answered again')
finally:
    daemon.kill()
    daemon.wait()
    shutil.rmtree(root, ignore_errors=True)

if not failures:
    print('web answers: a permission dialog, a question card and an interrupt passed')
sys.exit(1 if failures else 0)
