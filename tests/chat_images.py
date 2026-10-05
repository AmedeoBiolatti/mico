#!/usr/bin/env python3
"""Pictures, charts and equations in a chat: a click zooms one, a second click
puts it back; a right-click copies it as an image, its LaTeX, its chart spec or
its file's path.

Against an isolated daemon, a client that says it shows kitty images, and a
chat holding a display equation, a chart and a PNG file. The clipboard is a
stand-in wl-copy that records what it was given.
Run: python3 tests/chat_images.py build/mico
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
import unicodedata
import zlib

binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else 'build/mico').resolve())
SESSION = '2b2c3d4e-5f6a-4b7c-8d9e-0f1a2b3c4d5e'
PH = '\U0010EEEE'  # kitty's image placeholder
W, H = 140, 50


def png(w, h):
    rows = b''.join(b'\x00' + b''.join(bytes([(x * 7) % 256, (y * 5) % 256, 128, 255]) for x in range(w))
                    for y in range(h))

    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


def clean(line):
    return ''.join(ch for ch in line if not unicodedata.combining(ch))


failures = []


def check(ok, what):
    if not ok:
        failures.append(what)
        print(f'FAIL images: {what}')


with tempfile.TemporaryDirectory(prefix='mico-images-') as directory:
    root = Path(directory)
    work = root / 'work'
    work.mkdir()
    picture = png(120, 60)
    (work / 'pic.png').write_bytes(picture)
    chart = json.dumps({'type': 'line', 'title': 'loss', 'x': [0, 1, 2, 3],
                        'series': [{'name': 't', 'y': [3, 2, 1.5, 1]}]})
    tex = r'\int_0^1 x^2\,dx = \frac{1}{3}'
    text = (f'Here is an equation:\n\n$${tex}$$\n\nA chart:\n\n```chart\n{chart}\n```\n\n'
            f'A picture:\n\n![pic]({work / "pic.png"})\n\nThe end.')
    transcript = root / '.claude/projects/-work' / f'{SESSION}.jsonl'
    transcript.parent.mkdir(parents=True)
    transcript.write_text('\n'.join(json.dumps(r) for r in [
        {'type': 'user', 'cwd': str(work), 'sessionId': SESSION, 'timestamp': '2026-10-01T10:00:00.000Z',
         'message': {'role': 'user', 'content': 'show me'}},
        {'type': 'assistant', 'cwd': str(work), 'sessionId': SESSION, 'timestamp': '2026-10-01T10:00:01.000Z',
         'message': {'role': 'assistant', 'content': [{'type': 'text', 'text': text}]}},
        {'type': 'ai-title', 'aiTitle': 'Images fixture', 'sessionId': SESSION}]) + '\n')
    config = root / 'config/mico'
    config.mkdir(parents=True)
    (config / 'folders').write_text(f'{work}\n')
    (config / 'view').write_text(f'tab Sessions\nfolder {work}\n')
    (root / 'bin').mkdir()
    fake = root / 'bin/wl-copy'
    fake.write_text('#!/bin/sh\necho "$@" > "$HOME/wl-args"\ncat > "$HOME/wl-got"\n')
    fake.chmod(0o755)
    env = dict(os.environ, HOME=directory, XDG_CONFIG_HOME=str(root / 'config'), XDG_STATE_HOME=str(root / 'state'),
               XDG_RUNTIME_DIR=directory, WAYLAND_DISPLAY='stand-in', PATH=f'{root / "bin"}:{os.environ.get("PATH", "")}')
    for k in ('TMUX', 'DISPLAY'):
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

        # A terminal that takes kitty's images, with 10x20 cells.
        send(1, struct.pack('<HHBHH', W, H, 1, 10, 20))
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
            # A client attached just for this gets one whole frame: nothing
            # replayed, nothing left over from the frames before.
            peek = socket.socket(socket.AF_UNIX)
            peek.connect(str(root / 'mico/default.sock'))
            peek.sendall(bytes([1]) + struct.pack('<I', 9) + struct.pack('<HHBHH', W, H, 1, 10, 20))
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
            return '\n'.join(out.splitlines()[:-1])  # the status line moves on by itself

        def row_of(label, after):
            lines = screen().splitlines()
            y = next((i for i, l in enumerate(lines) if label in clean(l)), None)
            return None if y is None else y + after

        def click(x, y, button=0):
            send(2, f'\x1b[<{button};{x + 1};{y + 1}M\x1b[<{button};{x + 1};{y + 1}m'.encode())
            receive(0.8)

        def pick(label):
            lines = screen().splitlines()
            y = next(i for i, l in enumerate(lines) if label in clean(l))
            click(clean(lines[y]).index(label), y)

        def copied(mark):
            got = re.findall(rb'\x1b]52;c;([A-Za-z0-9+/=]*)', bytes(frames[mark:]))
            return [base64.b64decode(g).decode() for g in got]

        receive(1.5)
        send(2, b':go Images fixture\r')
        receive(0.4)
        send(2, b'\r')
        receive(2.5)
        normal = screen()
        check(normal.count(PH) > 0, 'the chat shows its pictures as images')

        # Each zooms on a click, and is back as it was on the next.
        for name, label, after, col in [('equation', 'Here is an equation', 2, 47), ('chart', 'loss', 1, 52),
                                        ('picture', 'A picture', 2, 47)]:
            y = row_of(label, after)
            click(col, y)
            zoomed = screen()
            check(zoomed.count(PH) > normal.count(PH), f'a click zooms the {name}')
            click(col, row_of(label, after))
            back = screen()
            check(back == normal, f'a second click puts the {name} back, and the view with it')

        # Right-click: what each can be copied as.
        click(47, row_of('Here is an equation', 2), 2)
        menu = screen()
        check('Zoom in' in menu and 'Copy image' in menu and 'Copy LaTeX' in menu, 'an equation offers zoom, image, LaTeX')
        mark = len(frames)
        pick('Copy LaTeX')
        check(copied(mark) == [tex], 'Copy LaTeX copies its source')
        click(47, row_of('Here is an equation', 2), 2)
        pick('Copy image')
        got = (root / 'wl-got').read_bytes() if (root / 'wl-got').exists() else b''
        check(got.startswith(b'\x89PNG') and (root / 'wl-args').read_text().split() == ['--type', 'image/png'],
              'an equation is copied as a PNG')

        click(52, row_of('loss', 1), 2)
        menu = screen()
        check('Copy chart spec' in menu and 'Copy LaTeX' not in menu, 'a chart offers its spec, no LaTeX')
        mark = len(frames)
        pick('Copy chart spec')
        check(copied(mark) == [chart], 'Copy chart spec copies the block it was drawn from')

        click(47, row_of('A picture', 2), 2)
        menu = screen()
        check('Copy file path' in menu and 'Copy chart spec' not in menu, 'a picture offers its path')
        pick('Copy image')
        check((root / 'wl-got').read_bytes() == picture, 'a picture is copied as the file it is, byte for byte')
        click(47, row_of('A picture', 2), 2)
        mark = len(frames)
        pick('Copy file path')
        check(copied(mark) == [str(work / 'pic.png')], 'Copy file path copies the path')

        if not failures:
            print('chat images: zoom, and copy as image, LaTeX, spec and path passed')
    finally:
        daemon.terminate()
        try:
            daemon.wait(timeout=5)
        except subprocess.TimeoutExpired:
            daemon.kill()
            daemon.wait()
    sys.exit(1 if failures else 0)
