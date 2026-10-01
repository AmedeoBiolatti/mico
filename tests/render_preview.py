#!/usr/bin/env python3
"""Render mico's actual cells as PNGs, using isolated synthetic chats.

Usage: python3 tests/render_preview.py artifacts/design/after
Requires Pillow for rendering only; no real coding agent is started.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from PIL import Image, ImageDraw, ImageFont

repo = Path(__file__).resolve().parent.parent
out = Path(sys.argv[1] if len(sys.argv) > 1 else 'artifacts/design/preview').resolve()
out.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix='mico-ui-preview-') as directory:
    root = Path(directory)
    binary = root / 'snapshot'
    objects = [str(p) for p in (repo / 'build/CMakeFiles').glob('mico_*.dir/src/**/*.o')]
    subprocess.run(['c++', '-std=c++23', '-O2', '-I' + str(repo / 'src'),
                    str(repo / 'tests/ui_snapshot.cpp'), *objects, '-lutil', '-o', str(binary)], check=True)
    project = root / 'workspace/mico'
    other = root / 'workspace/website'
    project.mkdir(parents=True)
    other.mkdir(parents=True)
    (root / 'config/mico').mkdir(parents=True)
    (root / 'config/mico/folders').write_text(str(project) + '\n' + str(other) + '\n')
    (root / 'bin').mkdir()
    agent = root / 'bin/codex'
    agent.write_text('#!/bin/sh\nexec /bin/cat\n')
    agent.chmod(0o755)
    sessions = root / '.codex/sessions/2026/09/21'
    sessions.mkdir(parents=True)
    title = 'Polish the chat experience'
    session_id = '11111111-1111-4111-8111-111111111111'
    records = [
        {'type': 'session_meta', 'payload': {'id': session_id, 'cwd': str(project)}},
        {'type': 'turn_context', 'payload': {'model': 'gpt-5.6', 'effort': 'high', 'approval_policy': 'on-request'}},
        {'type': 'response_item', 'payload': {'type': 'message', 'role': 'user', 'content': [{'text': 'Make the chat list easier to scan and let me pick up a conversation with one click.'}]}},
        {'type': 'response_item', 'payload': {'type': 'message', 'role': 'assistant', 'content': [{'text': "I'll check how the sidebar and session lifecycle fit together, then update the opening flow."}]}},
        {'type': 'response_item', 'payload': {'type': 'function_call', 'call_id': 'read', 'name': 'read', 'arguments': 'src/views/chat_list.cpp'}},
        {'type': 'response_item', 'payload': {'type': 'function_call_output', 'call_id': 'read', 'output': 'Read 420 lines'}},
        {'type': 'response_item', 'payload': {'type': 'message', 'role': 'assistant', 'channel': 'final', 'content': [{'text': '## Ready to pick up where you left off\n\nYour conversations now have a clearer title and a separate line for the agent and status.\n\n- **One click** opens and resumes a saved chat.\n- Running chats reconnect to the existing agent.\n- Your draft stays with its conversation.\n\nThe changes are in `chat_list.cpp` and `session.cpp`.\n\n```cpp\nopen_chat(session);\n```\n\nAll session and input checks passed.'}]}},
    ]
    (sessions / 'rollout-primary.jsonl').write_text(''.join(json.dumps(r, separators=(',', ':')) + '\n' for r in records))
    names = [{'id': session_id, 'thread_name': title}]
    for i, name in enumerate(['Fix transcript discovery', 'Investigate tool status', 'Tidy up keyboard shortcuts'], 2):
        sid = f'00000000-0000-4000-8000-{i:012d}'
        path = sessions / f'rollout-{i}.jsonl'
        path.write_text(json.dumps({'type': 'session_meta', 'payload': {'id': sid, 'cwd': str(project)}}) + '\n')
        modified = time.time() - (5 * 60, 2 * 3600, 86400)[i - 2]
        os.utime(path, (modified, modified))
        names.append({'id': sid, 'thread_name': name})
    (root / '.codex/session_index.jsonl').write_text(''.join(json.dumps(n) + '\n' for n in names))
    env = dict(os.environ, HOME=str(root), XDG_CONFIG_HOME=str(root / 'config'),
               PATH=str(root / 'bin') + ':' + os.environ.get('PATH', ''))
    fonts = '/usr/share/fonts/truetype/dejavu/'
    regular = ImageFont.truetype(fonts + 'DejaVuSansMono.ttf', 16)
    bold = ImageFont.truetype(fonts + 'DejaVuSansMono-Bold.ttf', 16)
    italic = ImageFont.truetype(fonts + 'DejaVuSansMono-Oblique.ttf', 16)
    cw, ch = 10, 22
    for w, h, mode in [(120, 36, 'stored'), (120, 36, 'live'), (80, 24, 'live'), (48, 18, 'live'),
                       (120, 36, 'working'), (48, 18, 'working'), (80, 24, 'prompt')]:
        cells = subprocess.check_output([str(binary), str(w), str(h), mode], env=env, cwd=project).decode().splitlines()[1:]
        image = Image.new('RGB', (w * cw, h * ch), '#11131a')
        draw = ImageDraw.Draw(image)
        for i, line in enumerate(cells):
            cp, fg, bg, attrs, width = map(int, line.split())
            x, y = i % w * cw, i // w * ch
            fg = fg if fg >= 0 else 0xc8ceda
            bg = bg if bg >= 0 else 0x11131a
            if attrs & 16:
                fg, bg = bg, fg
            color = lambda c: ((c >> 16) & 255, (c >> 8) & 255, c & 255)
            draw.rectangle((x, y, x + cw - 1, y + ch - 1), fill=color(bg))
            if width and cp != 32:
                font = bold if attrs & 1 else italic if attrs & 4 else regular
                draw.text((x, y + 1), chr(cp), font=font, fill=color(fg))
        target = out / f'{mode}-{w}x{h}.png'
        image.save(target)
        print(target)
