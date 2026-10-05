#!/usr/bin/env python3
"""What Claude runs in the background shows on its chat, and goes when it ends.

Against an isolated daemon and a stand-in for claude, resumed by the daemon:
the stand-in starts a monitor and moves a command to the background, as
Claude writes them in its transcript; the chat's row and the strip under
the chat say so; once their end notices are written, neither does.
Run: python3 tests/claude_background.py build/mico
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
SESSION = '7a6b5c4d-3e2f-4a1b-9c8d-7e6f5a4b3c2d'
W, H = 160, 40

FAKE_CLAUDE = r'''#!/usr/bin/env python3
import json, os, sys, time
if '--resume' not in sys.argv:
    sys.exit(0)
home = os.environ['HOME']
transcript = os.environ['FIXTURE_TRANSCRIPT']
sys.stdout.write('> \r\n')
sys.stdout.flush()
def write(r):
    with open(transcript, 'a') as f:
        f.write(json.dumps(r, separators=(',', ':')) + '\n')
ts = time.strftime('%Y-%m-%dT%H:%M:%S.000Z', time.gmtime())
time.sleep(1)
write({'type': 'assistant', 'timestamp': ts, 'message': {'content': [
    {'type': 'tool_use', 'id': 'm1', 'name': 'Monitor', 'input': {'description': 'watch the bake', 'timeout_ms': 600000}},
    {'type': 'tool_use', 'id': 'b1', 'name': 'Bash', 'input': {'command': 'make -j8', 'run_in_background': True}}]}})
write({'type': 'user', 'timestamp': ts, 'message': {'content': [{'type': 'tool_result', 'tool_use_id': 'm1', 'content': 'Monitor started'}]},
       'toolUseResult': {'taskId': 'tm1', 'timeoutMs': 600000}})
# Claude names a background command's output file in its result; a
# monitor's is beside it, under its own tmp folder.
tasks = os.path.join(os.environ['TMPDIR'], f'claude-{os.getuid()}', '-work', os.environ['FIXTURE_SESSION'], 'tasks')
os.makedirs(tasks)
with open(os.path.join(tasks, 'tb1.output'), 'w') as f:
    f.write('compiling\n[3/10] Building a.o\n')
with open(os.path.join(tasks, 'tm1.output'), 'w') as f:
    f.write('bake\r 20%|##        | 20/100\r 45%|####5     | 45/100 [00:10<02:05, 4.5it/s]')
write({'type': 'user', 'timestamp': ts, 'message': {'content': [{'type': 'tool_result', 'tool_use_id': 'b1',
       'content': f'Running in the background (ID: tb1). Output is being written to: {tasks}/tb1.output. You will be notified'}]},
       'toolUseResult': {'backgroundTaskId': 'tb1'}})
open(os.path.join(home, 'started'), 'w').close()
while not os.path.exists(os.path.join(home, 'end')):
    time.sleep(0.1)
for task in ('tm1', 'tb1'):
    write({'type': 'user', 'timestamp': ts, 'message': {'role': 'user', 'content':
        f'<task-notification>\n<task-id>{task}</task-id>\n<status>completed</status>\n<summary>done</summary>\n</task-notification>'}})
time.sleep(600)
'''


def wait_for(what, check, seconds=15):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if check():
            return
        time.sleep(0.05)
    raise AssertionError(f'timed out waiting for {what}')


with tempfile.TemporaryDirectory(prefix='mico-background-') as directory:
    root = Path(directory)
    work = root / 'work'
    work.mkdir()
    (root / 'bin').mkdir()
    fake = root / 'bin/claude'
    fake.write_text(FAKE_CLAUDE)
    fake.chmod(0o755)
    transcript = root / '.claude/projects/-work' / f'{SESSION}.jsonl'
    transcript.parent.mkdir(parents=True)
    transcript.write_text(''.join(json.dumps(r, separators=(',', ':')) + '\n' for r in [
        {'type': 'user', 'cwd': str(work), 'sessionId': SESSION, 'timestamp': '2026-10-01T10:00:00.000Z',
         'message': {'role': 'user', 'content': 'Bake the lightmaps'}},
        {'type': 'ai-title', 'aiTitle': 'Background fixture chat', 'sessionId': SESSION}]))
    (root / 'state/mico').mkdir(parents=True)
    (root / 'state/mico/running').write_text(f'claude\t{SESSION}\t{work}\n')
    config = root / 'config/mico'
    config.mkdir(parents=True)
    (config / 'folders').write_text(f'{work}\n')
    (config / 'view').write_text(f'tab Sessions\nfolder {work}\n')
    env = dict(os.environ, HOME=directory, XDG_CONFIG_HOME=str(root / 'config'),
               XDG_STATE_HOME=str(root / 'state'), XDG_RUNTIME_DIR=directory,
               PATH=f'{root / "bin"}:{os.environ.get("PATH", "")}', FIXTURE_TRANSCRIPT=str(transcript),
               FIXTURE_SESSION=SESSION, TMPDIR=str(root / 'tmp'))
    (root / 'tmp').mkdir()
    env.pop('DISPLAY', None)
    env.pop('WAYLAND_DISPLAY', None)
    daemon = subprocess.Popen([binary, '--daemon'], cwd=work, env=env,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
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
        return subprocess.check_output([binary, '--vt', str(capture), '--dump', str(W), str(H)],
                                       cwd=directory, env=env).decode()

    def wait_screen(what, check, seconds=15):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            receive(0.3)
            s = screen()
            if check(s):
                return s
        raise AssertionError(f'{what} never showed:\n{screen()}')

    try:
        address = root / 'mico/default.sock'
        wait_for('the socket', address.exists)
        client.connect(str(address))
        send(1, struct.pack('<HH', W, H))
        wait_for('the stand-in to start its work', (root / 'started').exists)
        # Open the chat, as a click on its row would.
        send(2, b':go Background fixture\r')
        receive(0.3)
        send(2, b'\r')  # the switcher's own Enter: open it
        s = wait_screen('the background work', lambda s: '2 in background' in s and '1 monitor' in s)
        if os.environ.get('SHOW'):
            print(s)
        assert '1 monitor · 1 command running' in s, 'the strip does not say what runs'
        # Progress, read from what each prints: the monitor's tqdm bar, found
        # under Claude's tmp folder, and the command's [n/m], from the file
        # its result named.
        s = wait_screen('the progress', lambda s: 'running 45%' in s and 'in background 45%' in s)
        row = next(y for y, l in enumerate(s.splitlines()) if 'running 45%' in l)
        col = s.splitlines()[row].index('◉')
        send(2, f'\x1b[<0;{col + 1};{row + 1}M\x1b[<0;{col + 1};{row + 1}m'.encode())
        s = wait_screen('the task list', lambda s: '45% · 45/100 · 2m 05s left' in s and '30% · 3/10' in s)
        send(2, b'\x1b')
        (root / 'end').touch()
        s = wait_screen('the work ending', lambda s: 'in background' not in s and 'running ▾' not in s)
        print('claude background: monitor and command shown while they run, gone when they end passed')
    finally:
        client.close()
        daemon.terminate()
        try:
            daemon.wait(timeout=5)
        except subprocess.TimeoutExpired:
            daemon.kill()
            daemon.wait()
        subprocess.run(['pkill', '-f', str(fake)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
