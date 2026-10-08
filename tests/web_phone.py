#!/usr/bin/env python3
"""`:web tailscale`: the web view put on the tailnet, and its address as a QR code.

Against an isolated daemon and a stand-in `tailscale` that answers `status`
and `serve` and keeps a log: mico asks for the machine's name, publishes with
`serve --bg` when nothing else is served, draws the address as a QR code for a
phone, and takes only its own serve down again with `:web off`. When OpenCV and
pyte are installed the QR code on the screen is decoded and must read back as
the address; without them that part is skipped.
Run: python3 tests/web_phone.py build/mico
"""
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
W, H = 120, 50
failures = []


def check(ok, what):
    if not ok:
        failures.append(what)
        print(f'FAIL web phone: {what}')


FAKE = r'''#!/bin/sh
echo "$@" >> "$HOME/tailscale.log"
case "$1 $2" in
  "status --json") echo '{"BackendState":"Running","Self":{"DNSName":"my-pc.tail1234.ts.net."}}' ;;
  "serve status") if [ -f "$HOME/serving" ]; then echo '{"TCP":{"443":{"HTTPS":true}}}'; else echo '{}'; fi ;;
  "serve --bg") touch "$HOME/serving" ;;
  "serve --https=443") rm -f "$HOME/serving" ;;
esac
'''

with tempfile.TemporaryDirectory(prefix='mico-phone-') as directory:
    root = Path(directory)
    work = root / 'work'
    work.mkdir()
    config = root / 'config/mico'
    config.mkdir(parents=True)
    (config / 'folders').write_text(f'{work}\n')
    (root / 'bin').mkdir()
    fake = root / 'bin/tailscale'
    fake.write_text(FAKE)
    fake.chmod(0o755)
    env = dict(os.environ, HOME=directory, XDG_CONFIG_HOME=str(root / 'config'), XDG_STATE_HOME=str(root / 'state'),
               XDG_RUNTIME_DIR=directory, PATH=f'{root / "bin"}:{os.environ.get("PATH", "")}')
    for k in ('TMUX', 'DISPLAY', 'WAYLAND_DISPLAY'):
        env.pop(k, None)
    daemon = subprocess.Popen([binary, '--daemon'], cwd=work, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(300):
            if (root / 'mico/default.sock').exists():
                break
            time.sleep(0.01)
        sock = socket.socket(socket.AF_UNIX)
        sock.connect(str(root / 'mico/default.sock'))
        frames = bytearray()
        pending = bytearray()

        def send(kind, payload):
            sock.sendall(bytes([kind]) + struct.pack('<I', len(payload)) + payload)

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
            capture = root / 'screen.ansi'
            capture.write_bytes(bytes(frames))
            out = subprocess.check_output([binary, '--vt', str(capture), '--dump', str(W), str(H)], cwd=directory, env=env).decode()
            return '\n'.join(out.splitlines()[:-1])

        send(1, struct.pack('<HHBHH', W, H, 0, 10, 20))
        receive(1.5)
        send(2, b':web tailscale\r')
        receive(1.5)
        calls = (root / 'tailscale.log').read_text().splitlines() if (root / 'tailscale.log').exists() else []
        check('status --json' in calls, 'it asks tailscale for this machine\'s name')
        check('serve --bg 7311' in calls, 'it publishes the web view with tailscale serve')
        check(calls.count('serve status --json') == 1, 'after looking at what is already served')
        check((config / 'web-host').read_text().strip() == 'my-pc.tail1234.ts.net', 'the tailnet name is kept')
        shown = screen()
        check('On your phone' in shown and 'c copies the address' in shown, 'the QR card is on screen')
        check('▀' in shown or '▄' in shown or '█' in shown, 'and holds a code')

        # The code on the screen reads back as the address, where there is a decoder.
        try:
            import cv2
            import numpy as np
            import pyte
        except ImportError:
            print('web phone: QR decoding skipped, no opencv or pyte here')
        else:
            token = (config / 'web-token').read_text().strip()
            want = f'https://my-pc.tail1234.ts.net/#token={token}'
            sc, st = pyte.Screen(W, H), pyte.ByteStream(pyte.Screen(W, H))
            st = pyte.ByteStream(sc)
            st.feed(bytes(frames))
            rows = [y for y in range(H) if any(sc.buffer[y][x].data == '▀' for x in range(W))]
            check(bool(rows), 'the card has its code rows')
            if rows:
                def color(name, default):
                    return default if name in ('default', '') else int(name, 16)
                xs = [x for x in range(W) if any(sc.buffer[y][x].data == '▀' for y in rows)]
                x0, x1 = min(xs), max(xs)
                mods = []
                for y in rows:
                    top, bottom = [], []
                    for x in range(x0, x1 + 1):
                        c = sc.buffer[y][x]
                        top.append(color(c.fg, 0xFFFFFF) == 0)
                        bottom.append(color(c.bg, 0xFFFFFF) == 0)
                    mods += [top, bottom]
                s = 8
                img = np.full(((len(mods) + 8) * s, (len(mods[0]) + 8) * s), 255, np.uint8)
                for y, r in enumerate(mods):
                    for x, d in enumerate(r):
                        if d:
                            img[(y + 4) * s:(y + 5) * s, (x + 4) * s:(x + 5) * s] = 0
                text, _, _ = cv2.QRCodeDetector().detectAndDecode(img)
                check(text == want, f'the code reads back as the address (got {text!r})')

        send(2, b'x')
        receive(0.5)
        check('On your phone' not in screen(), 'any key puts the card away')

        # Already served by mico: asking again does not serve twice.
        send(2, b':web tailscale\r')
        receive(1.0)
        send(2, b'x')
        receive(0.3)
        calls = (root / 'tailscale.log').read_text().splitlines()
        check(calls.count('serve --bg 7311') == 1, 'a second ask leaves its own serve as it is')

        send(2, b':web off\r')
        receive(1.0)
        calls = (root / 'tailscale.log').read_text().splitlines()
        check('serve --https=443 off' in calls and not (root / 'serving').exists(), ':web off takes its serve down')

        # Something else already served: left alone.
        (root / 'serving').touch()
        (root / 'tailscale.log').write_text('')
        (config / 'web-serve').write_text('')
        send(2, b':web tailscale\r')
        receive(1.0)
        calls = (root / 'tailscale.log').read_text().splitlines()
        check(not any(c.startswith('serve --bg') for c in calls), 'a serve that is not mico\'s is never replaced')
        send(2, b'x')
        receive(0.3)
        send(2, b':web off\r')
        receive(0.8)
        calls = (root / 'tailscale.log').read_text().splitlines()
        check(not any('off' in c for c in calls), 'nor taken down')

        if not failures:
            print('web phone: tailnet publish, QR code and clean-up passed')
    finally:
        daemon.terminate()
        try:
            daemon.wait(timeout=5)
        except subprocess.TimeoutExpired:
            daemon.kill()
            daemon.wait()
    sys.exit(1 if failures else 0)
