"""Tests of tools/make_pack.py, which builds the client's arena.pak (a Panda3D
Multifile encrypted with ChaCha20; the C++ side is client/src/core).

    cd server && python -m unittest tests.test_pack -v
"""

import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ARENA = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ARENA, 'tools'))

import make_pack  # noqa: E402
from panda3d.core import Filename, Multifile  # noqa: E402

KEY = bytes(range(32))


class PackTest(unittest.TestCase):
    def test_chacha20_matches_rfc8439(self):
        nonce = bytes.fromhex('000000000000004a00000000')
        text = (b"Ladies and Gentlemen of the class of '99: If I could offer you only one "
                b"tip for the future, sunscreen would be it.")
        out = make_pack.chacha20(KEY, nonce, text, counter=1)
        self.assertEqual(out[:16].hex(), '6e2e359a2568f98041ba0728dd0d6981')
        self.assertEqual(out[-8:].hex(), '8eedf2785e42874d')
        self.assertEqual(make_pack.chacha20(KEY, nonce, out, counter=1), text)

    def test_panda3d_reads_the_multifile(self):
        with open(os.path.join(ARENA, 'common', 'arena.dc'), 'rb') as f:
            dc = f.read()
        entries = [('dc/arena.dc', dc), ('shaders/x.frag', b'void main() {}\n'), ('empty', b'')]
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, 'test.mf')
            with open(path, 'wb') as f:
                f.write(make_pack.multifile(entries))
            mf = Multifile()
            self.assertTrue(mf.openRead(Filename.fromOsSpecific(path)))
            self.assertEqual(mf.getNumSubfiles(), 3)
            for name, data in entries:
                index = mf.findSubfile(name)
                self.assertGreaterEqual(index, 0, name)
                self.assertEqual(bytes(mf.readSubfile(index)), data)
            mf.close()

    def test_pack_round_trip_and_wrong_key(self):
        entries = [('dc/direct.dc', b'dclass X {};\n'), ('shaders/a.vert', b'#version 150\n')]
        blob = make_pack.pack(entries, KEY)
        self.assertNotIn(b'dclass', blob)  # not readable as it is
        self.assertEqual(make_pack.unpack(blob, KEY), dict(entries))
        with self.assertRaises(ValueError):
            make_pack.unpack(blob, bytes(32))
        # Every pack gets its own nonce: the same files never look the same.
        self.assertNotEqual(make_pack.pack(entries, KEY)[20:], blob[20:])


if __name__ == '__main__':
    unittest.main()
