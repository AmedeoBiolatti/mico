#!/usr/bin/env python3
"""Agents outlive their daemon, and a finished turn is announced.

Against an isolated daemon and a stand-in for claude: a daemon resumes the
agents listed as running, keeps the list as they run, leaves it for the next
daemon when stopped with `mico kill`, and clears it on :quit. A client whose
terminal has lost focus is sent a notification when the agent's turn ends.
Run: python3 tests/daemon_restore.py build/mico
"""
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time

binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else 'build/mico').resolve())
SESSION = '0f8e2c1a-5b7d-4e3f-9a6c-2d1b0e9f8a7c'

# Claude as far as mico can tell: a working spinner, then an idle prompt.
FAKE_CLAUDE = r'''#!/usr/bin/env python3
import os, sys, time
# mico also asks claude what commands and models it offers; not a session.
if '--resume' not in sys.argv:
    sys.exit(0)
with open(os.path.join(os.environ['HOME'], 'argv.log'), 'a') as f:
    f.write(' '.join(sys.argv[1:2] + sys.argv[2:3]) + '\n')
time.sleep(0.5)
sys.stdout.write('✻ Working… (esc to interrupt)\r\n')
sys.stdout.flush()
time.sleep(2)
sys.stdout.write('\x1b[2J\x1b[H> \r\n')
sys.stdout.flush()
time.sleep(600)
'''


def wait_for(what, check, seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if check():
            return
        time.sleep(0.05)
    raise AssertionError(f'timed out waiting for {what}')


with tempfile.TemporaryDirectory(prefix='mico-daemon-restore-') as directory:
    root = Path(directory)
    work = root / 'work'
    work.mkdir()
    (root / 'bin').mkdir()
    fake = root / 'bin/claude'
    fake.write_text(FAKE_CLAUDE)
    fake.chmod(0o755)
    transcript = root / '.claude/projects/fixture' / f'{SESSION}.jsonl'
    transcript.parent.mkdir(parents=True)
    transcript.write_text('{"type":"user","message":{"content":"hello"},"cwd":"%s"}\n' % work)
    state = root / 'state/mico'
    state.mkdir(parents=True)
    running = state / 'running'
    running.write_text(f'claude\t{SESSION}\t{work}\n')
    argv_log = root / 'argv.log'

    env = dict(os.environ, HOME=directory, XDG_CONFIG_HOME=str(root / 'config'),
               XDG_STATE_HOME=str(root / 'state'), XDG_RUNTIME_DIR=directory,
               PATH=f'{root / "bin"}:{os.environ.get("PATH", "")}')
    env.pop('DISPLAY', None)
    env.pop('WAYLAND_DISPLAY', None)
    address = root / 'mico/default.sock'

    def start_daemon():
        daemon = subprocess.Popen([binary, '--daemon'], cwd=work, env=env,
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        wait_for('the daemon socket', lambda: address.exists() or daemon.poll() is not None, 5)
        assert daemon.poll() is None, 'fixture daemon exited at startup'
        return daemon

    def stop(daemon):
        try:
            daemon.wait(timeout=10)
        except subprocess.TimeoutExpired:
            daemon.kill()
            daemon.wait()
            raise AssertionError('daemon did not stop')

    class Client:
        def __init__(self, flags):
            self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.sock.connect(str(address))
            self.pending = bytearray()
            self.frames = bytearray()
            # u16 w, u16 h, u8 flags, u16 cell_w, u16 cell_h
            self.send(1, struct.pack('<HHBHH', 120, 32, flags, 0, 0))

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
                if not data:
                    break
                self.pending.extend(data)
                while len(self.pending) >= 5:
                    size = struct.unpack_from('<I', self.pending, 1)[0]
                    if len(self.pending) < 5 + size:
                        break
                    if self.pending[0] == 5:
                        self.frames.extend(self.pending[5:5 + size])
                    del self.pending[:5 + size]

    daemon = start_daemon()
    try:
        # The listed agent is resumed, not started afresh.
        wait_for('claude to be resumed', lambda: argv_log.exists() and SESSION in argv_log.read_text(), 10)
        first = argv_log.read_text().splitlines()
        assert len(first) == 1 and f'--resume {SESSION}' in first[0], first

        # A terminal that says it has lost focus and raises OSC 777
        # notifications is told when the turn ends.
        client = Client(flags=2 << 3)
        client.send(2, b'\x1b[O')
        end = time.monotonic() + 12
        while b'\x1b]777;notify;' not in client.frames and time.monotonic() < end:
            client.receive(0.2)
        assert b'\x1b]777;notify;' in client.frames, 'no notification for a finished turn'
        assert b'Claude finished' in client.frames, client.frames[-400:]

        # The daemon keeps the list while the agent runs.
        wait_for('the running list', lambda: running.exists() and SESSION in running.read_text(), 5)

        # `mico kill` stops the agents but leaves the list for the next daemon.
        client.send(7, b'')
        client.receive(1)
        client.sock.close()
        stop(daemon)
        assert running.exists() and SESSION in running.read_text(), 'mico kill dropped the running list'

        daemon = start_daemon()
        wait_for('claude to be resumed again', lambda: len(argv_log.read_text().splitlines()) == 2, 10)
        assert f'--resume {SESSION}' in argv_log.read_text().splitlines()[1]

        # Nobody is told while the terminal is in front.
        client = Client(flags=2 << 3)
        client.send(2, b'\x1b[I')
        client.receive(6)
        assert b'\x1b]777;' not in client.frames, 'notified a terminal that has focus'

        # :quit ends the agents for good.
        client.send(2, b':quit\r')
        client.receive(1)
        client.sock.close()
        stop(daemon)
        assert not running.exists(), ':quit left agents to resume'
        print('daemon restore: resume, notification, kill and quit passed')
    finally:
        if daemon.poll() is None:
            daemon.terminate()
            try:
                daemon.wait(timeout=5)
            except subprocess.TimeoutExpired:
                daemon.kill()
                daemon.wait()
