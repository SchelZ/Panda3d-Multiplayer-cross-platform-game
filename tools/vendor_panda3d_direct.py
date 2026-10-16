#!/usr/bin/env python3
"""Copies Panda3D's DC parser and CConnectionRepository into third_party/.

Why: in Panda3D 1.10 these C++ classes (DCFile, DCPacker, DCClass,
CConnectionRepository, ...) are compiled into the Python extension module
(direct.pyd / direct.so) only, not into libp3direct; see the comment
"These are part of direct.pyd, not libp3direct.dll, because they rely on the
Python libraries. If a C++ user needs these modules, we can move them back and
filter out the Python-specific code." in makepanda/makepanda.py. So a C++ game
using the official SDK has to compile them itself, which is what we do.

The only change made to the sources is that their Python bindings are
switched off: `HAVE_PYTHON` becomes `ARENA_DC_WITH_PYTHON`, which is never
defined. The rest of Panda3D keeps its own HAVE_PYTHON setting, so class
layouts in the SDK's headers stay exactly as the SDK was built.

    python tools/vendor_panda3d_direct.py /path/to/panda3d-1.10.16-source
"""

import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEST = os.path.normpath(os.path.join(HERE, '..', 'third_party', 'panda3d-direct'))

DISTRIBUTED_FILES = [
    'cConnectionRepository.h', 'cConnectionRepository.I', 'cConnectionRepository.cxx',
    'config_distributed.h', 'config_distributed.cxx',
]
PREBUILT = {
    'dcParser.cxx.prebuilt': 'dcParser.cxx',
    'dcParser.h.prebuilt': 'dcParser.h',
    'dcLexer.cxx.prebuilt': 'dcLexer.cxx',
}
SKIP = {'dcParser.yxx', 'dcLexer.lxx'}


def filtered(text):
    return re.sub(r'\bHAVE_PYTHON\b', 'ARENA_DC_WITH_PYTHON', text)


def copy(src, dst):
    with open(src, 'r', encoding='utf-8', newline='') as f:
        text = f.read()
    with open(dst, 'w', encoding='utf-8', newline='') as f:
        f.write(filtered(text))


def main(argv):
    if len(argv) != 2:
        sys.exit(__doc__)
    src = os.path.abspath(argv[1])
    dcparser = os.path.join(src, 'direct', 'src', 'dcparser')
    distributed = os.path.join(src, 'direct', 'src', 'distributed')
    if not os.path.isdir(dcparser) or not os.path.isdir(distributed):
        sys.exit('%s does not look like a Panda3D source tree' % src)

    if os.path.isdir(DEST):
        for name in ('dcparser', 'distributed'):
            shutil.rmtree(os.path.join(DEST, name), ignore_errors=True)
    os.makedirs(os.path.join(DEST, 'dcparser'), exist_ok=True)
    os.makedirs(os.path.join(DEST, 'distributed'), exist_ok=True)

    for name in sorted(os.listdir(dcparser)):
        if name in SKIP:
            continue
        if name in PREBUILT:
            copy(os.path.join(dcparser, name), os.path.join(DEST, 'dcparser', PREBUILT[name]))
        elif os.path.splitext(name)[1] in ('.h', '.I', '.cxx'):
            copy(os.path.join(dcparser, name), os.path.join(DEST, 'dcparser', name))
    for name in DISTRIBUTED_FILES:
        copy(os.path.join(distributed, name), os.path.join(DEST, 'distributed', name))
    shutil.copy(os.path.join(src, 'LICENSE'), os.path.join(DEST, 'LICENSE'))
    print('vendored into %s' % DEST)


if __name__ == '__main__':
    main(sys.argv)
