#!/usr/bin/env python3
"""Fails when a source file includes a header from a layer above its own.

Each directory under src/ is one layer, and may include headers from its own
layer and the layers listed before it. The order is the one CMakeLists.txt
builds its libraries in. Files directly under src/ (main, the tests) sit on
top and may include anything.

    python3 tools/check_layers.py [src-dir]
"""
import pathlib
import re
import sys

# Lowest first. A layer may include itself and anything earlier.
LAYERS = [
    'third_party',
    'base',
    'vt',
    'math',
    'model',
    'adapters',
    'core',
    'term',
    'tui',  # views/ and ui/
    'net',
]
# Directories that build into one library.
LIBRARY = {'views': 'tui', 'ui': 'tui'}
# Pairs that sit side by side: neither may include the other. Math is
# content layout; nothing below the renderers needs to draw.
INDEPENDENT = [{'math', 'vt'}, {'math', 'model'}, {'math', 'adapters'}, {'math', 'core'}]

INCLUDE = re.compile(r'^\s*#\s*include\s+"([a-z_]+)/[^"]+"', re.M)


def layer(d):
    return LIBRARY.get(d, d)


def allowed(src, dst):
    if src == dst:
        return True
    if src not in LAYERS or dst not in LAYERS:
        return False
    if {src, dst} in INDEPENDENT:
        return False
    return LAYERS.index(dst) < LAYERS.index(src)


def main():
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else pathlib.Path(__file__).parent.parent / 'src')
    bad = []
    for f in sorted(root.rglob('*')):
        if f.suffix not in ('.h', '.cpp') or f.parent == root:
            continue
        src = layer(f.relative_to(root).parts[0])
        if src not in LAYERS:
            bad.append(f'{f.relative_to(root)}: directory is in no layer')
            continue
        for n, line in enumerate(f.read_text(errors='replace').splitlines(), 1):
            m = INCLUDE.match(line)
            if m and not allowed(src, layer(m.group(1))):
                bad.append(f'{f.relative_to(root)}:{n}: {src} includes {m.group(1)}/')
    for b in bad:
        print(b)
    if bad:
        print(f'{len(bad)} include(s) cross a layer boundary upward')
        return 1
    print('layers ok')
    return 0


if __name__ == '__main__':
    sys.exit(main())
