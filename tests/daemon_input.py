#!/usr/bin/env python3
"""Exercise fragmented paste and Escape over a real, isolated daemon socket."""
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time

binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else 'build/mico').resolve())
with tempfile.TemporaryDirectory(prefix='mico-daemon-input-') as directory:
    root = Path(directory)
    env = dict(os.environ, HOME=directory, XDG_CONFIG_HOME=directory,
               XDG_STATE_HOME=directory, XDG_RUNTIME_DIR=directory)
    daemon = subprocess.Popen([binary, '--daemon'], cwd=directory, env=env,
                              stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    pending = bytearray()
    frames = bytearray()

    def send(kind, payload):
        client.sendall(bytes([kind]) + struct.pack('<I', len(payload)) + payload)

    def receive(seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            client.settimeout(max(0.001, end - time.monotonic()))
            try:
                data = client.recv(1 << 20)
            except socket.timeout:
                break
            assert data, 'daemon disconnected unexpectedly'
            pending.extend(data)
            while len(pending) >= 5:
                size = struct.unpack_from('<I', pending, 1)[0]
                if len(pending) < 5 + size:
                    break
                if pending[0] == 5:
                    frames.extend(pending[5:5 + size])
                del pending[:5 + size]

    def screen():
        capture = root / 'screen.ansi'
        capture.write_bytes(frames)
        return subprocess.check_output([binary, '--vt', str(capture), '--dump', '120', '32'],
                                       cwd=directory, env=env).decode()

    try:
        address = root / 'mico/default.sock'
        for _ in range(200):
            if address.exists():
                break
            assert daemon.poll() is None, 'fixture daemon exited at startup'
            time.sleep(0.01)
        client.connect(str(address))
        send(1, struct.pack('<HH', 120, 32))
        receive(0.1)
        send(2, b':')
        receive(0.05)
        send(2, b'\x1b')
        time.sleep(0.005)
        send(2, b'[200~PASTE_BOUNDARY_CHECK\x1b[201~')
        receive(0.15)
        assert ': PASTE_BOUNDARY_CHECK' in screen(), 'fragmented paste was interpreted as keys'
        send(2, b'\x1b')
        receive(0.1)
        # Escape closes the command line, and what it held goes with it.
        assert 'PASTE_BOUNDARY_CHECK' not in screen(), 'standalone Escape was not delivered'
        print('daemon input: fragmented paste and standalone Escape passed')
    finally:
        client.close()
        daemon.terminate()
        try:
            daemon.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            daemon.kill()
            daemon.communicate()
