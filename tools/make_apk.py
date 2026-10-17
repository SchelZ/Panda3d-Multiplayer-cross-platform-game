#!/usr/bin/env python3
"""Packs the Android build into an installable, signed APK.

    make_apk.py --sdk SDK --lib libarena.so --abi arm64-v8a --pack arena.pak \\
                --manifest android/AndroidManifest.xml --out PandaArena.apk

Uses the Android SDK's build tools (aapt2, zipalign, apksigner: install
"build-tools" and one "platforms;android-N" with Android Studio's SDK manager
or sdkmanager) and a JDK for apksigner/keytool. The game has no Java code
(NativeActivity), so there is nothing to compile with javac or d8.

Steps: generate the launcher icon (drawn here, no image files), compile it
with aapt2, link the manifest + icon + assets (arena.pak, stored uncompressed)
into an APK, add lib/<abi>/libarena.so, zipalign, sign. Without --keystore a
debug key is made once (like Android Studio's) and reused, so updates install
over each other.
"""

import argparse
import glob
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib


# ----------------------------------------------------------------- the icon

def png(width, height, pixels):
    """pixels: rows of (r, g, b, a) floats 0..1 -> PNG bytes."""
    raw = bytearray()
    for row in pixels:
        raw.append(0)
        for r, g, b, a in row:
            raw += bytes(max(0, min(255, int(c * 255 + 0.5))) for c in (r, g, b, a))

    def chunk(kind, data):
        return (struct.pack('>I', len(data)) + kind + data +
                struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(bytes(raw), 9)) + chunk(b'IEND', b''))


def mix(a, b, t):
    return tuple(x + (y - x) * t for x, y in zip(a, b))


def over(dst, src):
    """Alpha-blends src (r, g, b, a) over dst."""
    a = src[3] + dst[3] * (1 - src[3])
    if a <= 0:
        return (0, 0, 0, 0)
    return tuple((src[i] * src[3] + dst[i] * dst[3] * (1 - src[3])) / a for i in range(3)) + (a,)


SKY_TOP = (0.36, 0.62, 0.93)
SKY_LOW = (0.74, 0.87, 0.96)
GRASS = (0.45, 0.74, 0.33)
GRASS_DARK = (0.32, 0.6, 0.27)
GEM_LIGHT = (0.75, 1.0, 0.97)
GEM_MID = (0.32, 0.88, 0.9)
GEM_DARK = (0.12, 0.55, 0.7)


def gem(x, y):
    """The orb: a faceted diamond around (0, 0), radius ~1. RGBA or None."""
    d = abs(x) * 1.25 + abs(y)
    if d > 1.0:
        glow = max(0.0, 1.0 - (math.hypot(x, y) - 0.7) / 0.6)
        return (0.7, 1.0, 0.95, 0.45 * glow * glow) if glow > 0 else None
    # Four facets lit from the top left.
    if x <= 0 and y >= 0:
        c = GEM_LIGHT
    elif x > 0 and y >= 0:
        c = GEM_MID
    elif x <= 0:
        c = mix(GEM_MID, GEM_DARK, 0.45)
    else:
        c = GEM_DARK
    edge = min(1.0, (1.0 - d) / 0.06)
    return c + (edge,)


def meadow(x, y):
    """Sky and a rolling hill; (x, y) in -1..1, y up."""
    hill = -0.18 + 0.1 * math.cos(x * 2.2 + 0.6)
    if y > hill:
        t = (y - hill) / (1.0 - hill)
        return mix(SKY_LOW, SKY_TOP, min(1.0, t)) + (1.0,)
    t = min(1.0, (hill - y) / 0.9)
    return mix(GRASS, GRASS_DARK, t) + (1.0,)


def render(size, shade, samples=3):
    rows = []
    for py in range(size):
        row = []
        for px in range(size):
            acc = [0.0, 0.0, 0.0, 0.0]
            for sy in range(samples):
                for sx in range(samples):
                    x = ((px + (sx + 0.5) / samples) / size) * 2 - 1
                    y = 1 - ((py + (sy + 0.5) / samples) / size) * 2
                    c = shade(x, y)
                    for i in range(3):
                        acc[i] += c[i] * c[3]
                    acc[3] += c[3]
            n = samples * samples
            a = acc[3] / n
            row.append(tuple(acc[i] / acc[3] if acc[3] > 0 else 0.0 for i in range(3)) + (a,))
        rows.append(row)
    return rows


def gemOver(base, cx, cy, scale):
    def shade(x, y):
        c = base(x, y)
        g = gem((x - cx) / scale, (y - cy) / scale)
        return over(c, g) if g else c
    return shade


def legacyIcon(x, y):
    """A rounded square: the meadow with an orb above the hill."""
    r = 0.22
    qx, qy = max(abs(x) - (1 - r), 0.0), max(abs(y) - (1 - r), 0.0)
    if math.hypot(qx, qy) > r:
        return (0, 0, 0, 0)
    return gemOver(meadow, 0.0, 0.12, 0.52)(x, y)


def adaptiveForeground(x, y):
    """Only the orb, inside the middle 66% that every launcher mask shows."""
    g = gem(x / 0.36, (y - 0.04) / 0.36)
    return g if g else (0, 0, 0, 0)


def adaptiveBackground(x, y):
    return meadow(x, y * 0.8)


def writeIcons(resDir):
    """Writes res/ (legacy and adaptive launcher icons, and the theme); returns the files."""
    files = {
        'mipmap-xxxhdpi/ic_launcher.png': png(192, 192, render(192, legacyIcon)),
        'mipmap-xxxhdpi/ic_launcher_fg.png': png(432, 432, render(432, adaptiveForeground, 2)),
        'mipmap-xxxhdpi/ic_launcher_bg.png': png(432, 432, render(432, adaptiveBackground, 1)),
        'mipmap-anydpi-v26/ic_launcher.xml': (
            b'<?xml version="1.0" encoding="utf-8"?>\n'
            b'<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
            b'    <background android:drawable="@mipmap/ic_launcher_bg" />\n'
            b'    <foreground android:drawable="@mipmap/ic_launcher_fg" />\n'
            b'</adaptive-icon>\n'),
        # The window Android shows while the app starts (and Android 12's own
        # splash behind the icon) in the engine logo's grey, which the game's
        # first frame shows too (client/src/game/SplashScreen.h): no flash.
        'values/theme.xml': (
            b'<?xml version="1.0" encoding="utf-8"?>\n'
            b'<resources>\n'
            b'    <color name="splash_background">#FF252525</color>\n'
            b'    <style name="Theme.Arena" parent="@android:style/Theme.NoTitleBar.Fullscreen">\n'
            b'        <item name="android:windowBackground">@color/splash_background</item>\n'
            b'    </style>\n'
            b'</resources>\n'),
    }
    paths = []
    for rel, data in files.items():
        path = os.path.join(resDir, *rel.split('/'))  # native separators (Windows)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'wb') as f:
            f.write(data)
        paths.append(path)
    return paths


# ---------------------------------------------------------------- the tools

def versionKey(name):
    return [int(p) if p.isdigit() else -1 for p in re.split(r'[.\-]', name)]


def findBuildTools(sdk, wanted):
    if wanted:
        path = os.path.join(sdk, 'build-tools', wanted)
        if not os.path.isdir(path):
            sys.exit('error: no build-tools %s in %s' % (wanted, sdk))
        return path
    versions = [d for d in os.listdir(os.path.join(sdk, 'build-tools'))
                if os.path.isdir(os.path.join(sdk, 'build-tools', d))] \
        if os.path.isdir(os.path.join(sdk, 'build-tools')) else []
    if not versions:
        sys.exit('error: no build-tools in %s (install them with the SDK manager)' % sdk)
    return os.path.join(sdk, 'build-tools', max(versions, key=versionKey))


def findPlatformJar(sdk, wanted):
    if wanted:
        jar = os.path.join(sdk, 'platforms', 'android-%s' % wanted, 'android.jar')
        if not os.path.isfile(jar):
            sys.exit('error: no %s' % jar)
        return jar
    jars = glob.glob(os.path.join(sdk, 'platforms', 'android-*', 'android.jar'))
    if not jars:
        sys.exit('error: no platforms/android-N/android.jar in %s (install one with the SDK manager)' % sdk)
    return max(jars, key=lambda j: versionKey(os.path.basename(os.path.dirname(j))[8:]))


def tool(buildTools, name):
    for candidate in (name + '.exe', name + '.bat', name):
        path = os.path.join(buildTools, candidate)
        if os.path.isfile(path):
            return path
    found = shutil.which(name)
    if found:
        return found
    sys.exit('error: %s not found in %s' % (name, buildTools))


def javaTool(name):
    home = os.environ.get('JAVA_HOME')
    if home:
        for candidate in (name + '.exe', name):
            path = os.path.join(home, 'bin', candidate)
            if os.path.isfile(path):
                return path
    found = shutil.which(name)
    if found:
        return found
    sys.exit('error: %s not found: install a JDK (Android Studio has one in its jbr folder) '
             'and set JAVA_HOME' % name)


def run(cmd, **kwargs):
    result = subprocess.run(cmd, capture_output=True, text=True, **kwargs)
    if result.returncode != 0:
        sys.exit('error: %s failed:\n%s%s' % (os.path.basename(cmd[0]), result.stdout, result.stderr))
    return result


def ensureDebugKeystore(path):
    if os.path.isfile(path):
        return
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    run([javaTool('keytool'), '-genkeypair', '-keystore', path, '-storepass', 'android',
         '-alias', 'androiddebugkey', '-keypass', 'android', '-keyalg', 'RSA', '-keysize', '2048',
         '-validity', '10000', '-dname', 'CN=Android Debug,O=Android,C=US'])
    print('made a debug signing key: %s' % path)


# ------------------------------------------------------------------- main

def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    p.add_argument('--sdk', required=True, help='Android SDK folder (build-tools/, platforms/)')
    p.add_argument('--build-tools', default='', help='build-tools version (default: the newest)')
    p.add_argument('--platform', default='', help='android.jar API level to link against (default: the newest)')
    p.add_argument('--lib', required=True, help='libarena.so')
    p.add_argument('--abi', required=True, help='arm64-v8a, armeabi-v7a, x86_64, ...')
    p.add_argument('--pack', required=True, help='arena.pak')
    p.add_argument('--manifest', required=True)
    p.add_argument('--min-sdk', type=int, default=24)
    p.add_argument('--target-sdk', type=int, default=35)
    p.add_argument('--version-code', type=int, default=1)
    p.add_argument('--version-name', default='1.0')
    p.add_argument('--keystore', default='', help='signing keystore (default: a debug key next to --out)')
    p.add_argument('--ks-pass', default='pass:android', help='apksigner password spec')
    p.add_argument('--key-alias', default='androiddebugkey')
    p.add_argument('--out', required=True, help='the .apk to write')
    args = p.parse_args(argv)

    buildTools = findBuildTools(args.sdk, args.build_tools)
    androidJar = findPlatformJar(args.sdk, args.platform)
    aapt2, zipalign, apksigner = (tool(buildTools, n) for n in ('aapt2', 'zipalign', 'apksigner'))
    for f in (args.lib, args.pack, args.manifest):
        if not os.path.isfile(f):
            sys.exit('error: missing %s' % f)

    with tempfile.TemporaryDirectory(prefix='arena-apk-') as tmp:
        # Resources: the icon.
        resDir = os.path.join(tmp, 'res')
        compiled = os.path.join(tmp, 'compiled')
        os.makedirs(compiled)
        # One aapt2 call for the whole res/ folder: it reads each resource's
        # type from its folder name (compiling files one by one failed on
        # Windows, where the paths mixed \ and /).
        writeIcons(resDir)
        run([aapt2, 'compile', '--dir', resDir, '-o', compiled])
        flats = sorted(glob.glob(os.path.join(compiled, '*.flat')))
        if not flats:
            sys.exit('error: aapt2 compiled no resources from %s' % resDir)

        # The assets: arena.pak, stored (it is encrypted: compressing gains nothing).
        assets = os.path.join(tmp, 'assets')
        os.makedirs(assets)
        shutil.copyfile(args.pack, os.path.join(assets, 'arena.pak'))

        linked = os.path.join(tmp, 'linked.apk')
        cmd = [aapt2, 'link', '-o', linked, '-I', androidJar, '--manifest', args.manifest,
               '--min-sdk-version', str(args.min_sdk), '--target-sdk-version', str(args.target_sdk),
               '--version-code', str(args.version_code), '--version-name', args.version_name,
               '-A', assets, '-0', 'pak', '--auto-add-overlay']
        for flat in flats:
            cmd += ['-R', flat]
        run(cmd)

        # The game itself.
        with zipfile.ZipFile(linked, 'a', zipfile.ZIP_DEFLATED) as apk:
            apk.write(args.lib, 'lib/%s/libarena.so' % args.abi)

        aligned = os.path.join(tmp, 'aligned.apk')
        run([zipalign, '-p', '-f', '4', linked, aligned])

        keystore = args.keystore or os.path.join(os.path.dirname(os.path.abspath(args.out)), 'debug.keystore')
        if not args.keystore:
            ensureDebugKeystore(keystore)
        out = os.path.abspath(args.out)
        run([apksigner, 'sign', '--ks', keystore, '--ks-pass', args.ks_pass, '--ks-key-alias', args.key_alias,
             '--v4-signing-enabled', 'false',
             '--min-sdk-version', str(args.min_sdk), '--out', out, aligned])
        run([apksigner, 'verify', '--min-sdk-version', str(args.min_sdk), out])
    size = os.path.getsize(args.out) / 1e6
    print('wrote %s (%.1f MB, %s, signed with %s)' % (args.out, size, args.abi, os.path.basename(keystore)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
