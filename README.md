# Panda Arena

A small online arena game built on **Panda3D 1.10.16** (latest release) and its
[distributed networking](https://docs.panda3d.org/1.10/python/programming/networking/distributed/index)
system. The client is **C++**, compiled with **g++** on Linux and Windows (MinGW-w64)
and with the Android NDK's clang for **Android** phones and tablets, and the dedicated
server is **Python**.

![Panda Arena](docs/screenshot.jpg)
<img width="1601" height="901" alt="image" src="https://github.com/user-attachments/assets/66adf704-797f-472c-a5a2-f37faf74fe5a" />


*The Windows client (MinGW-w64 g++ build, shown here running under Wine with software
OpenGL) against the Python server and three bots.*

A sunny meadow in a forest valley: collect glowing gems (mint = 1 point, gold = 5), and
dash into other players to steal 2 of their points. When the round timer runs out, the highest score wins, and a new
round starts a few seconds later. Up to 16 players per server (configurable).

---

## Quick start

```bash
# Server (any OS with Python 3.8+)
pip install -r server/requirements.txt           # panda3d==1.10.16
python server/arena_server.py                    # listens on 0.0.0.0:4400

# Client (after building, see below)
build/linux/bin/arena_client --host <server-ip> --name Alice        # Linux
build\mingw\bin\arena_client.exe --host <server-ip> --name Alice   # Windows
adb install -r build/android-arm64/bin/PandaArena.apk              # Android
```

Open TCP port 4400 on the server's firewall (or pick another with `--port`), and UDP 4400
so phones and PCs on the same network find the server by themselves. Every player runs
the client against the same host.

| Key | Action |
|---|---|
| WASD / arrows | move |
| Space | dash (1.6 s cooldown) |
| Enter | chat (Enter sends, Esc cancels) |
| Mouse wheel | zoom |
| F1 / F3 / F11 | help / network statistics / fullscreen |
| Esc | quit |

On a touch screen (Android, or `--touch` on a PC with the mouse as the finger): the stick
sits in the bottom-left corner and stays there; put your thumb on it and push the knob
(direction only: you always run at full speed, like with the keys). The DASH button in
the bottom-right corner dashes when the button itself is touched, and shows the cooldown.
Two fingers elsewhere on the right half pinch to zoom; tapping the status line shows
network statistics; Back returns to the server list.

---

## Building the client

The client is built with **g++** on both systems. CMake downloads the Panda3D 1.10.16
source and compiles the parts the game uses (core, OpenGL, the window system, the
framework) with the same g++, then links them **statically**. No Panda3D SDK is
needed, and the result is one self-contained executable (~10 MB stripped) per platform.
The first build takes a few minutes; later builds only recompile the game.

| | Linux | Windows |
|---|---|---|
| Compiler | g++ 12 or newer (C++20; tested: 13) | MinGW-w64 g++ 12 or newer (tested: 13, 16.1) |
| Window / GL | X11, GLX | Win32, WGL |
| Executable needs | libGL, libX11 (and FreeType, if the system's is used) | only DLLs that ship with Windows |

### Linux (g++)

```bash
sudo apt install build-essential cmake ninja-build git libgl-dev libx11-dev libfreetype-dev
cmake --preset linux-gcc
cmake --build --preset linux-gcc
ctest --preset linux-gcc            # Python tests need: pip install -r server/requirements.txt
build/linux/bin/arena_client --host 192.168.1.20
```

### Windows (MinGW-w64 g++)

You need a **current MinGW-w64 g++ (12 or newer; tested: 13 and 16.1)**, CMake, Ninja, Git
and Python 3. Older toolchains such as the GCC 11.1 bundled with Nim fail inside their own
headers, so CMake stops with a message if it finds one.

**WinLibs (recommended: no installer, one folder, includes CMake and Ninja).**
1. On [winlibs.com](https://winlibs.com), under *Release versions* → *UCRT runtime*,
   download **GCC 16.1.0 (with POSIX threads) + MinGW-w64 14.0.0 (UCRT), Win64**, the
   archive *without LLVM/Clang/LLD/LLDB*: the .zip (about 270 MB; Windows opens it) or
   the .7z (110 MB; needs 7-Zip). Direct link to the .zip:
   <https://github.com/brechtsanders/winlibs_mingw/releases/download/16.1.0posix-14.0.0-ucrt-r3/winlibs-x86_64-posix-seh-gcc-16.1.0-mingw-w64ucrt-14.0.0-r3.zip>
2. Extract it anywhere, for example `C:\winlibs` (`tar -xf winlibs.zip -C C:\winlibs` in
   `cmd.exe`), which gives `C:\winlibs\mingw64\bin\g++.exe`. A folder with spaces such as
   `C:\Program Files\...`, or over Nim's MinGW (`C:\Program Files\nim-2.2.2_64\dist\mingw64`),
   works too: GCC cannot link from a path with a space on its own (`ld: cannot find
   C:/Program`), so CMake then links with a small specs file that fixes it
   (`cmake/MinGWPathWithSpaces.cmake`; the configure output says so).
3. Put its `mingw64\bin` on the PATH **before** any other MinGW (if you extracted over Nim's
   MinGW, it already is, and Nim uses this GCC too):
   `Win+R` → `sysdm.cpl` → *Advanced → Environment Variables* → *System variables* →
   `Path` → *Edit* → *New*, then *Move up* to the top. System entries come before user
   entries. Or for one `cmd.exe` session: `set PATH=C:\winlibs\mingw64\bin;%PATH%`.
4. Install [Git for Windows](https://git-scm.com/download/win) and
   [Python 3](https://www.python.org/downloads/) (tick *Add python.exe to PATH*).
5. In a **new** terminal, check: `where g++` lists that folder first and
   `g++ --version` says 16.1.0.

**MSYS2 (alternative).** Install [MSYS2](https://www.msys2.org), open the **UCRT64** shell
and run
`pacman -S --needed git mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-python`.

Then, in the `arena` folder (after switching compilers, delete the old `build\mingw`
folder first: CMake remembers the compiler it found):

```bat
rmdir /s /q build\mingw
cmake --preset windows-mingw
cmake --build --preset windows-mingw
ctest --preset windows-mingw
build\mingw\bin\arena_client.exe --host <local_ip>
```

### Windows .exe from Linux (cross-compile)

```bash
sudo apt install g++-mingw-w64-x86-64-posix wine    # wine only to run the tests
cmake --preset windows-mingw-cross
cmake --build --preset windows-mingw-cross
ctest --preset windows-mingw-cross                  # runs the .exe files through Wine
```

### Android (phones and tablets)

The same game runs on Android 7.0 (API 24) and newer with OpenGL ES 3. The whole client,
Panda3D included, is one library, `libarena.so`, that Android's NativeActivity runs; there
is no Java code. It is built with the same CMake and the Android NDK's **clang**: Google
removed GCC from the NDK in 2019 (r18), so g++ is not an option there.

What is different on a phone:

* **Renderer:** Panda3D's OpenGL ES renderer (`gles2gsg`) in its Android window
  (`androiddisplay`). makepanda only builds that window for OpenGL ES 1, so
  `cmake/Panda3DFromSource.cmake` builds it for ES 2/3 itself. Patches 0003 and 0004
  (`cmake/panda3d-patches/`) fix what that needed: shadow-map depth textures on ES, keeping
  the GL context while the app is in the background, asking for an ES 3 context,
  `ALooper_pollAll` (gone from current NDKs), and a failed internal check logging instead
  of closing the app. The shaders are the same files: on OpenGL ES the GLSL 1.50 source is
  compiled as GLSL ES 3.00, which only needs another version line and precisions. The
  shadow map is 1024².
* **Controls:** the touch controls above, sized for the screen (the 2D interface grows on
  small screens).
* **Connect screen:** servers on the same network appear by themselves. A probe on UDP
  4400 is answered by `server/arena/discovery.py`; replies are never larger than the probe
  and are rate-limited, so the port cannot amplify floods. Otherwise type the address on the
  keypad (`192.168.1.20` or `host:port`). The last server and your name are remembered.
* **Resources:** `arena.pak` ships inside the APK (`assets/`) and is decrypted in memory
  like on the PC.
* **In the background** (Home, the app switcher) the game stops drawing as soon as Android
  pauses it, before the window's surface goes away, and stays connected; it draws again
  when it is back on screen.

What you need, once (easiest with [Android Studio](https://developer.android.com/studio):
*Tools → SDK Manager*):

* *SDK Platforms:* one Android version, e.g. Android 15 (API 35);
* *SDK Tools:* **NDK (Side by side)** r27 or newer, **Android SDK Build-Tools**,
  **Android SDK Platform-Tools** (adb);
* a JDK 17+ for signing: Android Studio has one in its `jbr` folder;
* CMake, Ninja, Python 3 and Git, as for the PC builds.

Without Android Studio, the [command-line tools](https://developer.android.com/studio#command-line-tools-only)
install the same: `sdkmanager "platforms;android-35" "build-tools;35.0.0" "ndk;27.2.12479018" "platform-tools"`.

```bat
:: Windows (cmd.exe)
set ANDROID_HOME=%LOCALAPPDATA%\Android\Sdk
set JAVA_HOME=C:\Program Files\Android\Android Studio\jbr
cmake --preset android-arm64
cmake --build --preset android-arm64
adb install -r build\android-arm64\bin\PandaArena.apk
```

On Linux it is the same with `export ANDROID_HOME=~/Android/Sdk` and so on. The newest
NDK in `%ANDROID_HOME%\ndk\` (by the version in its `source.properties`, whatever the
folder is called) is used (`cmake/toolchains/android.cmake`; the configure output names
it); set `ANDROID_NDK_HOME` to pick another. Android builds compile at `-O2` without debug
info rather than the NDK's `-O3 -g`. If the NDK's clang itself crashes ("PLEASE submit a
bug report", `Exception Code: 0xC0000005`), as NDK r27d's clang 18.0.4 did on Windows on
FreeType files, install a newer NDK with the SDK Manager, delete `build\android-arm64` and
configure again.

* The build makes `bin/PandaArena.apk` (`tools/make_apk.py`: aapt2, zipalign, apksigner;
  the launcher icon is drawn by the script). It is signed with a debug key made on the first
  build, `bin/debug.keystore`. Keep it: Android only installs an update signed with the
  same key.
* Install with `adb install -r` (enable *Developer options → USB debugging* on the phone),
  or copy the APK to the phone and open it there (allow installing unknown apps).
* Play: run the server on a PC on the same Wi-Fi, with TCP and UDP 4400 allowed in its
  firewall. The phone lists it; tap it.
* Logs: `adb logcat -s PandaArena Panda3D`.
* `--preset android-x86_64` builds for the emulator and Chromebooks. For a store release,
  sign with your own key: `-DARENA_ANDROID_KEYSTORE=release.keystore
  -DARENA_ANDROID_KEY_ALIAS=<alias>`, with the password in the `ARENA_KEYSTORE_PASS`
  environment variable.
* Try the phone version on a PC: `arena_client --touch` shows the connect screen and the
  touch controls (the mouse is the finger, Esc is Back). `--preset linux-gles` builds the
  Linux client with the OpenGL ES renderer Android uses (EGL; Mesa runs it everywhere).

### Build notes

* `cmake/Panda3DFromSource.cmake` mirrors the file lists, defines and flags of Panda3D's
  own `makepanda.py` (`-std=gnu++11 -fno-exceptions -ffast-math -O2 ...`). Panda3D's
  build system has no MinGW target, so `cmake/panda3d-patches/0001-mingw-w64.patch` adds
  the five small portability fixes it needs (e.g. `dlfcn.h`, `msvcrt` time zone,
  duplicate XInput declarations); it is applied automatically.
* `-DPANDA3D_SOURCE_DIR=/path/to/panda3d` reuses an existing 1.10.16 checkout instead of
  downloading one. FreeType is taken from the system on Linux and otherwise built from
  source (`-DARENA_PANDA3D_SYSTEM_LIBS=ON/OFF` to choose).
* Compiler options follow the main project's rules (`docs/GUIDE.md` §6,
  `cmake/CompilerOptions.cmake`). The game code uses `-O3`, LTO, the safe fast-math subset
  (`ARENA_FAST_MATH`, which keeps NaN/Inf) and `-fno-plt`. `-DARENA_X86_64_V3=ON` targets
  AVX2/FMA (CPUs since 2013). It is off by default so the `.exe` runs everywhere.
  Profile-guided optimisation: configure with `-DARENA_PGO=GEN`, build, play or run
  `arena_client --bot` for a few minutes, then reconfigure with `-DARENA_PGO=USE` and
  build again.
* `--preset linux-gcc-sdk` links an installed Panda3D C++ SDK instead (the
  [.deb](https://www.panda3d.org/download/) or a `makepanda` build; `-DPANDA3D_ROOT=...`).
  The official Windows SDK is compiled with MSVC, whose C++ ABI g++ cannot link against,
  so on Windows Panda3D is always built from source.
* The build packs the DC files and shaders into `bin/arena.pak` (see *Resources*), which
  needs Python 3 (standard library only). Without Python the files are copied next to the
  client unencrypted instead.

### Resources: an encrypted Multifile

The client's resources (`common/direct.dc`, `common/arena.dc`, `client/shaders/*`) ship
in `arena.pak` next to the executable, never as readable files. `tools/make_pack.py`
writes them into a Panda3D [Multifile](https://docs.panda3d.org/1.10/python/programming/advanced-loading/multifiles)
and encrypts the whole Multifile with ChaCha20 (RFC 8439). At start-up the client
decrypts it in memory and mounts it read-only at `/arena` in Panda's virtual file system
(`client/src/core/ResourcePack.cpp`). The DC parser and the shader loader then read
`/arena/dc/arena.dc` and so on like ordinary files, and nothing decrypted is ever written to
disk. A pack made with another key, or a damaged one, is refused with a clear message.

* Panda's own Multifile encryption (`set_encryption_flag`) needs OpenSSL, which the
  g++-built Panda3D, and the static Windows `.exe` in particular, does not include. So the
  Multifile is encrypted as a whole instead, with the same code in C++ and Python, both
  checked against the RFC's test vector.
* The key: `-DARENA_PACK_KEY=<64 hex digits>` (e.g. one key for all your releases). By
  default every build folder gets its own random key, kept in its CMake cache. The
  executable holds it as key XOR mask plus mask, so it is not one string in the binary.
* What this protects: the files cannot be read or edited casually, and changing them breaks
  the client. It cannot stop someone who takes the executable apart with a debugger. No
  client-side encryption can, because the client must decrypt to run. It does not need to,
  either: the server checks the DC hash at login and validates everything players send. The
  server reads the plain DC files, which stay on the server.
* `--pack FILE` loads another pack; `--dc-dir DIR` loads plain DC files (development).

### Panda3D settings (PRC)

The client builds its Panda3D configuration in (`configure()` in `client/src/app/App.cpp`)
and sets only what changes something for this game. Most performance variables are
already at their best by default (state and transform caches, vertex buffers, pre-munged
vertex data, deferred state garbage collection). The model, texture-loading and audio
variables do not apply: the world is generated at start-up from code, with no model or
texture files.

| Setting | Why |
|---|---|
| `color-bits 8 8 8`, `depth-bits 24`, `framebuffer-stencil #f` | never a 16-bit screen (banding in the sky) or a 16-bit depth buffer (flickering distant hills), which phones may pick unless asked; no stencil, it is not used |
| near plane 1 m (camera lens) | twice the depth precision of 0.5 m; the camera is always 15 m or more from what it shows |
| `gl-validate-shaders #f` (on with `--verbose`) | validation only prints diagnostics, and stalls the first frame |
| `text-minfilter linear_mipmap_linear`, `text-page-size 512 512` | small HUD text stays crisp instead of shimmering, and all its glyphs fit on one or two textures (fewer draw calls) |
| `sleep-precision 0` with `clock-mode limited` | headless clients and phones sleep between frames instead of spinning the last 10 ms of each |
| Android: one window attempt with `BF_fb_props_optional` | the closest framebuffer config is taken (a phone without 24-bit depth still opens); Panda's Android window cannot be opened a second time after a failed attempt |
| Android: `clock-frame-rate 60`, no MSAA | 90/120 Hz phones draw half the frames, staying cool and at full clock speed; phone pixels are tiny, and multisampled EGL configs are where emulators such as BlueStacks misbehave |

OpenGL ES builds (Android, `linux-gles`) set `load-display pandagles2` with
`aux-display pandagles` and `aux-display p3tinydisplay` as fallbacks, as Panda3D's Android
guide recommends (`pandagles2` is the renderer for shaders). The game always draws with
`pandagles2`: Panda3D is compiled into the client, the OpenGL ES 2/3 renderer
(androiddisplay with gles2gsg; egl on Linux) is the only renderer compiled into those builds,
and it registers itself while the program loads, so Panda never looks for plugin files.
`pandagles` (OpenGL ES 1) and `p3tinydisplay` (software) are not in the Android build at all.
The log shows which renderer and framebuffer the game got (`renderer:` and `window opened`).
Desktop OpenGL builds name no `load-display`: in a Panda3D SDK build it would load the OpenGL
ES plugin, which the desktop GLSL shaders do not run on.

`threading-model` (a separate cull/draw thread) is marked experimental in 1.10 and stays
off; it would also add a frame of input delay.

### Graphics

The world is generated at start-up as low-poly, flat-shaded geometry with vertex colours
(`client/src/game/Scenery.cpp`, no model files). It has a sunny meadow with paths,
pebbles and flowers; mossy boulders and an old stump with mushrooms (the obstacles); and
a bank of ferns, bushes and stones at the edge. Around it is a forest valley of broadleaf
and pine trees and rocky outcrops on rising hills. The side toward the camera only has low
plants, so nothing hides the players. All of it is two vertex buffers (about 283,000
vertices, two draw calls), built in about 20 ms.

GLSL shaders (`client/shaders`, loaded from the pack), in the spirit of
[3D Game Shaders For Beginners](https://github.com/lettier/3d-game-shaders-for-beginners):
* **world**:
  * a soft, slightly banded sun with **shadow maps** (Panda's directional-light shadow
    camera, sampled with 4-tap PCF);
  * sky/ground **hemisphere ambient**;
  * **cloud shadows** drifting across the ground;
  * a **rim light** that separates shapes;
  * **wind** that sways grass, ferns, flowers and leaves (per-vertex weights);
  * **distance haze** into the horizon colour;
  * lighting in linear colour with a soft highlight shoulder.
* **sky**: a dome with a blue-to-haze gradient, a sun with glow, and slow fbm clouds.

The players are blocky characters with a visor and eyes, in their colour. Their walk cycle
comes from how fast they are drawn moving: legs and arms swing, and the body bobs and
leans into the run. Dashes stretch them and kick up dust, and hits flash them. Their bodies
turn toward where they move like the local player's does. Gems float, spin and glow.
Butterflies flutter over the meadow and pollen drifts in the sun.

Performance choices (as in the main project): static scenery is batched into two vertex
buffers filled with one copy each (non-indexed); thin things (grass, effects, labels) are
hidden from the shadow camera, and the shadow pass renders depth only without the shaders;
the shadow frustum covers just the arena and the trees around it (2048² by default); per
frame only the players, gems and a few dozen particles move.

Options: `--zoom 0.55..1.6` sets the start view (closer is lower, like over a shoulder;
the mouse wheel changes it), `--no-shadows` keeps the shaders without the shadow map, and
`--low` uses fixed-function lighting (the same scene, no shadows). If the driver has no
GLSL, or the shaders do not compile on it, the client falls back to fixed-function by itself.
Window creation falls back from 4x MSAA, to no MSAA, to a software OpenGL framebuffer (VMs
and remote desktops).

---

## How the networking works

The game follows the manual's architecture: one **ServerRepository**, one **AI repository**
(a `ClientRepository` with `dcSuffix='AI'`, see *AI Repositories*), and client repositories.

```
 C++ client ──┐                               ┌─────────── arena_server.py (one process) ─────────┐
 C++ client ──┼── TCP 4400 ──▶ ArenaServerRepository ◀── loopback ──▶ ArenaAIRepository         │
 Python bot ──┘    (relay, validation, limits)  │                       TimeManagerAI     zone 1 │
                                                │                       DistributedArenaGameAI 2 │
                                                └────────────────────────────────────────────────┘
```

* **DC files** (`common/direct.dc` pinned from 1.10.16, `common/arena.dc`) define the
  distributed classes. Server, AI, C++ and Python clients load the same two files in the
  same order. The DC hash is checked during the handshake.
* **Connect flow** (as in *Client Repositories*): connect → hello → `SET_DOID_RANGE` →
  interest in zone 1 → the `TimeManager` syncs the clock → interest in zones 1+2 → the
  client sees `DistributedArenaGame` and creates its own `DistributedArenaPlayer`.
* **Players** (`DistributedArenaPlayer`) are owned by their client and have just two DC
  fields, name and color, sent once. Each client simulates its own movement at a fixed
  120 Hz step (instant response). Movement itself travels as compact binary messages (see
  *Wire format*), and every player is named on the wire by a one-byte **slot** that the
  server assigns. Network timestamps come from a C++ port of `ClockDelta`.
* **Input-aware prediction** (`predictMotion` in `client/src/game/Movement.cpp`). Every
  movement sample carries position, velocity **and the movement input being held**.
  Movement is deterministic (velocity moves toward input × max speed at a fixed
  acceleration; a dash holds its velocity for 27 physics steps), so from one sample everyone
  predicts the acceleration, the cruising and the braking exactly, in closed form, down
  to the 120 Hz step's rounding. A dash sample is rewound to the dash's start, so
  receivers know exactly when it ends.
* **Dead reckoning** (`client/src/game/DeadReckoning.cpp`). The sender runs the same
  prediction and sends only when it would go wrong: it is already more than 12 cm or
  1 m/s off, or would be within the next 0.15 s. That last case is any change of input:
  pressing, releasing, turning. So a short tap costs exactly two samples (press, release), a
  straight run a heartbeat every 0.5 s, and standing still nothing. Samples go out at most
  20 times a second.
* **Drawing remote players** (`client/src/game/RemoteMotion.cpp`). Remote players are
  drawn a little in the past, by a **view delay** of about the 90th percentile of how
  late their samples arrive (it adapts slowly, 20–250 ms). So a sample usually arrives
  before the drawing reaches its time, and takes over at exactly that time. Stops,
  reversals, taps and dashes are drawn when and where they happened: no overshoot and no
  sliding back. A sample later than that takes over at once. Either way the drawing never
  jumps: the position error left at the switch fades out with a smooth cubic, in at least
  0.1 s and never faster than 8 m/s. Only errors over 3 m (respawns) snap.
* **Rotation.** Players face where they move, so heading is not sent. Every body, the
  local player's included, turns toward its drawn direction of motion quickly but at most
  900°/s (`turnToward`), so a reversal is a short spin instead of a snap.
* **The AI is authoritative** for everything that scores. It sees every movement sample,
  rejects impossible movement (speed budget on arrival times, bounds) and tells the owner
  to snap back (`correctPosition`). It detects orb pickups along each movement segment and
  sends `knockback`/`pong` as targeted updates to one client. Late joiners get the game
  state from the `required`/`ram` fields (`REQUEST_GENERATES`), and every player's slot
  and last movement sample from the server's cache, before the players' generates arrive.
* **Lag-compensated hits** (`DistributedArenaGameAI._checkDashHits`). A dash hits whoever
  it touched *on the attacker's screen*. The AI keeps one second of each player's samples
  on that player's own timeline (the timestamp in every sample). It replays each dash in
  10 ms steps against the victim as the attacker drew it (`server/arena/motion.py`, a
  line-by-line twin of `RemoteMotion`): with the attacker's view delay, which travels in
  every sample it sends, and with when each sample reached it. A unit test checks the C++
  drawing and the Python replay agree to a millimetre. Immunity after a hit is judged the
  same way. Timestamps are trusted at most 0.4 s into the past, so a client cannot buy a
  bigger rewind.
* **Latency.** Messages of a frame/tick are batched into one TCP write (`collect-tcp`,
  flushed once per frame, `TCP_NODELAY`). The server relays movement and requests in the
  tick they arrive (100 Hz). Measured localhost round trip: about one client frame
  (~17 ms at 60 fps); the server's share is under 1 ms.

### Wire format

Everything on the wire is binary. Movement, the only high-rate traffic, skips the
generic field update (message type, sender, object id and field id: 12 bytes of header,
plus Panda3D's 2-byte length prefix). It uses three message types instead
(`server/arena/protocol.py`, `client/src/net/Protocol.h`):

| Message | Payload |
|---|---|
| `CLIENT_MOVE` (client → server) | one 10-byte state |
| `SERVER_MOVES` (server → clients, once per tick) | `uint8` count, then per player `uint8` slot + 10-byte state, the newest of the tick |
| `SERVER_SLOTS` (server → clients) | `uint8` slot + `uint32` doId per entry; doId 0 frees a slot |

The 10-byte state:

| Field | Type | Meaning |
|---|---|---|
| x, y | `int16` | position in cm |
| vx, vy | `int8` | velocity in ¼ m/s |
| flags | `uint8` | bit 0: a dash started here; bit 1: an input is held; bits 2–7: the sender's view delay in 4 ms |
| dir | `uint8` | held input direction, 256ths of a turn (1.4°) |
| t | `uint16` | network timestamp (10 ms ticks) |

| One movement update | before (`DistributedSmoothNode.setSmXYH`) | now |
|---|---|---|
| client → server | 18 bytes, no velocity | 14 bytes, with velocity and input |
| server → each other client | 22 bytes | 11 bytes inside the tick's batch (+5 per batch) |

The other fields are small too. Players are named by `uint8` slot in scores, hits, chat and
pickups (not a `uint32` doId), `ping`/`pong` take a `uint16`, `knockback` two `int8`s.
Game messages ("X joined", "Round 3 - go!", round results, shutdown) are a 4-byte
`notice(kind, slot, value)` that the client turns into text. The only strings left are
the player's name (sent once) and chat lines. Compression (zlib) would not help:
messages are 10–20 bytes, below what a compressor needs to save anything.

### The C++ side of distributed networking

The manual's C++ page shows the same Python API because, in 1.10, the C++ core of the
system (`DCFile`, `DCClass`, `DCPacker`, `CConnectionRepository`) is compiled only into
the Python module `panda3d/direct.pyd`, not into `libp3direct`. A C++ program cannot link
it from the SDK. `third_party/panda3d-direct` holds those exact 1.10.16 sources with the
Python bindings filtered out (see its README), and `client/src/net` builds a C++
`ClientRepository` on top, speaking the CMU protocol of `ClientRepository.py`.
Its packing is checked byte-for-byte against Python in the unit tests.

### Robustness

Server (`server/arena/server_repository.py`, a hardened `ServerRepository`):
* handshake with protocol version, DC hash and role; one trusted AI (per-process secret
  token); no doId range, interest or objects before a valid hello;
* allow-lists: players may create one `DistributedArenaPlayer` in the arena zone and send
  only the AI's `p2p` request fields. Names and colors cannot change after spawn. A
  `CLIENT_MOVE` always moves the sender's own slot, so nobody can move anyone else, and
  it must be exactly 10 bytes;
* every generate and update is validated against the DC file before it is relayed. Peers
  never receive malformed data, and the stock server's crash bugs on unknown
  class/field/target are gone;
* per-client message and byte rate limits, a 4 KB message cap, a 5 s hello timeout, a 20 s
  idle timeout, a max player count, and a cap on pending connections;
* a send timeout on every socket, so a client that stops reading is dropped instead of
  blocking the server;
* no exception reaches the task manager; the offending client is dropped with a reason
  (`SERVER_REJECT_ARENA`). Graceful shutdown on SIGINT/SIGTERM tells everyone why;
* single-threaded network polling: Panda3D's threaded reader can abort the process when a
  connection closes while it is polled, which kicking clients does.

Client (`client/src/net/ClientRepository.cpp`):
* non-blocking connect on a worker thread, so the window keeps rendering. Reconnects with
  jittered exponential backoff (1 → 10 s). It stops retrying on fatal rejects (wrong
  version or DC files);
* every received field is validated against the DC file before game code sees it. Updates
  are accepted only from the object's owner, which blocks spoofed targeted messages;
* receive watchdog for half-open connections, bounded work per frame, graceful disconnect
  on exit, predicted orb pickups with rollback, input-independent fixed-step physics.

### Measured (this repository's test machine, 4 cores, Linux)

| | |
|---|---|
| Server, 15 C++ bot clients | 5.2% of one core with lag compensation, 144–163 msg/s in (1.7–2.0 KB/s), 1,130–1,260 msg/s out |
| Windowed client, 1280×720, software OpenGL (llvmpipe) | 22 fps with shaders and shadows, 29 without shadows, 55–64 with `--low`; a GPU renders this scene far faster |
| Scenery | 283,000 vertices in 2 batches, generated in ~20 ms |
| Server, idle at 100 Hz tick | ~1.4% of one core |
| Headless C++ client | ~0.7% of one core |
| Upload per player | ~0.12 KB/s (movement + pings) |
| Download per player | ~0.16 KB/s per other moving player |

### Under internet latency

`server/netsim.py` is a TCP proxy that adds a round trip time and random jitter, keeping
TCP's ordering, so late data arrives in bunches as on a real connection. With
`e2e_cpp_clients.py --netsim RTT:JITTER --trace-dir DIR`, every client plays through it,
and logs where it really is and where (and which way) it draws everyone else (`--trace`).
The test then reports how the netcode coped (`server/tests/trace_stats.py`).

The numbers below are for 8 C++ bots over 45 s. The bots chase gems and every 5–12 s they
fidget for 1–2.5 s, like people do: 50–180 ms taps, often reversing. That is the motion
that is hardest to show smoothly. Before → now, where "before" is the previous version
(velocity-only samples, drawn in the present with fading offsets):

| Per player-minute (fewer is better) | localhost | 100 ms ± 15 | 200 ms ± 40 |
|---|---|---|---|
| **Backtracks**: drawn moving more than 90° away from any way the player really moved in the last 0.3 s (the rubber band after a tap) | 20 → 0.1 | 59 → 2.2 | 76 → 5.6 |
| **Jumps**: drawn more than 1.5× faster than the real motion | 0.4 → 0.3 | 53 → 2.6 | 232 → 6.4 |
| **Turn snaps**: heading turned more than 20° in one frame | 106 → 0.3 | 109 → 1.4 | n/a → 0.0 |

| | localhost | 100 ms ± 15 | 200 ms ± 40 |
|---|---|---|---|
| Movement samples per client per second | 10.1 → 7.5 | 10.5 → 7.4 | 10.3 → 8.2 |
| Upload / download per client (B/s) | 120 / 1,236 → 100 / 1,090 | 126 / 1,286 → 99 / 1,070 | 124 / 1,247 → 109 / 1,118 |
| Dash contacts the server confirmed | 100% → 99% | 99% → 96% | 95% → 92% |
| Drawn vs real position at the same moment, mean | 18 → 35 cm | 74 → 134 cm | 149 → 219 cm |
| Corrections of honest clients | 0 | 0 | 0 |

The last table's position row is the price of the view delay. Others are drawn about one
delay in the past (at 9 m/s, 10 cm per 11 ms), exactly along the path they took, instead
of guessed into the present and pulled back. Hits are judged on what the attacker drew, so
they stay fair. Compared with the first version of this game (fixed 20 Hz `setSmXYH`),
each client now uploads and downloads about 70% less.

---

## Tests

`ctest` runs four suites:

* `arena_tests` (C++):
  * clock sync math (incl. 16-bit wrap, as in `ClockDelta.py`);
  * movement and collisions, and the client physics never tripping the server's speed check;
  * DC packing identical to Python, and validation of truncated/padded/oversized fields;
  * the 10-byte movement state byte-identical to Python;
  * the closed-form prediction matching the 120 Hz movement code to millimetres;
  * dead reckoning: a tap costs two samples, heartbeats only on straight runs, the error
    bound on curves and dashes, silence while standing;
  * remote drawing: taps and zigzags drawn without sliding back, late turns blended
    without jumps, teleports snap, and the result matching the AI's Python replay;
  * the view delay and the turn rate;
  * ChaCha20 against RFC 8439, and the built `arena.pak` mounting and reading like the
    plain files while refusing any other key;
  * the touch controls (stick directions and dead zone, the stick following the thumb,
    dash on touch down, pinch zoom, the statistics tap), the connect screen's address
    parsing, LAN discovery packets byte-identical to Python, and the settings file.
* `server_tests` (Python): `test_server.py` has 21 hostile-client and protocol cases (wrong
  hash/version, fake AI, messages before hello, forbidden classes, foreign objects,
  targeted updates, renames, malformed fields and moves, moves without a player,
  flooding, silent connections, random garbage, the compact relay and the newcomer cache)
  checked against the server's answer and survival. `test_lagcomp.py` covers
  lag-compensated hits: the attacker's view under latency, braking predicted from a
  released input, a tap drawn exactly as it happened, late turns blended without jumps,
  capped velocities and rewinds, immunity, and the speed check ignoring timestamps.
  `test_pack.py` checks `tools/make_pack.py`: ChaCha20 against the RFC, and Panda3D's own
  `Multifile` reading what it writes. `test_discovery.py` checks that LAN probes get answers,
  garbage does not, replies never outgrow probes, and replies are rate-limited.
* `end_to_end` (`server/tests/e2e_cpp_clients.py`): real server, three headless C++ clients
  and a Python bot. Each client must join, see everyone, get pongs, receive every chat
  line, score, never be corrected, and exit 0. Then the server must stop cleanly on SIGTERM.
  Given several client builds, it mixes them in one game:
  `python server/tests/e2e_cpp_clients.py build/linux/bin/arena_client build/mingw-cross/bin/arena_client.exe --clients 4`
  plays Linux and Windows (Wine) clients against each other.
* `end_to_end_wan`: the same with four clients behind `netsim.py` (150 ms RTT, up to
  20 ms jitter each way). There must be no corrections, and every chat line and pong
  must arrive.

Load testing: `python server/bots.py --count 16 --port 4400` connects headless Python
players; `arena_client --headless` runs C++ bots.

---

## Reference

### Server: `python server/arena_server.py [options]`

| Option | Default | |
|---|---|---|
| `--bind ADDR` | `0.0.0.0` | listen address (`::` for IPv6) |
| `--port N` | 4400 | TCP port |
| `--max-players N` | 16 | |
| `--round-time S` | 180 | seconds per round |
| `--orbs N` | 18 | orbs on the field |
| `--tick-rate HZ` | 100 | relay latency is at most one tick |
| `--name NAME` | computer name | shown to players on the same network (LAN discovery) |
| `--no-lan` | | do not answer LAN discovery probes (UDP, same port number) |
| `--status-interval S` | 30 | status line (players, msg/s, KB/s, kicks, round stats) |
| `--verbose` | | debug logging |

### Client: `arena_client [options]`

`--host`, `--port`, `--name` (max 16 bytes), `--color 0-11`, `--size WxH`, `--fullscreen`,
`--zoom 0.55-1.6`, `--no-shadows`, `--no-msaa`, `--no-vsync`, `--low`, `--no-reconnect`,
`--pack FILE`, `--dc-dir DIR`, `--touch` (touch controls, and the connect screen unless
`--host` is given), `--ui-scale 0.8-2`.
Automation: `--headless`, `--bot`, `--duration S`, `--screenshot FILE` (after
`--screenshot-after S`), `--say TEXT`, `--expect-players N`, `--trace FILE`. The client prints a
`summary:` line on exit. Exit codes: 0 ok, 1 init failed, 2 never joined, 3 fewer players
than expected, 4 rejected by the server. `arena_client --help` lists everything.

### Layout

```
common/                 direct.dc (pinned 1.10.16) + arena.dc, shared by all sides
server/arena_server.py  dedicated server (ServerRepository + AI repository)
server/bots.py          headless Python players for load tests
server/netsim.py        latency/jitter/bandwidth proxy for testing under internet conditions
server/arena/           protocol, policy, hardened ServerRepository, AI repository, game logic,
                        motion.py (the AI's replay of what clients draw), discovery.py (LAN)
server/tests/           hardening, lag compensation, pack and end-to-end tests, trace analysis
client/src/core/        logging, ChaCha20, the encrypted resource pack (VFS mount), settings
client/src/net/         C++ ClientRepository, DistributedObject, ClockDelta, TimeManager,
                        LAN discovery
client/src/game/        game client, players, prediction, dead reckoning, remote drawing,
                        physics, scenery, world, HUD, touch controls, connect screen
client/src/app/         window, input, main loop, options, platform layer,
                        AndroidMain.cpp (the Android entry point)
client/shaders/         GLSL: world (light, shadows, wind, haze) and sky
client/tests/           C++ unit tests
third_party/panda3d-direct/   Panda3D's DC parser + CConnectionRepository (see README)
cmake/Panda3DFromSource.cmake  compiles Panda3D 1.10.16 with g++ (default)
cmake/panda3d-patches/  patches for the Panda3D sources (MinGW-w64, OpenGL ES, Android)
cmake/toolchains/       MinGW-w64 cross-compilation toolchain
cmake/FindPanda3D.cmake locates an installed SDK (linux-gcc-sdk preset)
tools/make_pack.py      builds arena.pak (Multifile + ChaCha20)
tools/make_apk.py       packs and signs the Android APK
android/                AndroidManifest.xml
tools/vendor_panda3d_direct.py  refreshes third_party/panda3d-direct
```

### Limitations

* The CMU `ServerRepository` protocol is TCP only. A lost packet delays later ones on
  that connection; there is no UDP channel.
* Movement is client-authoritative with server-side plausibility checks, not a
  server-side simulation. Speed hacks and teleports are caught, but subtle cheating within
  the speed budget is not.
* Lag compensation favours the attacker, as most action games do. With a slow connection
  on either side, a victim can be hit where the attacker saw them, up to 0.4 s behind
  where the victim already is on their own screen.
* One Python process serves one arena. Run more processes on other ports for more arenas.
