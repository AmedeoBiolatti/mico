#!/usr/bin/env python3
"""Btw asks a throwaway Codex fork headless: one thread, no new chat, the parent untouched.

Each question runs `codex exec … fork <id> --ephemeral` with the question on
stdin; the thread is kept across a daemon reload, and a follow-up carries the
earlier answer.
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

from web_socket import WebSocket, free_port

binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else 'build/mico').resolve())
PARENT = '11111111-2222-4333-8444-555555555555'
FAKE = r'''#!/usr/bin/env python3
import json, os, sys, tty
root = os.environ['FIXTURE_ROOT']
with open(root + '/launches', 'a') as f:
    f.write(json.dumps(sys.argv[1:]) + '\n')
if sys.argv[1:2] == ['exec']:
    # A side question: the prompt on stdin, the answer as --json events.
    asked = sys.stdin.read()
    with open(root + '/asked', 'a') as f:
        f.write(asked + '\x1e')
    n = open(root + '/asked').read().count('\x1e')
    question = asked.rsplit('Side question: ', 1)[-1].strip()
    print('Reading additional input from stdin...', file=sys.stderr, flush=True)
    for event in ({'type': 'thread.started', 'thread_id': 'side'}, {'type': 'turn.started'},
                  {'type': 'item.completed', 'item': {'id': 'item_0', 'type': 'agent_message', 'text': f'SIDE_REPLY {n}: {question}'}},
                  {'type': 'turn.completed', 'usage': {}}):
        print(json.dumps(event), flush=True)
    sys.exit(0)
if len(sys.argv) < 3 or sys.argv[1] != 'resume':
    sys.exit(0)
tty.setraw(0)
os.write(1, b'> ')
while True:
    chunk = os.read(0, 65536)
    with open(root + '/parent-input', 'ab') as f:
        f.write(chunk)
'''

with tempfile.TemporaryDirectory(prefix='mico-work-fork-') as directory:
    root = Path(directory)
    work = root / 'work'
    work.mkdir()
    config = root / 'config/mico'
    config.mkdir(parents=True)
    port = free_port()
    (config / 'folders').write_text(f'{work}\n')
    (config / 'web').write_text(f'on {port}\n')
    (config / 'agent-hints').write_text('off\n')
    state = root / 'state/mico'
    state.mkdir(parents=True)
    (state / 'running').write_text(f'codex\t{PARENT}\t{work}\n')
    transcript = root / '.codex/sessions/2026/10/19/rollout-parent.jsonl'
    transcript.parent.mkdir(parents=True)
    transcript.write_text(''.join(json.dumps(r) + '\n' for r in [
        {'type':'session_meta', 'payload':{'id':PARENT, 'cwd':str(work), 'source':'cli'}},
        {'type':'response_item', 'payload':{'type':'message', 'role':'user', 'content':[{'type':'input_text','text':'Keep the main task running'}]}}
    ]))
    original = transcript.read_bytes()
    (root / 'bin').mkdir()
    fake = root / 'bin/codex'
    fake.write_text(FAKE)
    fake.chmod(0o755)
    env = dict(os.environ, HOME=directory, XDG_CONFIG_HOME=str(root / 'config'), XDG_STATE_HOME=str(root / 'state'),
               XDG_RUNTIME_DIR=directory, PATH=f'{root / "bin"}:{os.environ.get("PATH", "")}',
               FIXTURE_ROOT=directory)
    env.pop('CODEX_HOME', None)
    daemon = subprocess.Popen([binary, '--daemon'], cwd=work, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    ws = None
    try:
        end = time.monotonic() + 15
        while time.monotonic() < end:
            try:
                socket.create_connection(('127.0.0.1', port), timeout=.1).close()
                break
            except OSError:
                assert daemon.poll() is None, 'daemon exited'
                time.sleep(.02)
        ws = WebSocket(port, (config / 'web-token').read_text().strip())
        seen = []
        def wait(what, pred, seconds=20):
            until = time.monotonic() + seconds
            while time.monotonic() < until:
                seen.extend(ws.messages(.2))
                for m in reversed(seen):
                    if pred(m): return m
            raise AssertionError(f'timed out waiting for {what}: {seen[-3:]}')
        def command(**message):
            rid = str(time.monotonic_ns())
            ws.send(dict(message, rid=rid))
            result = wait('command result', lambda m:m.get('type') == 'result' and m.get('rid') == rid)
            assert result['ok'], result
            return result
        agents = wait('parent', lambda m:m['type'] == 'agents' and any(a['session_id'] == PARENT for a in m['agents']))
        key = next(a['key'] for a in agents['agents'] if a['session_id'] == PARENT)
        # Leave a serial-number gap so reloading must remap the side thread
        # by its live session, rather than accidentally matching its old key.
        spare = command(type='start', agent='codex', cwd=str(work))
        command(type='stop', key=spare['key'])
        ws.send({'type':'open', 'path':str(transcript)})
        wait('Btw support', lambda m:m['type'] == 'work_state' and m['supports_btw'])
        command(type='btw', key=key, text='Why is opening a chat slow?')
        wait('the side thread', lambda m:m['type'] == 'work_state' and
             any(i['id'] == 'btw:thread' and i['status'] == 'done' for i in m['items']))
        ws.send({'type':'work', 'path':str(transcript), 'item':'btw:thread', 'open':True})
        wait('the side reply', lambda m:m['type'] == 'work_state' and 'SIDE_REPLY 1: Why is opening a chat slow?' in m.get('output',''))
        agents = wait('the chat list', lambda m:m['type'] == 'agents')
        assert [a['session_id'] for a in agents['agents']] == [PARENT], f'a side question started a chat: {agents}'
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as reset:
            reset.settimeout(20)
            reset.connect(str(root / 'mico/default.sock'))
            reset.sendall(bytes([9]) + struct.pack('<I', 0))
            reply = b''
            while len(reply) < 5:
                chunk = reset.recv(65536)
                assert chunk, 'daemon disconnected before acknowledging reload'
                reply += chunk
            assert reply[0] == 10 and struct.unpack_from('<I', reply, 1)[0] == 0, reply
        ws.sock.close()
        ws = WebSocket(port, (config / 'web-token').read_text().strip())
        seen.clear()
        agents = wait('parent after reload', lambda m:m['type'] == 'agents' and any(a['session_id'] == PARENT for a in m['agents']))
        key = next(a['key'] for a in agents['agents'] if a['session_id'] == PARENT)
        ws.send({'type':'open', 'path':str(transcript)})
        wait('the side thread after reload', lambda m:m['type'] == 'work_state' and any(i['id'] == 'btw:thread' for i in m['items']))
        ws.send({'type':'work', 'path':str(transcript), 'item':'btw:thread', 'open':True})
        wait('the kept answer', lambda m:m['type'] == 'work_state' and 'SIDE_REPLY 1:' in m.get('output',''))
        command(type='btw', key=key, text='Would caching help?')
        wait('the follow-up reply', lambda m:m['type'] == 'work_state' and 'SIDE_REPLY 2: Would caching help?' in m.get('output','')
             and 'SIDE_REPLY 1:' in m.get('output',''))
        launches = [json.loads(line) for line in (root / 'launches').read_text().splitlines()]
        side = [l for l in launches if l[:1] == ['exec']]
        assert len(side) == 2 and all(l[l.index('fork') + 1] == PARENT and '--ephemeral' in l and
                                      'sandbox_mode="read-only"' in l and l[-1] == '-' for l in side), launches
        assert not any(l[:1] == ['fork'] for l in launches), f'a side question forked a live chat: {launches}'
        assert transcript.read_bytes() == original, 'a side question changed the parent transcript'
        assert not (root / 'parent-input').exists(), 'a side question was typed into the parent'
        asked = (root / 'asked').read_text().split('\x1e')
        assert 'do not edit anything' in asked[0], asked[0]
        assert 'SIDE_REPLY 1: Why is opening a chat slow?' in asked[1] and asked[1].rstrip().endswith('Would caching help?'), asked[1]
        assert daemon.poll() is None
        print('work btw: headless fork, no new chat, kept across reload, follow-up carries the thread, parent untouched')
    finally:
        if ws: ws.sock.close()
        daemon.terminate()
        try: daemon.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            daemon.kill(); daemon.communicate()
        subprocess.run(['pkill', '-f', str(fake)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
