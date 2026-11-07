#!/usr/bin/env python3
"""A damaged transcript must not stall startup, rendering, indexes or shutdown.

Uses isolated state and stored fixture chats; never starts a coding agent.
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

with tempfile.TemporaryDirectory(prefix='mico-transcript-recovery-') as directory:
    root = Path(directory)
    work = root / 'work'
    work.mkdir()
    config = root / 'config/mico'
    config.mkdir(parents=True)
    (config / 'folders').write_text(str(work) + '\n')
    path = root / '.claude/projects/fixture/chat.jsonl'
    path.parent.mkdir(parents=True)

    def record(role, text):
        return json.dumps({'parentUuid': 'previous', 'type': role, 'cwd': str(work),
                           'message': {'role': role, 'content': [{'type': 'text', 'text': text}]}},
                          separators=(',', ':'))

    clipped_image = '{"parentUuid":"old","type":"user","message":{"content":[{"type":"image","source":{"data":"AAAA'
    clipped_signature = '{"parentUuid":"old","message":{"content":[{"type":"thinking","signature":"AAAA'
    malformed_array = '{"parentUuid":"old","type":"assistant","message":{"content":[}}\n'
    original = (record('user', 'BEFORE_DAMAGE') + '\n' + clipped_image +
                record('assistant', 'RECOVERED_IMAGE_SUFFIX') + '\n' + malformed_array +
                clipped_signature + record('assistant', 'RECOVERED_SIGNATURE_SUFFIX') + '\n' +
                record('user', 'JOINED_FIRST') + record('assistant', 'JOINED_SECOND') + '\n')
    path.write_text(original)
    (config / 'view').write_text(f'tab Sessions\nfolder {work}\nchat {path}\n')
    empty_path = root / 'bin'
    empty_path.mkdir()
    env = dict(os.environ, HOME=directory, XDG_CONFIG_HOME=str(root / 'config'),
               XDG_STATE_HOME=str(root / 'state'), XDG_RUNTIME_DIR=directory,
               PATH=str(empty_path), MICO_SCOPES='0')
    env.pop('DISPLAY', None)
    env.pop('WAYLAND_DISPLAY', None)
    env.pop('CLAUDE_CONFIG_DIR', None)
    env.pop('CODEX_HOME', None)
    daemon = subprocess.Popen([binary, '--daemon'], cwd=work, env=env,
                              stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

    def frame():
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(5)
            client.connect(str(root / 'mico/default.sock'))
            hello = struct.pack('<HH', 160, 48)
            client.sendall(bytes([1]) + struct.pack('<I', len(hello)) + hello)
            pending = bytearray()
            while True:
                data = client.recv(1 << 20)
                assert data, 'daemon disconnected while rendering a damaged transcript'
                pending.extend(data)
                while len(pending) >= 5:
                    kind = pending[0]
                    size = struct.unpack_from('<I', pending, 1)[0]
                    if len(pending) < 5 + size:
                        break
                    payload = bytes(pending[5:5 + size])
                    del pending[:5 + size]
                    if kind == 5:
                        client.sendall(bytes([4]) + struct.pack('<I', 0))
                        return payload

    try:
        deadline = time.monotonic() + 5
        while not (root / 'mico/default.sock').exists():
            assert daemon.poll() is None, 'daemon exited during startup'
            assert time.monotonic() < deadline, 'daemon never opened its socket'
            time.sleep(.02)
        captured = root / 'frame.ansi'
        captured.write_bytes(frame())
        screen = subprocess.check_output([binary, '--vt', str(captured), '--dump', '160', '48'],
                                         env=env, timeout=5).decode()
        for text in ('RECOVERED_IMAGE_SUFFIX', 'RECOVERED_SIGNATURE_SUFFIX', 'JOINED_FIRST', 'JOINED_SECOND'):
            assert text in screen, f'recovered content is missing from the screen: {text}\n{screen}'
        # A second attachment needs a fresh frame after startup/indexing.
        assert frame(), 'the daemon stopped responding after the initial frame'
        daemon.terminate()
        assert daemon.wait(timeout=5) == 0, 'daemon could not shut down cleanly'
        assert path.read_text() == original, 'recovery modified the agent transcript'
        log = (root / 'state/mico/mico.log').read_text()
        assert 'transcript recovery:' in log, 'recovery left no diagnostic in the log'
        print('transcript recovery: startup, rendering, reattach and shutdown passed; original unchanged')
    finally:
        if daemon.poll() is None:
            daemon.kill()
            daemon.wait(timeout=5)
