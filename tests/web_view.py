#!/usr/bin/env python3
"""The web view against a real, isolated daemon: the page, the checks in front
of the WebSocket, and the state protocol over it. Starts no agent."""
import base64
import hashlib
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time

binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else 'build/mico').resolve())


def free_port():
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
    s.close()
    return port


def http(port, path, headers):
    s = socket.create_connection(('127.0.0.1', port), timeout=5)
    req = f'GET {path} HTTP/1.1\r\n' + ''.join(f'{k}: {v}\r\n' for k, v in headers.items()) + '\r\n'
    s.sendall(req.encode())
    data = b''
    while b'\r\n\r\n' not in data:
        chunk = s.recv(65536)
        if not chunk:
            break
        data += chunk
    head, _, body = data.partition(b'\r\n\r\n')
    status = int(head.split(b' ')[1])
    return s, status, head.decode('latin-1'), body


class WebSocket:
    def __init__(self, port, token, origin=None, host=None):
        host = host or f'127.0.0.1:{port}'
        self.key = base64.b64encode(os.urandom(16)).decode()
        self.sock, self.status, self.head, rest = http(port, f'/ws?token={token}', {
            'Host': host, 'Origin': origin or f'http://{host}', 'Upgrade': 'websocket',
            'Connection': 'Upgrade', 'Sec-WebSocket-Key': self.key, 'Sec-WebSocket-Version': '13'})
        self.buf = bytearray(rest)

    def accept_ok(self):
        want = base64.b64encode(hashlib.sha1((self.key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest())
        return f'Sec-WebSocket-Accept: {want.decode()}' in self.head

    def send(self, obj):
        data = json.dumps(obj).encode()
        mask = os.urandom(4)
        n = len(data)
        head = bytes([0x81]) + (bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack('>H', n))
        self.sock.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

    def messages(self, seconds):
        out = []
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            while True:
                if len(self.buf) < 2:
                    break
                n, at = self.buf[1] & 0x7F, 2
                if n == 126:
                    n, at = struct.unpack_from('>H', self.buf, 2)[0], 4
                elif n == 127:
                    n, at = struct.unpack_from('>Q', self.buf, 2)[0], 10
                if len(self.buf) < at + n:
                    break
                if self.buf[0] & 0x0F == 1:
                    out.append(json.loads(self.buf[at:at + n].decode()))
                del self.buf[:at + n]
            self.sock.settimeout(max(0.01, end - time.monotonic()))
            try:
                data = self.sock.recv(1 << 20)
            except socket.timeout:
                break
            if not data:
                break
            self.buf.extend(data)
        return out


with tempfile.TemporaryDirectory(prefix='mico-web-') as directory:
    root = Path(directory)
    port = free_port()
    env = dict(os.environ, HOME=directory, XDG_CONFIG_HOME=directory,
               XDG_STATE_HOME=directory, XDG_RUNTIME_DIR=directory)
    project = root / 'project'
    project.mkdir()
    # The daemon keeps its socket in here too, and wants it closed to others.
    (root / 'mico').mkdir(mode=0o700)
    (root / 'mico/folders').write_text(f'{project}\n')
    (root / 'mico/web').write_text(f'on {port}\n')
    chat = root / '.claude/projects/web/1b1b1b1b-2222-4333-8444-555555555555.jsonl'
    chat.parent.mkdir(parents=True)
    line = lambda text: json.dumps({'type': 'user', 'cwd': str(project),
                                    'message': {'role': 'user', 'content': text}}) + '\n'
    chat.write_text(line('plot the web view'))

    daemon = subprocess.Popen([binary, '--daemon'], cwd=directory, env=env,
                              stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    failures = []

    def check(ok, what):
        if not ok:
            failures.append(what)
            print(f'FAIL web: {what}')

    try:
        for _ in range(300):
            try:
                socket.create_connection(('127.0.0.1', port), timeout=0.1).close()
                break
            except OSError:
                assert daemon.poll() is None, 'daemon exited at startup'
                time.sleep(0.02)
        token = (root / 'mico/web-token').read_text().strip()
        check(len(token) == 64 and (os.stat(root / 'mico/web-token').st_mode & 0o077) == 0,
              'the token is 32 random bytes, readable by the user alone')

        own = {'Host': f'127.0.0.1:{port}'}
        _, status, head, body = http(port, '/', own)
        check(status == 200 and b'<title>mico</title>' in body, 'the page is served')
        check("Content-Security-Policy: default-src 'self'" in head, 'the page is served with a CSP')
        check(http(port, '/app.js', own)[1] == 200, 'its script is served')
        check(http(port, '/missing', own)[1] == 404, 'anything else is not found')
        check(http(port, '/', {'Host': f'rebind.example:{port}'})[1] == 421,
              'a request naming another host is refused (DNS rebinding)')

        check(WebSocket(port, '').status == 403, 'the socket wants the token')
        check(WebSocket(port, 'f' * 64).status == 403, 'and the right one')
        check(WebSocket(port, token, origin='http://evil.example').status == 403,
              'a socket from another origin is refused, token or not')

        ws = WebSocket(port, token)
        check(ws.status == 101 and ws.accept_ok(), 'the right token opens the socket')
        got = ws.messages(1.0)
        kinds = [m['type'] for m in got]
        check(kinds[:1] == ['hello'], 'hello comes first')
        folders = next((m for m in got if m['type'] == 'folders'), {'folders': []})
        chats = [c for f in folders['folders'] for c in f['chats']]
        check(any(c['path'] == str(chat) and c['title'] == 'plot the web view' for c in chats),
              'the listing has the fixture chat')

        ws.send({'type': 'open', 'path': str(chat)})
        got = ws.messages(1.0)
        tail = next((m for m in got if m['type'] == 'chat'), None)
        check(tail is not None and tail['where'] == 'tail' and
              any(e.get('text') == 'plot the web view' for e in tail['events']), 'opening it sends its tail')

        with chat.open('a') as f:
            f.write(line('and live updates'))
        got = ws.messages(2.5)
        check(any(m['type'] == 'chat' and m['where'] == 'newer' and
                  any(e.get('text') == 'and live updates' for e in m['events']) for m in got),
              'a line written to the transcript reaches the browser')
        if not failures:
            print('web view: page, checks and protocol passed')
    finally:
        daemon.terminate()
        try:
            daemon.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            daemon.kill()
            daemon.communicate()
    sys.exit(1 if failures else 0)
