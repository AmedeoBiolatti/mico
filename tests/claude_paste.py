#!/usr/bin/env python3
"""Check what the mico prompt box sends to the agent's pty: long-text and image
pastes (mode 'paste'), and messages sent while the agent works (mode 'queue').

No model is contacted. The fixture stands in for Claude Code 2.1.280 as measured:
it enables bracketed paste, shows pasted text at once, and turns a pasted image
path into "[Image #N]" asynchronously, about 100 ms later. Input that arrives
while an image is still being attached is recorded as an overtake: that is the
race that sent text ahead of the image and could send a message without it.
The queue fixture draws Claude's spinner while it "works" for 1.2 s after each
message, which is what mico's busy detection reads, and records whether it was
working when each message arrived: Enter while working must arrive at once
(steering), Alt+Enter must wait for the end of the turn, one per turn.
Run: python3 tests/claude_paste.py
"""
import json
import os
from pathlib import Path
import select
import subprocess
import sys
import tempfile
import time
import tty

START, END = b'\x1b[200~', b'\x1b[201~'


def fixture(root):
    tty.setraw(0)
    os.write(1, b'\x1b[?2004h' + b'PASTE READY\r\n')
    (root / 'ready').touch()
    received = b''
    attaching_until = 0.0
    images = 0
    while True:
        if not select.select([0], [], [], 0.02)[0]:
            if attaching_until and time.time() >= attaching_until:
                attaching_until = 0.0
                images += 1
                os.write(1, f'[Image #{images}]\r\n'.encode())
            continue
        chunk = os.read(0, 65536)
        if attaching_until:
            with (root / 'overtaken').open('ab') as out:
                out.write(chunk + b'\n')
        received += chunk
        (root / 'received').write_bytes(received)
        # Echo each complete paste the way the input box would show it.
        while START in received and END in received.split(START, 1)[1]:
            before, rest = received.split(START, 1)
            body, received_rest = rest.split(END, 1)
            if body.endswith(b'.png') and body.startswith(b'/'):
                attaching_until = time.time() + 0.1
            else:
                os.write(1, b'TEXT ' + str(len(body)).encode() + b'\r\n')
            (root / 'pastes').open('ab').write(json.dumps(body.decode()).encode() + b'\n')
            received = before + received_rest
        if received.endswith(b'\r'):
            (root / 'done').touch()
            time.sleep(3)
            return


def queue_fixture(root):
    tty.setraw(0)
    # Claude's input box: its ❯ prompt framed by rules, as it draws it.
    rule = '\u2500'.encode() * 40 + b'\r\n'
    box = rule + '\u276f '.encode() + b'\r\n' + rule
    idle = b'\x1b[2J\x1b[H\x1b[?2004h> ready\r\n' + box
    working = b'\x1b[2J\x1b[H' + '\u273b Thinking\u2026 (esc to interrupt)'.encode() + b'\r\n' + box
    os.write(1, idle)
    (root / 'ready').touch()
    busy_until = 0.0
    pending = b''
    log = []
    while True:
        now = time.time()
        if busy_until and now >= busy_until:
            busy_until = 0.0
            os.write(1, idle)
        if not select.select([0], [], [], 0.02)[0]:
            continue
        pending += os.read(0, 65536)
        while b'\r' in pending:
            msg, pending = pending.split(b'\r', 1)
            text = msg.replace(START, b'').replace(END, b'').decode()
            was_working = busy_until > 0
            log.append({'text': text, 'working': was_working, 't': time.time()})
            (root / 'log.json').write_text(json.dumps(log))
            if not was_working:
                busy_until = time.time() + 1.2
                os.write(1, working)
            # Keep the spinner on screen while working, as Claude redraws it.
        if busy_until:
            os.write(1, working)


def run():
    repo = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix='mico-paste-') as directory:
        base = Path(directory)
        binary = base / 'driver'
        objects = [str(p) for p in (Path(os.environ.get('MICO_BUILD', repo / 'build')) / 'CMakeFiles/mico.dir/src').rglob('*.o')
                   if p.name not in ('main.cpp.o', 'selftest.cpp.o', 'regression_test.cpp.o', 'bench.cpp.o')]
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++23', '-O2', '-flto', '-I' + str(repo / 'src'),
                        str(repo / 'tests/claude_paste_driver.cpp'), *objects, '-lutil', '-o', str(binary)], check=True)
        root = base / 'root'
        root.mkdir()
        transcript = root / '.claude/projects/fixture/paste-fixture.jsonl'
        transcript.parent.mkdir(parents=True)
        transcript.write_text(json.dumps({'type': 'assistant', 'message': {'content': [
            {'type': 'text', 'text': 'READY FOR INPUT'}]}}) + '\n')
        image = root / 'shot.png'
        image.write_bytes(b'\x89PNG\r\n\x1a\n')
        long_text = '\n'.join(f'log line {i:04d}' for i in range(120))
        (root / 'long.txt').write_text(long_text)
        env = dict(os.environ, HOME=str(root), XDG_CONFIG_HOME=str(root / 'config'))
        subprocess.run([str(binary), str(Path(__file__).resolve()), str(root), str(image), 'paste'],
                       env=env, cwd=root, check=True, timeout=30)
        pastes = [json.loads(line) for line in (root / 'pastes').read_text().splitlines()]
        expected = [long_text + 'look ', str(image), ' end']
        assert pastes == expected, ('pastes differ', pastes[:1] and len(pastes[0]), [p[:40] for p in pastes])
        assert not (root / 'overtaken').exists(), (
            'input overtook an image being attached: ' + (root / 'overtaken').read_bytes().decode(errors='replace'))
        print('Claude paste handoff: long text and image passed', flush=True)

        qroot = base / 'queue'
        qroot.mkdir()
        qt = qroot / '.claude/projects/fixture/paste-fixture.jsonl'
        qt.parent.mkdir(parents=True)
        qt.write_text(transcript.read_text())
        qenv = dict(os.environ, HOME=str(qroot), XDG_CONFIG_HOME=str(qroot / 'config'))
        subprocess.run([str(binary), str(Path(__file__).resolve()), str(qroot), str(image), 'queue'],
                       env=qenv, cwd=qroot, check=True, timeout=40)
        log = json.loads((qroot / 'log.json').read_text())
        texts = [m['text'] for m in log]
        assert texts == ['A', 'D', 'B', 'C'], texts
        a, d, b, c = log
        assert d['working'], 'Enter while working must steer: arrive during the turn'
        assert not b['working'] and not c['working'], 'queued messages must wait for the turn to end'
        assert c['t'] - b['t'] > 1.0, 'one queued message per turn'
        print('Claude queue and steering passed', flush=True)


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--fixture':
        fixture(Path(sys.argv[2]))
    elif len(sys.argv) > 1 and sys.argv[1] == '--queue-fixture':
        queue_fixture(Path(sys.argv[2]))
    else:
        run()
