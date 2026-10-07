#!/usr/bin/env python3
"""Git in mico, for a folder holding several repositories.

A tracked folder that is in no repository holds three (one two levels
down), one of which has a submodule and another a repository git sees only
as an untracked folder. A Claude chat ran in the folder itself, committed in
one of them with `cd api && git commit`, and wrote a file there. The sidebar
counts the repositories, the Git tab lists them and focuses the one with
changes, credits the chat's file and commit, lists every repository's
changes on `g`, lists the chat once resumed under the work tree it changed,
focuses another repository on Enter, and the Diff tab and :blame find the
commit although the chat's folder is no repository.
Run: python3 tests/git_repos.py build/mico
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
SESSION = '0d9e1c2b-3a4f-4e5d-9c6b-7a8f9e0d1c2b'
# Claude as far as mico can tell: resumed, it waits.
FAKE_CLAUDE = r'''#!/usr/bin/env python3
import sys, time
if '--resume' not in sys.argv:
    sys.exit(0)
sys.stdout.write('> \r\n')
sys.stdout.flush()
time.sleep(600)
'''
W, H = 170, 45
GIT_ENV = dict(os.environ, GIT_CONFIG_GLOBAL='/dev/null', GIT_CONFIG_SYSTEM='/dev/null')


def git(repo, *args):
    return subprocess.check_output(['git', '-C', str(repo), '-c', 'user.name=Test', '-c', 'user.email=t@example.com',
                                    '-c', 'protocol.file.allow=always', *args],
                                   text=True, env=GIT_ENV, stderr=subprocess.DEVNULL).strip()


def new_repo(path, files):
    path.mkdir(parents=True)
    git(path, 'init', '-q', '-b', 'main')
    for name, text in files.items():
        (path / name).write_text(text)
    git(path, 'add', '.')
    git(path, 'commit', '-q', '-m', 'Start ' + path.name)


with tempfile.TemporaryDirectory(prefix='mico-repos-') as directory:
    root = Path(directory)
    folder = root / 'platform'
    folder.mkdir()
    api, web = folder / 'api', folder / 'web'
    new_repo(api, {'a.txt': 'first\nsecond\n'})
    new_repo(root / 'libsrc', {'lib.txt': 'lib\n'})
    git(api, 'submodule', 'add', '-q', str(root / 'libsrc'), 'vendor/lib')
    git(api, 'commit', '-q', '-m', 'Add the lib')
    (api / 'vendor/lib/lib.txt').write_text('changed in the submodule\n')
    # The chat's commit, made from the folder above with cd.
    (api / 'a.txt').write_text('API_LINE\nsecond\n')
    git(api, 'commit', '-q', '-am', 'Agent edits api')
    short = git(api, 'rev-parse', '--short=7', 'HEAD')
    (api / 'new.txt').write_text('hello\n')  # the chat wrote this one, below
    new_repo(web, {'index.html': '<p>hi</p>\n'})
    (web / 'index.html').write_text('<p>WEB_CHANGE</p>\n')
    (web / 'inner').mkdir()
    git(web / 'inner', 'init', '-q', '-b', 'main')
    new_repo(folder / 'libs/shared', {'s.txt': 's\n'})
    # A work tree of web beside it: listed with web, not as a repository.
    git(web, 'worktree', 'add', '-q', str(folder / 'web-wt'), '-b', 'feature')

    stamp = '2026-10-01T10:00:00.000Z'
    now = time.strftime('%Y-%m-%dT%H:%M:%S.000Z', time.gmtime(time.time() + 5))
    later = time.strftime('%Y-%m-%dT%H:%M:%S.000Z', time.gmtime(time.time() + 10))
    cwd = str(folder)
    records = [
        {'type': 'user', 'cwd': cwd, 'sessionId': SESSION, 'timestamp': stamp,
         'message': {'role': 'user', 'content': 'Edit api'}},
        {'type': 'ai-title', 'aiTitle': 'Platform chat', 'sessionId': SESSION},
        {'type': 'assistant', 'cwd': cwd, 'timestamp': stamp, 'message': {'role': 'assistant', 'content': [
            {'type': 'tool_use', 'id': 'tu1', 'name': 'Bash', 'input': {'command': "cd api && git commit -am 'Agent edits api'"}}]}},
        {'type': 'user', 'cwd': cwd, 'timestamp': stamp, 'message': {'role': 'user', 'content': [
            {'type': 'tool_result', 'tool_use_id': 'tu1',
             'content': f'[main {short}] Agent edits api\n 1 file changed, 1 insertion(+), 1 deletion(-)'}]}},
        {'type': 'assistant', 'cwd': cwd, 'timestamp': now, 'message': {'role': 'assistant', 'content': [
            {'type': 'tool_use', 'id': 'tu2', 'name': 'Write',
             'input': {'file_path': str(api / 'new.txt'), 'content': 'hello\n'}}]}},
        {'type': 'user', 'cwd': cwd, 'timestamp': now,
         'toolUseResult': {'type': 'create', 'filePath': str(api / 'new.txt'), 'content': 'hello\n'},
         'message': {'role': 'user', 'content': [{'type': 'tool_result', 'tool_use_id': 'tu2', 'content': 'File created'}]}},
        # Last, it works in web's other work tree, from where it runs.
        {'type': 'assistant', 'cwd': cwd, 'timestamp': later, 'message': {'role': 'assistant', 'content': [
            {'type': 'tool_use', 'id': 'tu3', 'name': 'Bash', 'input': {'command': 'cd web-wt && make'}}]}},
        {'type': 'user', 'cwd': cwd, 'timestamp': later, 'message': {'role': 'user', 'content': [
            {'type': 'tool_result', 'tool_use_id': 'tu3', 'content': 'make: Nothing to be done.'}]}},
    ]
    transcript = root / '.claude/projects/-platform' / f'{SESSION}.jsonl'
    transcript.parent.mkdir(parents=True)
    transcript.write_text(''.join(json.dumps(r, separators=(',', ':')) + '\n' for r in records))

    config = root / 'config/mico'
    config.mkdir(parents=True)
    (config / 'folders').write_text(f'{folder}\n')
    (config / 'view').write_text(f'tab 4\nfolder {folder}\nchat {transcript}\ndiff-by commit\ndiff-span 3\n')
    (root / 'bin').mkdir()
    (root / 'bin/claude').write_text(FAKE_CLAUDE)
    (root / 'bin/claude').chmod(0o755)

    env = dict(GIT_ENV, HOME=directory, XDG_CONFIG_HOME=str(root / 'config'),
               XDG_STATE_HOME=str(root / 'state'), XDG_RUNTIME_DIR=directory,
               PATH=f'{root / "bin"}:{os.environ.get("PATH", "")}')
    env.pop('DISPLAY', None)
    env.pop('WAYLAND_DISPLAY', None)
    daemon = subprocess.Popen([binary, '--daemon'], cwd=folder, env=env,
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

    def show(s):
        if os.environ.get('SHOW'):
            print(s)

    def line_with(s, text):
        return next(l for l in s.splitlines() if text in l)

    try:
        address = root / 'mico/default.sock'
        for _ in range(300):
            if address.exists():
                break
            assert daemon.poll() is None, 'fixture daemon exited at startup'
            time.sleep(0.01)
        client.connect(str(address))
        send(1, struct.pack('<HH', W, H))

        # The Diff tab: the commit the chat made from the folder above is
        # found in the repository it went to, with git's diff.
        s = wait_screen('the commit by chat', lambda s: short in s and 'API_LINE' in s)
        show(s)
        assert 'not in git' not in s and 'no repo' not in s, 'the commit was looked for in the chat\'s folder only'
        # The sidebar: the folder is in no repository, and holds five, the
        # last two found in what git says of the first three.
        wait_screen('the repositories counted', lambda s: '5 repos' in s)

        # :blame of a file in a repository inside the selected folder.
        send(2, b':blame api/a.txt:1\r')
        wait_screen(':blame', lambda s: 'made in Platform chat' in s)

        # The Git tab opens on the changes of the repository that has some.
        send(2, b':git\r')
        s = wait_screen('the changes', lambda s: 'platform \u203a api' in s and '5 Repos 5' in s and 'new.txt' in s
                        and 'submodule' in s)
        show(s)
        assert 'Claude \u00b7 Platform' in line_with(s, 'new.txt'), 'the file the chat wrote is not credited to it'
        assert 'submodule' in line_with(s, 'lib'), 'the submodule is not marked as one'
        assert 'index.html' not in s, 'another repository\'s changes are listed'

        # 5: the repositories, nested ones under theirs.
        send(2, b'5')
        s = wait_screen('the repositories', lambda s: 'libs/shared' in s and 'inner' in s)
        show(s)
        for name in ('libs/shared', 'web', 'vendor/lib', 'inner'):
            assert name in s, f'{name} is not listed'
        assert '\u2514 vendor/lib' in line_with(s, 'vendor/lib'), 'the submodule is not drawn under its repository'
        assert 'new commits' not in line_with(s, 'vendor/lib') and 'changed' in line_with(s, 'vendor/lib'), \
            'the submodule\'s state is not shown'
        assert 'web-wt' not in s, 'the work tree is listed as a repository'

        # 2: the log, the commit the chat made from the folder above marked.
        send(2, b'2')
        s = wait_screen('the log', lambda s: f' {short} ' in s and 'Agent edits api' in s)
        show(s)
        commit_line = line_with(s, f' {short} ')
        commit_line = commit_line[commit_line.index(f' {short} ') - 2:]
        assert '\u25c6' in commit_line, 'the commit the chat made from the folder above is not marked'

        # g: every repository's changes, each under its name; g again, one's.
        send(2, b'1g')
        s = wait_screen('every repository\'s changes', lambda s: 'g one repo' in s and 'index.html' in s)
        show(s)
        send(2, b'g')
        wait_screen('the focused repository\'s changes', lambda s: 'index.html' not in s)

        # Enter on a repository focuses it, and shows its changes.
        send(2, b'5jjj\r')  # api, vendor/lib, libs/shared, web
        s = wait_screen('web focused', lambda s: 'platform \u203a web' in s and 'index.html' in s)
        show(s)
        assert 'inner/' in s and 'repository' in line_with(s, 'inner/'), 'the nested repository is not marked as one'
        # 4: its work trees, the one beside it among them.
        send(2, b'4')
        wait_screen('web\'s work trees', lambda s: 'feature' in s and 'web-wt' in s)

        # The chat resumed runs in the folder, and last worked in web's other
        # work tree: it is listed there, saying where it runs.
        send(2, b':resume\r')
        receive(1)
        # The chat's pane takes ':' as text: the palette instead.
        send(2, b'\x1bOP')
        receive(0.3)
        send(2, b'git\r')
        s = wait_screen('the resumed chat in the Git tab', lambda s: 'runs in ./' in s)
        show(s)
        agent_line = line_with(s, 'runs in ./')
        assert 'Claude' in agent_line and 'Platform chat' in agent_line, 'the agent is not named'
        assert s.index('web-wt') < s.index('runs in ./'), 'the agent is not under the work tree it works in'
        print('git repos: several repositories, their changes, commits, blame and agents passed')
    finally:
        client.close()
        daemon.terminate()
        try:
            daemon.wait(timeout=5)
        except subprocess.TimeoutExpired:
            daemon.kill()
            daemon.wait()
