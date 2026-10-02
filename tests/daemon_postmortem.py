#!/usr/bin/env python3
"""A daemon that goes down leaves word of it in the log.

Against isolated daemons: a fatal signal is logged with what it was and a
backtrace; a daemon killed outright (SIGKILL, as the OOM killer does) is
noticed by the next one; a clean stop says why it stopped.
Run: python3 tests/daemon_postmortem.py build/mico
"""
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else 'build/mico').resolve())


def wait_for(what, check, seconds=10):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if check():
            return
        time.sleep(0.05)
    raise AssertionError(f'timed out waiting for {what}')


with tempfile.TemporaryDirectory(prefix='mico-postmortem-') as directory:
    root = Path(directory)
    env = dict(os.environ, HOME=directory, XDG_CONFIG_HOME=str(root / 'config'),
               XDG_STATE_HOME=str(root / 'state'), XDG_RUNTIME_DIR=directory)
    env.pop('DISPLAY', None)
    env.pop('WAYLAND_DISPLAY', None)
    address = root / 'mico/default.sock'
    log = root / 'state/mico/mico.log'
    pidfile = root / 'state/mico/daemon'

    def start():
        d = subprocess.Popen([binary, '--daemon'], cwd=directory, env=env,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        wait_for('the daemon', lambda: pidfile.exists() and f'{d.pid} ' in pidfile.read_text())
        wait_for('the socket', lambda: address.exists())
        return d

    def text():
        return log.read_text(errors='replace')

    daemons = []
    try:
        # A crash: the signal, and a backtrace.
        d = start()
        daemons.append(d)
        d.send_signal(signal.SIGSEGV)
        d.wait(timeout=10)
        t = text()
        assert '---- CRASH [daemon]' in t and 'SIGSEGV' in t, t[-2000:]
        assert 'while:' in t and 'backtrace' in t and '---- end of crash ----' in t, t[-2000:]
        assert pidfile.exists(), 'a crashed daemon cleaned up after itself'

        # The next daemon says the last one did not stop.
        before = len(t)
        d = start()
        daemons.append(d)
        wait_for('the note on the last daemon', lambda: 'ended without stopping' in text()[before:])
        wait_for('the journal lookup', lambda: 'the last daemon' in text()[text().find('ended without stopping', before) + 30:])

        # Killed outright, as the OOM killer does: nothing of its own, but the
        # next daemon notices.
        d.kill()
        d.wait(timeout=10)
        before = len(text())
        d = start()
        daemons.append(d)
        wait_for('the note on the killed daemon', lambda: 'ended without stopping' in text()[before:])

        # A clean stop says why, and leaves nothing for the next to report.
        client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        client.connect(str(address))
        client.sendall(bytes([7]) + struct.pack('<I', 0))
        d.wait(timeout=10)
        client.close()
        assert 'daemon stopping: mico kill' in text(), text()[-1500:]
        assert not pidfile.exists(), 'a daemon that stopped left its pid file'
        before = len(text())
        d = start()
        daemons.append(d)
        d.terminate()
        d.wait(timeout=10)
        tail = text()[before:]
        assert 'ended without stopping' not in tail, 'a clean stop was reported as a death'
        assert 'daemon stopping: SIGTERM' in tail, tail
        print('daemon postmortem: crash, kill and clean stops passed')
    finally:
        for d in daemons:
            if d.poll() is None:
                d.kill()
                d.wait()
