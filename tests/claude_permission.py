#!/usr/bin/env python3
"""Check mico's permission panel -> PTY -> Claude dialog handoff, no model.

The terminal fixture follows Claude Code 2.1.282's permission dialog: arrows
move the cursor, Tab on the first choice or on "No" opens a line of text for
Claude, text is only accepted there, Enter answers, Esc rejects. It redraws
after every key and fails on keys that arrive before a redraw settled.
Run: python3 tests/claude_permission.py
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

OPTIONS = ['Yes', 'Yes, and always allow access to /tmp/fixture from this project', 'No']


def fixture(root, mode):
    tty.setraw(0)
    cursor, amend, note = 0, False, ''

    def draw():
        out = '\x1b[2J\x1b[H' + '─' * 60 + '\r\n'
        out += ' Bash command\r\n   touch fixture.txt\r\n\r\n Do you want to proceed?\r\n'
        for i, label in enumerate(OPTIONS):
            if amend and i == cursor:
                label = ('Yes, and ' if i == 0 else 'No, ') + (note or 'tell Claude')
            mark = '❯' if i == cursor else ' '
            out += f' {mark} {i + 1}. {label}\r\n'
        out += '\r\n Esc to cancel' + ('' if amend else ' · Tab to amend') + '\r\n'
        os.write(1, out.encode())

    draw()
    (root / 'ready').touch()
    while True:
        key = os.read(0, 1024)
        with (root / 'keys').open('ab') as output:
            output.write(repr(key).encode() + b'\n')
        result = None
        if key == b'\x1b[B':
            cursor, amend, note = min(cursor + 1, len(OPTIONS) - 1), False, ''
        elif key == b'\x1b[A':
            cursor, amend, note = max(cursor - 1, 0), False, ''
        elif key == b'\t':
            if cursor not in (0, len(OPTIONS) - 1):
                raise RuntimeError('Tab on a choice that takes no text')
            amend = True
        elif key == b'\r':
            result = {'choice': cursor + 1, 'note': note}
        elif key == b'\x1b':
            result = {'choice': 'esc', 'note': ''}
        elif amend and key.isascii() and key.decode().isprintable():
            note += key.decode()
        else:
            raise RuntimeError(f'Unexpected key {key!r} (amend={amend})')
        # A real React screen commits after handling the current input event.
        time.sleep(0.06)
        if select.select([0], [], [], 0)[0]:
            raise RuntimeError('Next input arrived before the dialog redrew')
        if result is not None:
            (root / 'result.json').write_text(json.dumps(result))
            os.write(1, b'\x1b[2J\x1b[HMODEL_RESUMED\r\n')
            # Anything typed after the answer: a note sent as a message.
            got = b''
            deadline = time.time() + 3
            while not got.endswith(b'\r') and time.time() < deadline:
                if select.select([0], [], [], 0.2)[0]:
                    got += os.read(0, 1024)
                    os.write(1, b'.')
            (root / 'extra-input.tmp').write_bytes(got)
            (root / 'extra-input.tmp').rename(root / 'extra-input')
            time.sleep(5)
            return
        draw()


def run():
    repo = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix='mico-claude-permission-') as directory:
        base = Path(directory)
        binary = base / 'driver'
        objects = [str(p) for p in (Path(os.environ.get('MICO_BUILD', repo / 'build')) / 'CMakeFiles').glob('mico_*.dir/src/**/*.o')]
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++23', '-O2', '-flto', '-I' + str(repo / 'src'),
                        str(repo / 'tests/claude_permission_driver.cpp'), *objects, '-lutil', '-ldl',
                        '-o', str(binary)], check=True)
        expected = {
            'yes_note': ({'choice': 1, 'note': 'and log it'}, b''),
            'no_note': ({'choice': 3, 'note': 'use gamma'}, b''),
            'always_note': ({'choice': 2, 'note': ''},
                            b'Note on my answer to "Do you want to proceed?": fyi'),
            'click_no': ({'choice': 3, 'note': ''}, b''),
            'escape': ({'choice': 'esc', 'note': ''}, b''),
        }
        for mode, (want, extra) in expected.items():
            root = base / mode
            root.mkdir()
            transcript = root / '.claude/projects/fixture/permission-fixture.jsonl'
            transcript.parent.mkdir(parents=True)
            transcript.write_text(json.dumps({'type': 'assistant', 'message': {'content': [
                {'type': 'tool_use', 'id': 'bash-fixture', 'name': 'Bash',
                 'input': {'command': 'touch fixture.txt'}}]}}) + '\n')
            env = dict(os.environ, HOME=str(root), XDG_CONFIG_HOME=str(root / 'config'))
            env.pop('DISPLAY', None)
            subprocess.run([str(binary), str(Path(__file__).resolve()), str(root), mode],
                           env=env, cwd=root, check=True, timeout=30)
            got = json.loads((root / 'result.json').read_text())
            assert got == want, (mode, got, want)
            typed = (root / 'extra-input').read_bytes() if (root / 'extra-input').exists() else b''
            if extra:
                assert extra in typed and typed.endswith(b'\r'), (mode, typed)
            else:
                assert typed == b'', (mode, typed)
            print(f'Claude permission handoff: {mode} passed', flush=True)


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--fixture':
        fixture(Path(sys.argv[2]), sys.argv[3])
    else:
        run()
