#!/usr/bin/env python3
"""Btw uses a separate Codex fork, reuses it, and leaves the parent running."""
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
CHILD = '66666666-7777-4888-8999-aaaaaaaaaaaa'
FAKE = r'''#!/usr/bin/env python3
import json, os, sys, tty
if len(sys.argv) < 3 or sys.argv[1] not in ('resume', 'fork'):
    sys.exit(0)
tty.setraw(0)
root = os.environ['FIXTURE_ROOT']
is_fork = sys.argv[1] == 'fork'
with open(root + '/launches', 'a') as f:
    f.write(json.dumps(sys.argv[1:]) + '\n')
path = root + '/.codex/sessions/2026/10/19/' + ('rollout-child.jsonl' if is_fork else 'rollout-parent.jsonl')
transcript = open(path, 'a+', buffering=1)
def record(type, payload):
    transcript.write(json.dumps({'type':type,'payload':payload}) + '\n')
if is_fork:
    record('session_meta', {'id':os.environ['CHILD_ID'], 'cwd':os.environ['WORK_DIR'], 'source':'cli',
                            'forked_from_id':os.environ['PARENT_ID']})
    record('response_item', {'type':'message','role':'assistant','content':[{'type':'output_text','text':'OLD_PARENT_REPLY'}]})
os.write(1, b'> ')
buffer = b''
while True:
    chunk = os.read(0, 65536)
    with open(root + ('/child-input' if is_fork else '/parent-input'), 'ab') as f:
        f.write(chunk)
    buffer += chunk
    while b'\r' in buffer:
        line, buffer = buffer.split(b'\r', 1)
        text = line.decode(errors='replace')
        if not text.strip():
            continue
        record('response_item', {'type':'message','role':'user','content':[{'type':'input_text','text':text}]})
        record('event_msg', {'type':'task_started'})
        record('response_item', {'type':'message','role':'assistant','content':[{'type':'output_text','text':'SIDE_REPLY: '+text}]})
        record('event_msg', {'type':'task_complete'})
        os.write(1, b'\r\n> ')
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
               FIXTURE_ROOT=directory, WORK_DIR=str(work), PARENT_ID=PARENT, CHILD_ID=CHILD)
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
        work_state = wait('the side conversation', lambda m:m['type'] == 'work_state' and any(i['kind'] == 'btw' and i['path'] for i in m['items']))
        item = next(i for i in work_state['items'] if i['kind'] == 'btw')
        ws.send({'type':'work', 'path':str(transcript), 'item':item['id'], 'open':True})
        reply = wait('the side reply', lambda m:m['type'] == 'work_state' and 'SIDE_REPLY:' in m.get('output',''))
        assert 'OLD_PARENT_REPLY' not in reply['output'], 'Btw repeated the inherited parent conversation'
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
        state = wait('side conversation after reload', lambda m:m['type'] == 'work_state' and any(i['kind'] == 'btw' for i in m['items']))
        item = next(i for i in state['items'] if i['kind'] == 'btw')
        ws.send({'type':'work', 'path':str(transcript), 'item':item['id'], 'open':True})
        command(type='btw', key=key, text='Would caching help?')
        wait('the follow-up reply', lambda m:m['type'] == 'work_state' and 'Would caching help?' in m.get('output',''))
        launches = [json.loads(line) for line in (root / 'launches').read_text().splitlines()]
        assert launches.count(['fork', PARENT]) == 1, launches
        assert transcript.read_bytes() == original, 'a side question changed the parent transcript'
        assert not (root / 'parent-input').exists(), 'a side question was typed into the parent'
        child_input = (root / 'child-input').read_text()
        assert 'Do not edit files or continue the main task' in child_input, child_input
        assert 'Would caching help?' in child_input, child_input
        assert daemon.poll() is None
        print('work fork: context fork, separate output, reload, follow-up reuse and untouched parent passed')
    finally:
        if ws: ws.sock.close()
        daemon.terminate()
        try: daemon.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            daemon.kill(); daemon.communicate()
        subprocess.run(['pkill', '-f', str(fake)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
