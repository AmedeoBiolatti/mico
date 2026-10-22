#!/usr/bin/env python3
"""The web view against a real, isolated daemon: the page, the checks in front
of the WebSocket, and the state protocol over it. Starts no agent."""
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time

from web_socket import WebSocket, free_port, http, proof

binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else 'build/mico').resolve())


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

        check(WebSocket(port, '').status == 403, 'an empty token is no token')
        check(WebSocket(port, 'f' * 64).status == 403, 'the socket wants the right one')

        # No token offered: a challenge, and nothing else until it is answered.
        quiz = WebSocket(port, None)
        got = quiz.messages(0.5)
        check(quiz.status == 101 and [m['type'] for m in got] == ['challenge'] and
              len(got[0].get('nonce', '')) == 64, 'a socket with no token gets a challenge, and only that')
        quiz.send({'type': 'auth', 'proof': proof('f' * 64, got[0].get('nonce', ''))})
        check(quiz.closed_with(2) == 1008, 'a wrong answer closes it (1008)')
        quiz = WebSocket(port, None)
        quiz.send({'type': 'list'})
        check(quiz.closed_with(2) == 1008, 'so does any other message first')
        quiz = WebSocket(port, None)
        first = quiz.answer(token)
        got = quiz.messages(1.0)
        check([m['type'] for m in got][:1] == ['hello'], 'the right answer lets it in')
        again = WebSocket(port, None)
        check(again.answer(token) != first, 'each challenge has its own nonce')
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
        # Behind `tailscale serve`: one more host, over https, and no other.
        tail = 'box.tail1234.ts.net'
        check(http(port, '/', {'Host': tail})[1] == 421, 'a tailnet name is refused until it is configured')
        (root / 'mico/web-host').write_text(tail + '\n')
        _, status, head, body = http(port, '/', {'Host': tail})
        check(status == 200 and b'<title>mico</title>' in body, 'the page is served to the configured host')
        check(f'wss://{tail}' in head, 'whose CSP lets the page open its wss socket')
        check(http(port, '/', {'Host': 'other.ts.net'})[1] == 421, 'and still to no other')
        check(http(port, '/', own)[1] == 200, 'the local address still works')
        remote = WebSocket(port, token, origin=f'https://{tail}', host=tail)
        check(remote.status == 101, 'a socket from the https page of that host opens')
        check(WebSocket(port, token, origin='https://evil.example', host=tail).status == 403,
              'nor from another site')
        check(WebSocket(port, 'f' * 64, origin=f'https://{tail}', host=tail).status == 403, 'and the token is still wanted')
        (root / 'mico/web-host').write_text('Not A Host!\n')
        check(http(port, '/', {'Host': tail})[1] == 421, 'a malformed host setting opens nothing')
        # The token rides in a header, never in the address a proxy logs.
        check(WebSocket(port, token, in_query=True).status == 403, 'a token in the query string is refused')
        check('Sec-WebSocket-Protocol: mico\r\n' in remote.head and token not in remote.head,
              'the socket names back the plain subprotocol, not the token')

        # More connections than the cap: the extra ones are closed at once.
        crowd = [socket.create_connection(('127.0.0.1', port), timeout=2) for _ in range(40)]
        time.sleep(0.3)
        closed = 0
        for c in crowd:
            c.settimeout(0.05)
            try:
                closed += c.recv(1) == b''
            except (socket.timeout, ConnectionResetError):
                pass
        check(closed >= 8, f'connections past the cap are turned away ({closed} of 40 closed)')
        for c in crowd:
            c.close()
        time.sleep(0.3)

        # :web new-token: the old token stops working, and its browsers go.
        tui = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        tui.connect(str(root / 'mico/default.sock'))
        send = lambda kind, payload: tui.sendall(bytes([kind]) + struct.pack('<I', len(payload)) + payload)
        send(1, struct.pack('<HH', 120, 40))
        time.sleep(0.5)
        send(2, b':web new-token\r')
        for _ in range(100):
            fresh = (root / 'mico/web-token').read_text().strip()
            if fresh != token:
                break
            time.sleep(0.05)
        check(len(fresh) == 64 and fresh != token and (os.stat(root / 'mico/web-token').st_mode & 0o077) == 0,
              ':web new-token writes a new token, for the user alone')
        ws.sock.settimeout(3)
        got = b''
        try:
            while True:
                chunk = ws.sock.recv(65536)
                if not chunk:
                    break
                got += chunk
        except (socket.timeout, ConnectionResetError):
            pass
        check(b'\x88\x02\x03\xf0' in got, 'a browser let in with the old token is closed (1008)')
        check(WebSocket(port, token).status == 403, 'the old token opens nothing')
        check(WebSocket(port, fresh).status == 101, 'the new one does')
        stale = WebSocket(port, None)
        stale.answer(token)
        check(stale.closed_with(2) == 1008, 'nor does an answer keyed with the old token')

        # :web lan: this machine's own address on the network, plain http.
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            probe.connect(('192.0.2.1', 9))  # sends nothing: only picks the route's address
            ip = probe.getsockname()[0]
        except OSError:
            ip = ''
        probe.close()
        octets = [int(x) for x in ip.split('.')] if ip else [0, 0]
        private = octets[0] == 10 or (octets[0] == 172 and 16 <= octets[1] < 32) or octets[:2] == [192, 168]

        def reachable(addr):
            try:
                socket.create_connection((addr, port), timeout=0.3).close()
                return True
            except OSError:
                return False

        def until(want, addr):
            for _ in range(100):
                if reachable(addr) == want:
                    return True
                time.sleep(0.05)
            return False

        if not private:
            print(f'web view: no private address here ({ip or "none"}), the local network checks skipped')
        else:
            lan = f'{ip}:{port}'
            check(not reachable(ip), 'off the local network, the daemon is not reachable on its address')
            send(2, b':web lan\r')
            check(until(True, ip), ':web lan listens on the network address')
            check((root / 'mico/web').read_text().split() == ['on', str(port), 'lan'], 'and is kept in the setting')
            send(2, b'\x1b')  # puts the QR card away
            _, status, head, body = http(port, '/', {'Host': lan}, ip)
            check(status == 200 and b'<title>mico</title>' in body, 'the page is served to its own address')
            check(f'ws://{lan}' in head, 'whose CSP lets the page open its socket there')
            check(http(port, '/', {'Host': f'rebind.example:{port}'}, ip)[1] == 421, 'and still to no other name')
            check(http(port, '/', {'Host': f'192.0.2.1:{port}'}, ip)[1] == 421, 'nor an address not its own')
            check(WebSocket(port, fresh, addr=ip).status == 403, 'a token sent over plain http is refused')
            check(WebSocket(port, None, addr=ip, origin='http://evil.example').status == 403,
                  'a socket from another origin is refused')
            remote = WebSocket(port, None, addr=ip)
            remote.answer(fresh)
            got = remote.messages(1.0)
            check(remote.status == 101 and [m['type'] for m in got][:1] == ['hello'],
                  'the challenge answered lets a browser on the network in')
            send(2, b':web lan off\r')
            check(until(False, ip), ':web lan off takes it off the network')
            check(reachable('127.0.0.1') and WebSocket(port, fresh).status == 101, 'and 127.0.0.1 still serves')
        tui.close()
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
