#!/usr/bin/env python3
"""Check the full mico form -> PTY -> tool result handoff without a real model.

The terminal fixture follows Claude Code 2.1.278: Enter toggles options in a
multi-select; Next/Submit follows Other; single single-select submits directly.
It rejects combined keys and delays screen updates to expose render races.
Run: python3 tests/claude_questions.py
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


def fixture(root, mode):
    tty.setraw(0)
    transcript = root / '.claude/projects/fixture/question-fixture.jsonl'
    questions = json.loads((root / 'questions.json').read_text())
    answers = [[] for _ in questions]
    qi, cursor = 0, 0
    review = False
    os.write(1, b'FORM READY\r\n')
    (root / 'ready').touch()
    while True:
        key = os.read(0, 1024)
        with (root / 'keys').open('ab') as output:
            output.write(key + b'\n')
        if mode == 'stall':
            continue
        if key not in (b'\x1b[A', b'\x1b[B', b' ', b'\r'):
            raise RuntimeError(f'Burst or unsupported key: {key!r}')
        done = False
        if review:
            done = key == b'\r'
        else:
            q = questions[qi]
            n = len(q['options'])
            multi = q['multiSelect']
            if key == b'\x1b[B':
                cursor = min(cursor + 1, n + int(multi))
            elif key == b'\x1b[A':
                cursor = max(0, cursor - 1)
            elif multi and cursor < n:
                # Both Space and Enter toggle; neither advances.
                if cursor in answers[qi]:
                    answers[qi].remove(cursor)
                else:
                    answers[qi].append(cursor)
            elif key == b'\r' and (not multi or cursor == n + 1):
                if not multi:
                    if cursor >= n:
                        raise RuntimeError('Entered Other instead of a choice')
                    answers[qi] = [cursor]
                if len(questions) == 1 and not multi:
                    done = True
                else:
                    qi += 1
                    cursor = 0
                    review = qi == len(questions)
            else:
                raise RuntimeError('Entered Other instead of Next/Submit')
        # A real React screen commits after handling the current input event.
        time.sleep(0.06)
        if select.select([0], [], [], 0)[0]:
            raise RuntimeError('Next input arrived before the form updated')
        if done:
            (root / 'result.json').write_text(json.dumps(answers))
            with transcript.open('a') as output:
                output.write(json.dumps({'type': 'user', 'message': {'content': [
                    {'type': 'tool_result', 'tool_use_id': 'ask-fixture', 'content': json.dumps(answers)}]}}) + '\n')
                output.write(json.dumps({'type': 'assistant', 'message': {'content': [
                    {'type': 'text', 'text': 'MODEL_RESUMED'}], 'stop_reason': 'end_turn'}}) + '\n')
            os.write(1, b'MODEL_RESUMED\r\n')
            if mode == 'notes':
                # Everything typed after the answer, until a message's Enter.
                got = b''
                deadline = time.time() + 4
                while not got.endswith(b'\r') and time.time() < deadline:
                    if select.select([0], [], [], 0.2)[0]:
                        got += os.read(0, 1024)
                        os.write(1, b'.')  # echo, as a composer would
                (root / 'extra-input.tmp').write_bytes(got)
                (root / 'extra-input.tmp').rename(root / 'extra-input')
            elif select.select([0], [], [], 3)[0]:
                (root / 'extra-input').write_bytes(os.read(0, 1024))
            time.sleep(5)
            return
        os.write(1, f'FRAME {qi} {cursor} {answers}\r\n'.encode())


def run():
    repo = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix='mico-claude-questions-') as directory:
        base = Path(directory)
        binary = base / 'driver'
        objects = [str(p) for p in (Path(os.environ.get('MICO_BUILD', repo / 'build')) / 'CMakeFiles/mico.dir/src').rglob('*.o')
                   if p.name not in ('main.cpp.o', 'selftest.cpp.o', 'regression_test.cpp.o', 'bench.cpp.o')]
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++23', '-O2', '-flto', '-I' + str(repo / 'src'),
                        str(repo / 'tests/claude_question_driver.cpp'), *objects, '-lutil', '-o', str(binary)], check=True)
        for mode in ('single', 'notes', 'multi', 'multi_keyboard', 'mixed', 'two_multi', 'cancel', 'stall'):
            root = base / mode
            root.mkdir()
            questions = [{'question': 'Choose letters', 'header': 'Letters', 'multiSelect': mode not in ('single', 'notes'),
                          'options': [{'label': name} for name in ('Alpha', 'Bravo', 'Charlie')]}]
            if mode in ('mixed', 'two_multi'):
                questions.append({'question': 'Choose another', 'header': 'More', 'multiSelect': mode == 'two_multi',
                                  'options': [{'label': 'Delta'}, {'label': 'Echo'}]})
            (root / 'questions.json').write_text(json.dumps(questions))
            transcript = root / '.claude/projects/fixture/question-fixture.jsonl'
            transcript.parent.mkdir(parents=True)
            transcript.write_text(json.dumps({'type': 'assistant', 'message': {'content': [
                {'type': 'tool_use', 'id': 'ask-fixture', 'name': 'AskUserQuestion', 'input': {'questions': questions}}]}}) + '\n')
            env = dict(os.environ, HOME=str(root), XDG_CONFIG_HOME=str(root / 'config'))
            subprocess.run([str(binary), str(Path(__file__).resolve()), str(root), mode],
                           env=env, cwd=root, check=True, timeout=20)
            if mode not in ('cancel', 'stall'):
                actual = json.loads((root / 'result.json').read_text())
                expected = ([[1]] if mode in ('single', 'notes') else [[0, 2], [1]] if mode == 'mixed'
                            else [[0, 2], [0, 1]] if mode == 'two_multi' else [[0, 2]])
                assert actual == expected, (mode, actual, expected)
            else:
                assert (root / 'keys').read_bytes() == b' \n', 'Delivery continued after cancellation or missing feedback'
            print(f'Claude question handoff: {mode} passed', flush=True)


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--fixture':
        fixture(Path(sys.argv[2]), sys.argv[3])
    else:
        run()
