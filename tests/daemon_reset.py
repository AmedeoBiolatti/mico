#!/usr/bin/env python3
"""`mico reset` swaps the daemon and keeps the running chats.

Against an isolated daemon and a stand-in for claude: `mico reset` in a
terminal stops the daemon, starts another that resumes the listed agent, and
attaches to it; with no daemon running it only starts one.
Run: python3 tests/daemon_reset.py build/mico
"""
import os
from pathlib import Path
import pty
import signal
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


with tempfile.TemporaryDirectory(prefix='mico-daemon-reset-') as directory:
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
    (state / 'running').write_text(f'claude\t{SESSION}\t{work}\n')
    argv_log = root / 'argv.log'
    pidfile_dir = root / 'mico'

    env = dict(os.environ, HOME=directory, XDG_CONFIG_HOME=str(root / 'config'),
               XDG_STATE_HOME=str(root / 'state'), XDG_RUNTIME_DIR=directory,
               PATH=f'{root / "bin"}:{os.environ.get("PATH", "")}', TERM='xterm')
    env.pop('DISPLAY', None)
    env.pop('WAYLAND_DISPLAY', None)
    address = root / 'mico/default.sock'

    def reset_in_terminal():
        """Runs `mico reset` on a pty; returns (pid, fd) of the client."""
        pid, fd = pty.fork()
        if pid == 0:
            os.chdir(work)
            os.execve(binary, [binary, 'reset'], env)
        return pid, fd

    def finish(pid, fd):
        try:
            os.write(fd, b'q')  # detach
        except OSError:
            pass
        end = time.monotonic() + 10
        while time.monotonic() < end:
            try:
                if os.waitpid(pid, os.WNOHANG)[0]:
                    break
            except ChildProcessError:
                break
            try:
                os.read(fd, 1 << 16)
            except OSError:
                pass
            time.sleep(0.05)
        else:
            os.kill(pid, signal.SIGKILL)
            os.waitpid(pid, 0)
            raise AssertionError('the client did not detach')

    def kill_daemon():
        subprocess.run([binary, 'kill'], env=env, cwd=work, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, timeout=15)

    def drain(fd):
        try:
            os.read(fd, 1 << 16)
        except OSError:
            pass

    try:
        # No daemon: reset starts one, which resumes the listed agent.
        pid, fd = reset_in_terminal()
        wait_for('a daemon', address.exists, 10)
        wait_for('claude to be resumed', lambda: argv_log.exists() and SESSION in argv_log.read_text(), 10)
        time.sleep(0.5)
        drain(fd)
        assert len(argv_log.read_text().splitlines()) == 1, argv_log.read_text()
        finish(pid, fd)

        # A daemon running: reset replaces it, and the agent is resumed again.
        pid, fd = reset_in_terminal()
        wait_for('claude to be resumed again', lambda: len(argv_log.read_text().splitlines()) == 2, 15)
        assert all(f'--resume {SESSION}' in line for line in argv_log.read_text().splitlines())
        wait_for('the new daemon', address.exists, 5)
        finish(pid, fd)
    finally:
        kill_daemon()

print('ok')
