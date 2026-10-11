#!/usr/bin/env python3
"""Update and reload a working daemon, using a local release and fake curl.

No network, real agents, installed binaries or user state are touched.
"""
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tarfile
import tempfile
import time

source = Path(sys.argv[1] if len(sys.argv) > 1 else 'build/mico').resolve()
SESSION = '0f8e2c1a-5b7d-4e3f-9a6c-2d1b0e9f8a7c'
FAKE_AGENT = r'''#!/usr/bin/env python3
import os, select, signal, subprocess, sys, time, tty
if '--resume' not in sys.argv:
    sys.exit(0)
root = os.environ['HOME']
def stopped(sig, frame):
    with open(root + '/signals', 'a') as f: f.write(str(sig) + '\n')
    sys.exit(0)
signal.signal(signal.SIGHUP, stopped)
signal.signal(signal.SIGTERM, stopped)
with open(root + '/starts', 'a') as f: f.write(str(os.getpid()) + '\n')
# Work started by the agent must survive too, with the same process group.
child = subprocess.Popen([sys.executable, '-c',
    "import os,time; open(os.environ['HOME']+'/worker.pid','w').write(str(os.getpid())); "
    "exec(\"while True:\\n open(os.environ['HOME']+'/worker.beats','a').write('x')\\n time.sleep(.05)\")"])
tty.setraw(0)
sys.stdout.write('\x1b[2J\x1b[HRAW_SCREEN_KEEP 👩‍💻 δ́ 🇮🇹\r\n✻ Working… (esc to interrupt)\r\n')
sys.stdout.flush()
while True:
    with open(root + '/beats', 'a') as f: f.write('x')
    if select.select([0], [], [], .05)[0]:
        data = os.read(0, 65536)
        if not data: break
        with open(root + '/input', 'ab') as f: f.write(data)
'''
FAKE_CURL = r'''#!/usr/bin/env python3
import json, os, shutil, sys, time
root = os.environ['HOME']
if 'api.github.com' in sys.argv[-1]:
    if os.path.exists(root + '/offline'):
        sys.stderr.write('fixture: offline\n')
        sys.exit(22)
    sys.stdout.write(json.dumps({'tag_name':'v9999.0.0'}))
else:
    open(root + '/downloading', 'w').write('yes')
    time.sleep(.5)
    name = sys.argv[-1].rsplit('/', 1)[-1]
    shutil.copyfile(root + '/release/' + name, sys.argv[sys.argv.index('-o') + 1])
'''

with tempfile.TemporaryDirectory(prefix='mico-update-test-') as directory:
    root = Path(directory)
    work = root / 'work'
    work.mkdir()
    (root / 'bin').mkdir()
    binary = root / 'bin/mico'
    shutil.copy2(source, binary)
    for name, script in [('claude', FAKE_AGENT), ('curl', FAKE_CURL)]:
        (root / 'bin' / name).write_text(script)
        (root / 'bin' / name).chmod(0o755)
    state = root / 'state/mico'
    state.mkdir(parents=True)
    (state / 'running').write_text(f'claude\t{SESSION}\t{work}\n')
    transcript = root / f'.claude/projects/fixture/{SESSION}.jsonl'
    transcript.parent.mkdir(parents=True)
    transcript.write_text(json.dumps({'type':'user', 'sessionId':SESSION,
                                     'message':{'content':'hello'}, 'cwd':str(work)}) + '\n')
    config = root / 'config/mico'
    config.mkdir(parents=True)
    (config / 'folders').write_text(str(work) + '\n')
    (config / 'view').write_text(f'folder {work}\nchat {transcript}\n')
    release = root / 'release'
    release.mkdir()
    name = f'mico-v9999.0.0-linux-{platform.machine()}'
    archive = release / (name + '.tar.gz')
    with tarfile.open(archive, 'w:gz', compresslevel=1) as tar:
        tar.add(source, arcname=name + '/bin/mico')
    checksum = hashlib.sha256(archive.read_bytes()).hexdigest()
    (release / 'SHA256SUMS').write_text(f'{checksum}  {archive.name}\n')
    env = dict(os.environ, HOME=directory, XDG_CONFIG_HOME=str(root / 'config'),
               XDG_STATE_HOME=str(root / 'state'), XDG_RUNTIME_DIR=directory,
               PATH=f'{root / "bin"}:{os.environ.get("PATH", "")}', MICO_SCOPES='0')
    daemon = subprocess.Popen([str(binary), '--daemon'], cwd=work, env=env,
                              stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    clients = [socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) for _ in range(2)]
    pending = [bytearray(), bytearray()]
    frames = bytearray()

    def send(kind, payload=b'', index=0):
        clients[index].sendall(bytes([kind]) + struct.pack('<I', len(payload)) + payload)

    def receive(seconds=.1, index=0):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            clients[index].settimeout(max(.001, end - time.monotonic()))
            try:
                data = clients[index].recv(1 << 20)
            except socket.timeout:
                break
            assert data, 'attached terminal disconnected during reload'
            pending[index].extend(data)
            buf = pending[index]
            while len(buf) >= 5:
                n = struct.unpack_from('<I', buf, 1)[0]
                if len(buf) < 5 + n: break
                if buf[0] == 5 and index == 0: frames.extend(buf[5:5 + n])
                del buf[:5 + n]

    def wait_for(what, check, seconds=15):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            assert daemon.poll() is None, daemon.stderr.read().decode() if daemon.poll() is not None else ''
            if check(): return
            if all(c.fileno() >= 0 for c in clients) and (root / 'mico/default.sock').exists():
                try: receive(.04)
                except OSError: time.sleep(.04)
            else: time.sleep(.04)
        log = (state / 'mico.log').read_text() if (state / 'mico.log').exists() else ''
        raise AssertionError(f'timed out waiting for {what}\n{log}')

    def command(text):
        send(2, b'\x1bx' + text.encode() + b'\r')  # Alt+X reaches the command line over a chat

    def screen():
        capture = root / 'screen.ansi'
        capture.write_bytes(frames)
        return subprocess.check_output([str(binary), '--vt', str(capture), '--dump', '120', '40'],
                                       env=env, cwd=work).decode()

    def reloads():
        return (state / 'mico.log').read_text().count('reload complete')

    def uninterrupted():
        assert (root / 'starts').read_text().splitlines() == [str(agent_pid)]
        assert int((state / 'daemon').read_text().split()[0]) == daemon.pid
        assert not (root / 'signals').exists(), 'the agent received a shutdown signal'
        os.kill(agent_pid, 0)
        os.kill(worker_pid, 0)
        receive(.05, 1)  # the other terminal stayed attached too

    try:
        wait_for('daemon socket', lambda: (root / 'mico/default.sock').exists())
        for index, client in enumerate(clients):
            client.connect(str(root / 'mico/default.sock'))
            send(1, struct.pack('<HH', 120, 40), index)
        wait_for('working agent', lambda: (root / 'worker.pid').exists() and (root / 'beats').exists())
        agent_pid = int((root / 'starts').read_text().strip())
        worker_pid = int((root / 'worker.pid').read_text())
        # Enter the selected live chat, prepare a draft and reload in place.
        command('resume')
        receive(.2)
        send(2, b'\x1b3QUEUE_KEEP\x1b\r')
        receive(.2)
        rendered = screen()
        assert 'QUEUE_KEEP' in rendered, rendered
        send(2, b'DRAFT_KEEP')
        receive(.2)
        assert 'DRAFT_KEEP' in screen(), screen()
        command('reset')
        wait_for('first reload', lambda: reloads() == 1)
        receive(.2)
        uninterrupted()
        assert 'DRAFT_KEEP' in screen(), 'reload lost the unsent draft'
        assert 'QUEUE_KEEP' in screen(), 'reload lost the queued message'
        # The working terminal screen, including its parser state, is kept.
        command('view')
        receive(.2)
        assert 'RAW_SCREEN_KEEP' in screen(), screen()
        rendered = screen()
        assert '👩‍💻 δ́ 🇮🇹' in rendered, rendered
        send(2, b'PTY_AFTER_RESET')
        wait_for('pty input after reload', lambda: (root / 'input').exists() and
                 b'PTY_AFTER_RESET' in (root / 'input').read_bytes())
        command('view')
        receive(.1)
        # A checksum failure leaves the installed image and agents intact.
        installed_inode = binary.stat().st_ino
        (release / 'SHA256SUMS').write_text('0' * 64 + f'  {archive.name}\n')
        command('update')
        wait_for('checksum failure', lambda: 'update failed:' in (state / 'mico.log').read_text())
        assert binary.stat().st_ino == installed_inode
        assert reloads() == 1
        uninterrupted()
        # --check only looks, even when an update exists.
        command('update --check')
        wait_for('update check', lambda: "maintenance: mico " in (state / 'mico.log').read_text())
        assert binary.stat().st_ino == installed_inode and reloads() == 1
        # Download a verified release, reload it, and keep work advancing.
        (release / 'SHA256SUMS').write_text(f'{checksum}  {archive.name}\n')
        (root / 'downloading').unlink()
        command('update')
        wait_for('download to begin', lambda: (root / 'downloading').exists())
        beats = (root / 'beats').stat().st_size
        worker_beats = (root / 'worker.beats').stat().st_size
        receive(.25)
        assert (root / 'beats').stat().st_size > beats, 'update blocked the working agent'
        assert (root / 'worker.beats').stat().st_size > worker_beats
        wait_for('updated image reload', lambda: reloads() == 2)
        assert binary.stat().st_ino != installed_inode
        assert Path(f'/proc/{daemon.pid}/exe').stat().st_ino == binary.stat().st_ino
        uninterrupted()
        receive(.2)
        assert 'DRAFT_KEEP' in screen()
        assert 'QUEUE_KEEP' in screen()
        # An incompatible installed image is refused without falling back to kill.
        unsupported = root / 'unsupported'
        unsupported.write_text('#!/bin/sh\necho unsupported\nexit 2\n')
        unsupported.chmod(0o755)
        unsupported.replace(binary)
        command('reset')
        wait_for('refused reload', lambda: 'reload refused:' in (state / 'mico.log').read_text())
        assert reloads() == 2
        uninterrupted()
    finally:
        for client in clients: client.close()
        daemon.terminate()
        try: daemon.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            daemon.kill()
            daemon.communicate()

print('update: live pids, child work, terminals, drafts, pty, checksum and compatibility checks passed')
