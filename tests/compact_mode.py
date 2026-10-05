#!/usr/bin/env python3
"""On a narrow terminal (a phone), one pane at a time, all of it by touch.

Against an isolated daemon, two tracked folders and a stand-in for claude,
driven by taps as a phone's terminal sends them: the chats come first, ‹
goes back to the folders, a folder goes on to its chats, a chat opens it,
≡ opens the menu, Alt+1/2/3 and Ctrl+1/2/3 move between the three. A wide client gets the
side-by-side layout as before.
Run: python3 tests/compact_mode.py build/mico
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
SESSION = '1b2c3d4e-5f6a-4b7c-8d9e-0f1a2b3c4d5e'

FAKE_CLAUDE = r'''#!/usr/bin/env python3
import sys, time
if '--resume' not in sys.argv:
    sys.exit(0)
sys.stdout.write('FAKE CLAUDE READY\r\n> ')
sys.stdout.flush()
time.sleep(600)
'''


def tap(x, y):
    # A tap, as SGR mouse reports it: press and release, 1-based.
    return f'\x1b[<0;{x + 1};{y + 1}M\x1b[<0;{x + 1};{y + 1}m'.encode()


with tempfile.TemporaryDirectory(prefix='mico-compact-') as directory:
    root = Path(directory)
    alpha, beta = root / 'alpha', root / 'beta'
    alpha.mkdir()
    beta.mkdir()
    (root / 'bin').mkdir()
    fake = root / 'bin/claude'
    fake.write_text(FAKE_CLAUDE)
    fake.chmod(0o755)
    transcript = root / '.claude/projects/-beta' / f'{SESSION}.jsonl'
    transcript.parent.mkdir(parents=True)
    transcript.write_text(''.join(json.dumps(r, separators=(',', ':')) + '\n' for r in [
        {'type': 'user', 'cwd': str(beta), 'sessionId': SESSION, 'timestamp': '2026-10-01T10:00:00.000Z',
         'message': {'role': 'user', 'content': 'Tidy the beta folder'}},
        {'type': 'ai-title', 'aiTitle': 'Beta fixture chat', 'sessionId': SESSION}]))
    config = root / 'config/mico'
    config.mkdir(parents=True)
    (config / 'folders').write_text(f'{alpha}\n{beta}\n')
    (config / 'view').write_text(f'tab Sessions\nfolder {alpha}\n')
    env = dict(os.environ, HOME=directory, XDG_CONFIG_HOME=str(root / 'config'),
               XDG_STATE_HOME=str(root / 'state'), XDG_RUNTIME_DIR=directory,
               PATH=f'{root / "bin"}:{os.environ.get("PATH", "")}')
    env.pop('DISPLAY', None)
    env.pop('WAYLAND_DISPLAY', None)
    daemon = subprocess.Popen([binary, '--daemon'], cwd=alpha, env=env,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    class Client:
        def __init__(self, w, h):
            self.w, self.h = w, h
            self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.sock.connect(str(root / 'mico/default.sock'))
            self.pending = bytearray()
            self.frames = bytearray()
            self.send(1, struct.pack('<HH', w, h))

        def send(self, kind, payload):
            self.sock.sendall(bytes([kind]) + struct.pack('<I', len(payload)) + payload)

        def receive(self, seconds):
            end = time.monotonic() + seconds
            while time.monotonic() < end:
                self.sock.settimeout(max(0.001, end - time.monotonic()))
                try:
                    data = self.sock.recv(1 << 20)
                except socket.timeout:
                    break
                assert data, 'daemon disconnected unexpectedly'
                self.pending.extend(data)
                while len(self.pending) >= 5:
                    size = struct.unpack_from('<I', self.pending, 1)[0]
                    if len(self.pending) < 5 + size:
                        break
                    if self.pending[0] == 5:
                        self.frames.extend(self.pending[5:5 + size])
                    del self.pending[:5 + size]

        def screen(self):
            capture = root / 'screen.ansi'
            capture.write_bytes(self.frames)
            return subprocess.check_output([binary, '--vt', str(capture), '--dump', str(self.w), str(self.h)],
                                           cwd=directory, env=env).decode()

        def wait(self, what, check, seconds=10):
            end = time.monotonic() + seconds
            while time.monotonic() < end:
                self.receive(0.25)
                s = self.screen()
                if check(s):
                    return s
            raise AssertionError(f'{what} never showed:\n{self.screen()}')

        def row_of(self, s, text):
            for y, line in enumerate(s.splitlines()):
                if text in line:
                    return y
            raise AssertionError(f'no row with {text!r}:\n{s}')

    try:
        for _ in range(300):
            if (root / 'mico/default.sock').exists():
                break
            time.sleep(0.01)
        phone = Client(46, 30)
        top = lambda s: s.splitlines()[0]

        # Nothing open yet: the selected folder's chats, with a way back.
        s = phone.wait('the chats', lambda s: '‹ Folders' in top(s) and 'alpha' in top(s))
        assert 'Sessions' not in top(s), 'the tab strip is still there'
        assert '≡' in top(s), 'no menu in the bar'

        # ‹ goes back to the folders.
        phone.send(2, tap(2, 0))
        s = phone.wait('the folders', lambda s: 'Folders' in top(s) and '‹' not in top(s))
        assert 'alpha' in s and 'beta' in s, s

        # A folder goes on to its chats.
        phone.send(2, tap(5, phone.row_of(s, 'beta')))
        s = phone.wait('beta\'s chats', lambda s: 'beta' in top(s) and 'Beta fixture chat' in s)

        # A chat opens it, and the bar says which.
        phone.send(2, tap(5, phone.row_of(s, 'Beta fixture chat')))
        s = phone.wait('the chat', lambda s: '‹ Chats' in top(s) and 'Beta fixture chat' in top(s))

        # ≡ is the menu: the tabs and what the function keys do.
        phone.send(2, tap(44, 0))
        s = phone.wait('the menu', lambda s: 'Go to a chat' in s and 'Settings' in s and 'Detach' in s)
        phone.send(2, b'\x1b')
        phone.receive(0.3)

        # Alt+1, 2, 3: folders, chats, the chat.
        phone.send(2, b'\x1b1')
        phone.wait('Alt+1', lambda s: top(s).strip().startswith('mico') and 'Folders' in top(s))
        phone.send(2, b'\x1b3')
        phone.wait('Alt+3', lambda s: '‹ Chats' in top(s))
        phone.send(2, b'\x1b2')
        phone.wait('Alt+2', lambda s: '‹ Folders' in top(s))
        # Ctrl+1, 2, 3 too, where the terminal sends them (CSI u or
        # modifyOtherKeys): a plain terminal has no bytes for them.
        phone.send(2, b'\x1b[49;5u')
        phone.wait('Ctrl+1', lambda s: top(s).strip().startswith('mico') and 'Folders' in top(s))
        phone.send(2, b'\x1b[27;5;51~')
        phone.wait('Ctrl+3', lambda s: '‹ Chats' in top(s))
        phone.sock.close()

        # A wide terminal: side by side, the tab strip back.
        desk = Client(140, 36)
        s = desk.wait('the wide layout', lambda s: 'Sessions' in top(s) and 'Projects' in s)
        desk.sock.close()
        print('compact mode: chats, folders, a chat, the menu, Alt+ and Ctrl+1/2/3; wide unchanged passed')
    finally:
        daemon.terminate()
        try:
            daemon.wait(timeout=5)
        except subprocess.TimeoutExpired:
            daemon.kill()
            daemon.wait()
        subprocess.run(['pkill', '-f', str(fake)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
