#!/usr/bin/env python3
"""Builds arena.pak: the client's resources (DC files, shaders) in a Panda3D
Multifile, encrypted with ChaCha20 (RFC 8439).

    python tools/make_pack.py --key HEX64 --out bin/arena.pak \\
        dc/direct.dc=common/direct.dc dc/arena.dc=common/arena.dc shaders/=client/shaders

The client decrypts it in memory and mounts the Multifile in Panda's virtual
file system (client/src/core/ResourcePack.cpp), so nothing readable is ever
written to disk. Panda's own Multifile encryption needs OpenSSL, which the
g++-built Panda3D (and the static Windows .exe) does not include, so the whole
Multifile is encrypted here instead. Standard library only: CMake runs this at
build time. The key is in the client, so this keeps the files from being read
or edited casually; it cannot stop someone who takes the client apart (the
server checks everything players send anyway).

File layout:
    b'APAK', uint8 version (1), 3 zero bytes, 12-byte nonce,
    ChaCha20(key, nonce, counter 1) of the Multifile bytes.
"""

import argparse
import os
import struct
import sys

MAGIC = b'APAK'
VERSION = 1
HEADER = struct.Struct('<4sB3x12s')     # 20 bytes
MULTIFILE_MAGIC = b'pmf\0\n\r'


# ------------------------------------------------------------------ ChaCha20

def _rotl(v, n):
    return ((v << n) & 0xffffffff) | (v >> (32 - n))


def _block(key, counter, nonce):
    """One 64-byte ChaCha20 block (RFC 8439 2.3)."""
    state = [0x61707865, 0x3320646e, 0x79622d32, 0x6b206574]
    state += list(struct.unpack('<8I', key))
    state += [counter & 0xffffffff] + list(struct.unpack('<3I', nonce))
    x = list(state)
    for _ in range(10):
        for a, b, c, d in ((0, 4, 8, 12), (1, 5, 9, 13), (2, 6, 10, 14), (3, 7, 11, 15),
                           (0, 5, 10, 15), (1, 6, 11, 12), (2, 7, 8, 13), (3, 4, 9, 14)):
            x[a] = (x[a] + x[b]) & 0xffffffff
            x[d] = _rotl(x[d] ^ x[a], 16)
            x[c] = (x[c] + x[d]) & 0xffffffff
            x[b] = _rotl(x[b] ^ x[c], 12)
            x[a] = (x[a] + x[b]) & 0xffffffff
            x[d] = _rotl(x[d] ^ x[a], 8)
            x[c] = (x[c] + x[d]) & 0xffffffff
            x[b] = _rotl(x[b] ^ x[c], 7)
    return struct.pack('<16I', *((x[i] + state[i]) & 0xffffffff for i in range(16)))


def chacha20(key, nonce, data, counter=1):
    """Encrypts or decrypts (the same operation) `data`."""
    if len(key) != 32 or len(nonce) != 12:
        raise ValueError('ChaCha20 needs a 32-byte key and a 12-byte nonce')
    out = bytearray(data)
    for offset in range(0, len(out), 64):
        stream = _block(key, counter + offset // 64, nonce)
        chunk = out[offset:offset + 64]
        n = len(chunk)
        word = int.from_bytes(chunk, 'little') ^ int.from_bytes(stream[:n], 'little')
        out[offset:offset + n] = word.to_bytes(n, 'little')
    return bytes(out)


# ----------------------------------------------------------------- Multifile

def multifile(entries):
    """Panda3D Multifile 1.1 bytes (panda/src/express/multifile.cxx) holding
    `entries`, a list of (name, data): header, index chain, then the data."""
    header = MULTIFILE_MAGIC + struct.pack('<hhII', 1, 1, 1, 0)  # version 1.1, scale 1, no timestamp
    indexSize = sum(4 + 4 + 4 + 2 + 4 + 2 + len(name.encode('utf-8')) for name, _ in entries) + 4
    pos = len(header) + indexSize
    index, blobs = [], []
    at = len(header)
    for name, data in entries:
        raw = name.encode('utf-8')
        size = 4 + 4 + 4 + 2 + 4 + 2 + len(raw)
        at += size
        index.append(struct.pack('<IIIHIH', at, pos, len(data), 0, 0, len(raw)) +
                     bytes(b ^ 0xff for b in raw))
        blobs.append(data)
        pos += len(data)
    return header + b''.join(index) + struct.pack('<I', 0) + b''.join(blobs)


def readMultifile(data):
    """{name: data} of a Multifile written by multifile() (for checks)."""
    if data[:6] != MULTIFILE_MAGIC:
        raise ValueError('not a Multifile')
    files = {}
    at = 18
    while True:
        (nextIndex,) = struct.unpack_from('<I', data, at)
        if nextIndex == 0:
            return files
        start, length, flags, _, nameLen = struct.unpack_from('<IIHIH', data, at + 4)
        nameAt = at + 4 + 4 + 4 + 2 + 4 + 2
        name = bytes(b ^ 0xff for b in data[nameAt:nameAt + nameLen]).decode('utf-8')
        files[name] = data[start:start + length]
        at = nextIndex


# ---------------------------------------------------------------------- pack

def pack(entries, key, nonce=None):
    nonce = nonce if nonce is not None else os.urandom(12)
    return HEADER.pack(MAGIC, VERSION, nonce) + chacha20(key, nonce, multifile(entries))


def unpack(blob, key):
    magic, version, nonce = HEADER.unpack_from(blob)
    if magic != MAGIC or version != VERSION:
        raise ValueError('not an arena.pak (version %d)' % VERSION)
    plain = chacha20(key, nonce, blob[HEADER.size:])
    if plain[:6] != MULTIFILE_MAGIC:
        raise ValueError('wrong key, or the pack is damaged')
    return readMultifile(plain)


def collect(specs):
    """NAME=FILE, or DIR/=FOLDER for every file in a folder (sorted)."""
    entries = []
    for spec in specs:
        name, _, path = spec.partition('=')
        if not path:
            raise SystemExit('expected NAME=PATH, got %r' % spec)
        if name.endswith('/'):
            for fn in sorted(os.listdir(path)):
                full = os.path.join(path, fn)
                if os.path.isfile(full) and not fn.startswith('.'):
                    with open(full, 'rb') as f:
                        entries.append((name + fn, f.read()))
        else:
            with open(path, 'rb') as f:
                entries.append((name, f.read()))
    return entries


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    p.add_argument('--key', required=True, help='64 hex digits')
    p.add_argument('--out', required=True)
    p.add_argument('files', nargs='+', help='NAME=FILE or DIR/=FOLDER')
    args = p.parse_args(argv)
    key = bytes.fromhex(args.key)
    if len(key) != 32:
        raise SystemExit('--key must be 64 hex digits (32 bytes)')
    entries = collect(args.files)
    blob = pack(entries, key)
    if unpack(blob, key) != dict(entries):  # never ship a pack we cannot read back
        raise SystemExit('pack check failed')
    tmp = args.out + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(blob)
    os.replace(tmp, args.out)
    print('%s: %d files, %d bytes' % (args.out, len(entries), len(blob)))


if __name__ == '__main__':
    sys.exit(main())
