#!/usr/bin/env python3
"""Git in mico: a folder's branch and state, and the commits agents made.

Against an isolated daemon, a real repository and a Claude transcript in
which the agent committed: the sidebar shows the branch and how many files
differ, the Diff tab lists the commit by its chat with git's own diff, a
commit git no longer has says so, and :commit and :blame go to the chat.
Run: python3 tests/git_commits.py build/mico
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
SESSION = '6c1f0e9a-2b3d-4c5e-8f70-1a2b3c4d5e6f'
W, H = 170, 45


def git(repo, *args):
    return subprocess.check_output(['git', '-C', str(repo), *args], text=True,
                                   env=dict(os.environ, GIT_CONFIG_GLOBAL='/dev/null',
                                            GIT_CONFIG_SYSTEM='/dev/null')).strip()


with tempfile.TemporaryDirectory(prefix='mico-git-') as directory:
    root = Path(directory)
    repo = root / 'proj'
    repo.mkdir()
    git(repo, 'init', '-q', '-b', 'main')
    git(repo, 'config', 'user.name', 'Test')
    git(repo, 'config', 'user.email', 'test@example.com')
    (repo / 'a.txt').write_text('first line\nsecond line\n')
    git(repo, 'add', 'a.txt')
    git(repo, 'commit', '-q', '-m', 'Start')
    # What the agent's commit changed: its first line.
    (repo / 'a.txt').write_text('AGENT_LINE\nsecond line\n')
    git(repo, 'commit', '-q', '-am', 'Agent rewrites the first line')
    full = git(repo, 'rev-parse', 'HEAD')
    short = full[:7]
    (repo / 'scratch.txt').write_text('untracked\n')  # no chat wrote this one
    (repo / 'new.txt').write_text('hello\n')          # the chat wrote this one, below
    # A second work tree, on a branch of its own.
    git(repo, 'worktree', 'add', '-q', str(root / 'proj-wt'), '-b', 'feature')
    # A repository whose config names a program for git to run on every status:
    # mico asks git about the folder unbidden, and must never run it.
    hook = root / 'fsmonitor-hook'
    hook.write_text(f'#!/bin/sh\ntouch {root / "fsmonitor-ran"}\n')
    hook.chmod(0o755)
    git(repo, 'config', 'core.fsmonitor', str(hook))

    # A Claude chat in that folder that made the commit, and announced one
    # git has never had.
    stamp = '2026-10-01T10:00:00.000Z'
    now = time.strftime('%Y-%m-%dT%H:%M:%S.000Z', time.gmtime(time.time() + 5))
    records = [
        {'type': 'user', 'cwd': str(repo), 'sessionId': SESSION, 'timestamp': stamp,
         'message': {'role': 'user', 'content': 'Commit the change'}},
        {'type': 'ai-title', 'aiTitle': 'Commit fixture chat', 'sessionId': SESSION},
        {'type': 'assistant', 'cwd': str(repo), 'timestamp': stamp, 'message': {'role': 'assistant', 'content': [
            {'type': 'tool_use', 'id': 'tu1', 'name': 'Bash',
             'input': {'command': "git commit -am 'Agent rewrites the first line'"}}]}},
        {'type': 'user', 'cwd': str(repo), 'timestamp': stamp, 'message': {'role': 'user', 'content': [
            {'type': 'tool_result', 'tool_use_id': 'tu1',
             'content': f'[main {short}] Agent rewrites the first line\n 1 file changed, 1 insertion(+), 1 deletion(-)'}]}},
        {'type': 'assistant', 'cwd': str(repo), 'timestamp': stamp, 'message': {'role': 'assistant', 'content': [
            {'type': 'tool_use', 'id': 'tu2', 'name': 'Bash', 'input': {'command': "git commit --amend -m 'Gone'"}}]}},
        {'type': 'user', 'cwd': str(repo), 'timestamp': stamp, 'message': {'role': 'user', 'content': [
            {'type': 'tool_result', 'tool_use_id': 'tu2', 'content': '[main deadbee] Rewritten away'}]}},
        # Writes a file it has not committed: the Git tab names the chat.
        {'type': 'assistant', 'cwd': str(repo), 'timestamp': now, 'message': {'role': 'assistant', 'content': [
            {'type': 'tool_use', 'id': 'tu4', 'name': 'Write',
             'input': {'file_path': str(repo / 'new.txt'), 'content': 'hello\n'}}]}},
        {'type': 'user', 'cwd': str(repo), 'timestamp': now,
         'toolUseResult': {'type': 'create', 'filePath': str(repo / 'new.txt'), 'content': 'hello\n'},
         'message': {'role': 'user', 'content': [{'type': 'tool_result', 'tool_use_id': 'tu4', 'content': 'File created'}]}},
        # Only mentions a commit: not one.
        {'type': 'assistant', 'cwd': str(repo), 'timestamp': stamp, 'message': {'role': 'assistant', 'content': [
            {'type': 'tool_use', 'id': 'tu3', 'name': 'Bash', 'input': {'command': "grep -rn 'git commit' ."}}]}},
        {'type': 'user', 'cwd': str(repo), 'timestamp': stamp, 'message': {'role': 'user', 'content': [
            {'type': 'tool_result', 'tool_use_id': 'tu3', 'content': '[main abcdef1] Not a commit'}]}},
    ]
    transcript = root / '.claude/projects/-proj' / f'{SESSION}.jsonl'
    transcript.parent.mkdir(parents=True)
    # Compact, as Claude writes it.
    transcript.write_text(''.join(json.dumps(r, separators=(',', ':')) + '\n' for r in records))

    config = root / 'config/mico'
    config.mkdir(parents=True)
    (config / 'folders').write_text(f'{repo}\n')
    (config / 'view').write_text(f'tab 4\nfolder {repo}\ndiff-by commit\ndiff-span 3\n')

    env = dict(os.environ, HOME=directory, XDG_CONFIG_HOME=str(root / 'config'),
               XDG_STATE_HOME=str(root / 'state'), XDG_RUNTIME_DIR=directory,
               GIT_CONFIG_GLOBAL='/dev/null', GIT_CONFIG_SYSTEM='/dev/null')
    env.pop('DISPLAY', None)
    env.pop('WAYLAND_DISPLAY', None)
    daemon = subprocess.Popen([binary, '--daemon'], cwd=repo, env=env,
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

    def wait_screen(what, check, seconds=10):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            receive(0.3)
            s = screen()
            if check(s):
                return s
        raise AssertionError(f'{what} never showed:\n{screen()}')

    try:
        address = root / 'mico/default.sock'
        for _ in range(300):
            if address.exists():
                break
            assert daemon.poll() is None, 'fixture daemon exited at startup'
            time.sleep(0.01)
        client.connect(str(address))
        send(1, struct.pack('<HH', W, H))

        s = wait_screen('the branch and the commit', lambda s: 'main ±2' in s and 'AGENT_LINE' in s)
        if os.environ.get('SHOW'):
            print(s)
        assert 'Agent rewrites the first line' in s and short in s, s
        assert 'Commit fixture chat' in s, 'the commit is not shown with its chat'
        assert 'deadbee' in s and 'not in git' in s, 'a commit git lacks is not marked'
        assert 'abcdef1' not in s, 'a command that only mentions a commit was taken for one'
        assert '2 commits' in s, s

        # :commit opens the chat that made it, at the call.
        send(2, f':commit {short}\r'.encode())
        wait_screen(':commit', lambda s: 'Agent rewrites the first line \u00b7 made in Commit fixture chat' in s)
        # :blame does the same from a line: one no agent made, then one it did.
        send(2, b':blame a.txt:2\r')
        wait_screen(':blame of a line no agent made', lambda s: 'not a commit by any chat' in s)
        send(2, b':blame a.txt:1\r')
        wait_screen(':blame', lambda s: 'made in Commit fixture chat' in s and 'not a commit by any chat' not in s)
        if os.environ.get('SHOW'):
            print(screen())

        # The Git tab: both work trees, what is uncommitted and who wrote it,
        # and the commits, the agent's marked with its chat.
        send(2, b':git\r')
        s = wait_screen('the Git tab', lambda s: 'Work trees' in s and 'Start' in s and 'new.txt' in s)
        if os.environ.get('SHOW'):
            print(s)
        assert 'feature' in s and 'proj-wt' in s, 'the second work tree is missing'
        assert 'Changes \u00b7 proj \u00b7 2 files' in s, 'the uncommitted files are not counted'
        new_line = next(l for l in s.splitlines() if 'new.txt' in l)
        # The list is narrow: the chat's name is cut short.
        assert 'Claude \u00b7 Commit' in new_line, 'the file the chat wrote is not credited to it'
        scratch_line = next(l for l in s.splitlines() if 'scratch.txt' in l)
        assert 'Claude' not in scratch_line, 'a file no chat wrote was credited to one'
        # From the commit's mark on: the sidebar shares the screen row.
        agent_line = next(l for l in s.splitlines() if f' {short} ' in l)
        agent_line = agent_line[agent_line.index(f' {short} ') - 2:]
        assert '\u25c6' in agent_line and 'Claude \u00b7 Commit' in agent_line, 'the agent commit is not marked'
        start_hash = git(repo, 'rev-parse', '--short=7', 'HEAD~1')
        start_line = next(l for l in s.splitlines() if f' {start_hash} ' in l)
        start_line = start_line[start_line.index(f' {start_hash} ') - 2:]
        assert '\u25c6' not in start_line, 'a commit by a person is marked as an agent\'s'
        assert 'Test' in start_line, 'a commit by a person is not shown by its author'
        assert not (root / 'fsmonitor-ran').exists(), "a repository's core.fsmonitor program was run"
        print('git commits: status, commits by chat, :commit, :blame and the Git tab passed')
    finally:
        client.close()
        daemon.terminate()
        try:
            daemon.wait(timeout=5)
        except subprocess.TimeoutExpired:
            daemon.kill()
            daemon.wait()
