#!/usr/bin/env python3
"""Each agent, and the daemon, in a systemd scope of its own.

Where a systemd user manager runs scopes (it skips elsewhere): a client starts
the daemon in a scope of its own, the daemon starts an agent in another, and
killing the agent's whole scope, as systemd-oomd does under memory pressure,
takes that agent and what it ran but leaves the daemon, which logs it.
Run: python3 tests/agent_scopes.py build/mico
"""
import os
from pathlib import Path
import pty
import select
import subprocess
import sys
import tempfile
import time

binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else 'build/mico').resolve())
SESSION = '3d2c1b0a-9f8e-4d7c-8b6a-5f4e3d2c1b0a'

if subprocess.run(['systemd-run', '--user', '--scope', '--quiet', '--collect', 'true'],
                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode != 0:
    print('agent scopes: skipped, no systemd user scopes here')
    sys.exit(0)

# Claude as far as mico can tell, and a heavy process of its own, as an
# agent's build or test would be.
FAKE_CLAUDE = r'''#!/usr/bin/env python3
import os, subprocess, sys, time
if '--resume' not in sys.argv:
    sys.exit(0)
child = subprocess.Popen(['sleep', '600'])
home = os.environ['HOME']
open(os.path.join(home, 'agent.pid.tmp'), 'w').write(f'{os.getpid()} {child.pid}\n')
os.rename(os.path.join(home, 'agent.pid.tmp'), os.path.join(home, 'agent.pid'))
sys.stdout.write('> \r\n')
sys.stdout.flush()
time.sleep(600)
'''


def wait_for(what, check, seconds=15, drain=None):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if check():
            return
        if drain:
            drain()
        time.sleep(0.05)
    raise AssertionError(f'timed out waiting for {what}')


def scope_of(pid):
    line = Path(f'/proc/{pid}/cgroup').read_text().splitlines()[0]
    return line.rsplit('/', 1)[-1]


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False


with tempfile.TemporaryDirectory(prefix='mico-scopes-') as directory:
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
    (root / 'state/mico').mkdir(parents=True)
    (root / 'state/mico/running').write_text(f'claude\t{SESSION}\t{work}\n')
    env = dict(os.environ, HOME=directory, XDG_CONFIG_HOME=str(root / 'config'),
               XDG_STATE_HOME=str(root / 'state'), XDG_RUNTIME_DIR=directory,
               PATH=f'{root / "bin"}:{os.environ.get("PATH", "")}')
    env.pop('MICO_SCOPES', None)
    env.pop('DISPLAY', None)
    env.pop('WAYLAND_DISPLAY', None)
    pidfile = root / 'state/mico/daemon'
    log = root / 'state/mico/mico.log'

    # A client on a terminal of its own starts the daemon, as `mico` does.
    client, master = pty.fork()
    if client == 0:
        os.chdir(work)
        os.execve(binary, [binary], env)

    def drain():
        while select.select([master], [], [], 0)[0]:
            try:
                if not os.read(master, 65536):
                    return
            except OSError:
                return

    daemon = 0
    try:
        wait_for('the daemon', lambda: pidfile.exists() and pidfile.read_text().split(), drain=drain)
        daemon = int(pidfile.read_text().split()[0])
        unit = scope_of(daemon)
        assert unit.startswith('mico-daemon-') and unit.endswith('.scope'), f'the daemon runs in {unit}'

        # The agent the daemon resumes, in a scope of its own.
        agent_pid = root / 'agent.pid'
        wait_for('the agent', agent_pid.exists, drain=drain)
        agent, heavy = (int(x) for x in agent_pid.read_text().split())
        agent_unit = scope_of(agent)
        assert agent_unit.startswith('mico-claude-') and agent_unit.endswith('.scope'), f'the agent runs in {agent_unit}'
        assert scope_of(heavy) == agent_unit, 'what the agent runs is not in its scope'

        # Killed for memory: the whole scope, outright.
        subprocess.run(['systemctl', '--user', 'kill', '--signal=SIGKILL', agent_unit], check=True)
        wait_for('the agent to go', lambda: not alive(agent) and not alive(heavy), drain=drain)
        wait_for('the log of it', lambda: 'session KILLED (SIGKILL)' in log.read_text(), drain=drain)
        drain()
        assert alive(daemon), 'killing an agent\'s scope took the daemon'
        assert agent_unit in log.read_text(), 'the log does not name the scope to look up'
        print('agent scopes: daemon and agent in scopes of their own; an agent killed alone passed')
    finally:
        subprocess.run([binary, 'kill'], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if daemon:
            end = time.monotonic() + 10
            while alive(daemon) and time.monotonic() < end:
                drain()
                time.sleep(0.05)
        try:
            os.kill(client, 9)
        except ProcessLookupError:
            pass
        os.waitpid(client, 0)
        os.close(master)
