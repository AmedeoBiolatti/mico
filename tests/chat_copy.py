#!/usr/bin/env python3
"""Copying a chat by dragging over it gives the text, not the view: no gutter
bars, no quote bars, no scrollbar, no pane margin, and nested lines keep the
indent they have beyond it.

Against an isolated daemon and a chat long enough to need a scrollbar, a drag
from the chat's top-left to past its bottom-right; what it copied is read off
the OSC 52 sequence in the frames that follow.
Run: python3 tests/chat_copy.py build/mico
"""
import base64
import json
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time

binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else 'build/mico').resolve())
SESSION = '3c3d4e5f-6a7b-4c8d-9e0f-1a2b3c4d5e6f'
W, H = 120, 40
BARS = set('▌▎▏▐│┃╭╮╰╯')

failures = []


def check(ok, what):
    if not ok:
        failures.append(what)
        print(f'FAIL copy: {what}')


with tempfile.TemporaryDirectory(prefix='mico-copy-') as directory:
    root = Path(directory)
    work = root / 'work'
    work.mkdir()
    steps = '\n'.join(f'{i}. step number {i}' for i in range(1, 41))
    answer = (f'Here is the plan:\n\n{steps}\n\n> A quoted line\n\n'
              '- outer item\n  - nested item\n\n> [!NOTE]\n> Mind the gap\n\nThe end.')
    transcript = root / '.claude/projects/-work' / f'{SESSION}.jsonl'
    transcript.parent.mkdir(parents=True)
    transcript.write_text('\n'.join(json.dumps(r) for r in [
        {'type': 'user', 'cwd': str(work), 'sessionId': SESSION, 'timestamp': '2026-10-01T10:00:00.000Z',
         'message': {'role': 'user', 'content': 'Make the chat list easier to scan'}},
        {'type': 'assistant', 'cwd': str(work), 'sessionId': SESSION, 'timestamp': '2026-10-01T10:00:01.000Z',
         'message': {'role': 'assistant', 'content': [{'type': 'text', 'text': answer}]}},
        {'type': 'user', 'cwd': str(work), 'sessionId': SESSION, 'timestamp': '2026-10-01T10:00:02.000Z',
         'message': {'role': 'user', 'content': 'Thanks, ship it'}},
        {'type': 'ai-title', 'aiTitle': 'Copy fixture', 'sessionId': SESSION}]) + '\n')
    config = root / 'config/mico'
    config.mkdir(parents=True)
    (config / 'folders').write_text(f'{work}\n')
    (config / 'view').write_text(f'tab Sessions\nfolder {work}\n')
    env = dict(os.environ, HOME=directory, XDG_CONFIG_HOME=str(root / 'config'), XDG_STATE_HOME=str(root / 'state'),
               XDG_RUNTIME_DIR=directory)
    for k in ('TMUX', 'DISPLAY', 'WAYLAND_DISPLAY'):
        env.pop(k, None)
    daemon = subprocess.Popen([binary, '--daemon'], cwd=work, env=env,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(300):
            if (root / 'mico/default.sock').exists():
                break
            time.sleep(0.01)
        sock = socket.socket(socket.AF_UNIX)
        sock.connect(str(root / 'mico/default.sock'))

        def send(kind, payload):
            sock.sendall(bytes([kind]) + struct.pack('<I', len(payload)) + payload)

        send(1, struct.pack('<HHBHH', W, H, 0, 10, 20))
        frames, pending = bytearray(), bytearray()

        def receive(seconds):
            end = time.monotonic() + seconds
            while time.monotonic() < end:
                sock.settimeout(max(0.001, end - time.monotonic()))
                try:
                    data = sock.recv(1 << 20)
                except socket.timeout:
                    break
                assert data, 'daemon disconnected'
                pending.extend(data)
                while len(pending) >= 5:
                    n = struct.unpack_from('<I', pending, 1)[0]
                    if len(pending) < 5 + n:
                        break
                    if pending[0] == 5:
                        frames.extend(pending[5:5 + n])
                    del pending[:5 + n]

        def screen():
            # One whole frame, from a client attached just for it.
            peek = socket.socket(socket.AF_UNIX)
            peek.connect(str(root / 'mico/default.sock'))
            peek.sendall(bytes([1]) + struct.pack('<I', 9) + struct.pack('<HHBHH', W, H, 0, 10, 20))
            got, buf = bytearray(), bytearray()
            end = time.monotonic() + 0.6
            while time.monotonic() < end:
                peek.settimeout(max(0.001, end - time.monotonic()))
                try:
                    data = peek.recv(1 << 20)
                except socket.timeout:
                    break
                if not data:
                    break
                buf.extend(data)
                while len(buf) >= 5:
                    n = struct.unpack_from('<I', buf, 1)[0]
                    if len(buf) < 5 + n:
                        break
                    if buf[0] == 5:
                        got.extend(buf[5:5 + n])
                    del buf[:5 + n]
            peek.sendall(bytes([4]) + struct.pack('<I', 0))
            peek.close()
            receive(0.2)
            capture = root / 'screen.ansi'
            capture.write_bytes(got)
            out = subprocess.check_output([binary, '--vt', str(capture), '--dump', str(W), str(H)],
                                          cwd=directory, env=env).decode()
            return out.splitlines()[:-1]

        def mouse(button, x, y, final='M'):
            send(2, f'\x1b[<{button};{x + 1};{y + 1}{final}'.encode())

        receive(1.5)
        send(2, b':go Copy fixture\r')
        receive(0.4)
        send(2, b'\r')
        receive(2.5)
        lines = screen()

        # The chat pane: from the column of the last user turn's bar, between
        # the pane's title row and the message box.
        last = next((y for y, l in enumerate(lines) if 'Thanks, ship it' in l), None)
        top = next((y for y, l in enumerate(lines) if 'step number' in l), None)
        check(last is not None and top is not None, 'the chat shows its turns')
        if last is not None and top is not None:
            bar_x = lines[last].index('▌')
            check(any(l.rstrip().endswith(('│', '▐')) for l in lines[top:last]), 'the chat has a scrollbar')

            mark = len(frames)
            mouse(0, bar_x - 1, top)
            for y in range(top, last + 1, 4):
                mouse(32, W - 1, y)
            mouse(32, W - 1, last)
            mouse(0, W - 1, last, 'm')
            receive(0.8)
            got = [base64.b64decode(g).decode() for g in re.findall(rb'\x1b]52;c;([A-Za-z0-9+/=]*)', bytes(frames[mark:]))]
            check(len(got) == 1, f'a drag copies once (got {len(got)})')
            text = got[-1] if got else ''
            rows = text.split('\n')

            check(not (set(text) & BARS), f'no bars in the copy: {sorted(set(text) & BARS)}')
            check(all(r == r.rstrip() for r in rows), 'no line ends in padding')
            indent = {r.strip(): len(r) - len(r.lstrip()) for r in rows if r.strip()}
            check(min(indent.values(), default=1) == 0, "the pane's margin is left out")
            check('A quoted line' in indent and indent['A quoted line'] > indent.get('The end.', 99),
                  'a quote copies as its text, indented as it is drawn')
            check(indent.get('◦ nested item', 0) > indent.get('• outer item', 99), 'a nested item keeps its own indent')
            check('Mind the gap' in indent, "a callout's body copies without its bar")
            check(rows[-1].strip() == 'Thanks, ship it', "the user's turn copies without its gutter bar")

        if not failures:
            print('chat copy: no bars, no scrollbar, no margin; nesting kept')
    finally:
        daemon.terminate()
        try:
            daemon.wait(timeout=5)
        except subprocess.TimeoutExpired:
            daemon.kill()
            daemon.wait()
    sys.exit(1 if failures else 0)
