cmake_minimum_required(VERSION 3.18)
if(POLICY CMP0144)
    cmake_policy(SET CMP0144 NEW)  # honour -DPANDA3D_ROOT
endif()
# MinGW GCC in a folder whose path has a space (e.g. under C:\Program Files):
# its specs add the default manifest with %:if-exists(default-manifest.o%s),
# whose result GCC splits at spaces, so every link fails ("ld: cannot find
# C:/Program"). Link with the same spec minus if-exists instead, which GCC
# passes as one argument (-specs, set before project() so CMake's compiler
# check uses it too). Builds with a toolchain file (Android, MinGW from Linux)
# bring their own compiler and are left alone.
if(CMAKE_HOST_WIN32 AND NOT CMAKE_TOOLCHAIN_FILE)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/MinGWPathWithSpaces.cmake)
else()
    # A build folder configured before that check may still have the flag.
    foreach(_kind EXE SHARED MODULE)
        if(DEFINED CACHE{CMAKE_${_kind}_LINKER_FLAGS} AND
           "$CACHE{CMAKE_${_kind}_LINKER_FLAGS}" MATCHES "mingw-path-with-spaces")
            string(REGEX REPLACE " *\"?-specs=[^\"]*mingw-path-with-spaces\\.specs\"?" ""
                   _flags "$CACHE{CMAKE_${_kind}_LINKER_FLAGS}")
            set_property(CACHE CMAKE_${_kind}_LINKER_FLAGS PROPERTY VALUE "${_flags}")
        endif()
    endforeach()
endif()

project(PandaArena VERSION 1.0.0 LANGUAGES C CXX)

if(ANDROID)
    message(STATUS "Android NDK: ${ANDROID_NDK} (${ANDROID_ABI}, API ${ANDROID_PLATFORM_LEVEL})")
    # -O2 without debug info for everything (Panda3D, FreeType, the game),
    # instead of the NDK's -O3 -g: Android's usual release level, objects a
    # fraction of the size (the library is stripped anyway), and NDK r27's
    # clang crashed on Windows optimising FreeType at -O3 -g.
    add_compile_options(-O2 -g0)
endif()

# C++ client of Panda Arena, built with g++ on Linux and MinGW-w64 g++ on Windows.
# By default the parts of Panda3D 1.10.16 the client needs are compiled from source
# with the same compiler and linked statically (cmake/Panda3DFromSource.cmake), so no
# prebuilt Panda3D SDK is needed. The server is Python (server/arena_server.py).

# C++20 with GNU extensions (-std=gnu++20). Strict ISO mode would hide _isnan and
# _finite in MinGW-w64's math.h, which Panda3D's headers use on Windows; Panda3D
# itself is compiled as gnu++11, like its own build system does.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if(MSVC)
    message(FATAL_ERROR "Panda Arena is built with g++: use MinGW-w64 on Windows "
                        "(MSYS2 UCRT64/MINGW64 shell, see README.md).")
endif()
# Old MinGW toolchains fail deep inside their own headers (e.g. Nim's bundled
# GCC 11.1: libstdc++'s unordered_map in C++20 mode, and a float.h that hides
# _isnan), so stop early with a clear message instead.
option(ARENA_ALLOW_OLD_GCC "Try to build with a MinGW GCC older than 12 anyway" OFF)
if(MINGW AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 12
   AND NOT ARENA_ALLOW_OLD_GCC)
    message(FATAL_ERROR
        "MinGW GCC ${CMAKE_CXX_COMPILER_VERSION} (${CMAKE_CXX_COMPILER}) is too old for this project. "
        "Install a current MinGW-w64, e.g. WinLibs GCC 16 UCRT (https://winlibs.com, see README.md), "
        "put its bin folder first on the PATH, delete this build folder and configure again. "
        "(-DARENA_ALLOW_OLD_GCC=ON tries anyway.)")
endif()

option(ARENA_PANDA3D_FROM_SOURCE "Compile Panda3D 1.10.16 with this compiler (otherwise use an installed SDK)" ON)
if(ANDROID)
    set(_arena_tests_default OFF)  # they run on the PC builds
else()
    set(_arena_tests_default ON)
endif()
option(ARENA_BUILD_TESTS "Build the client unit tests" ${_arena_tests_default})
option(ARENA_NATIVE_ARCH "Optimise for the build machine's CPU (-march=native); not portable" OFF)
# The same knobs as the main project's cmake/CompilerOptions.cmake (docs/GUIDE.md section 6).
option(ARENA_X86_64_V3 "Target x86-64-v3 (AVX2, FMA, F16C): CPUs since 2013; the exe will not run on older ones" OFF)
option(ARENA_FAST_MATH "The safe -ffast-math subset for game code (reassociation, FMA; keeps NaN/Inf)" ON)
set(ARENA_PGO "OFF" CACHE STRING "Profile-guided optimisation: OFF | GEN (instrumented build) | USE")
set_property(CACHE ARENA_PGO PROPERTY STRINGS OFF GEN USE)
set(ARENA_PGO_DIR "${CMAKE_BINARY_DIR}/pgo" CACHE PATH "Where GEN builds write and USE builds read profiles")

if(ARENA_PANDA3D_FROM_SOURCE)
    include(Panda3DFromSource)
else()
    if(WIN32)
        message(FATAL_ERROR
            "The prebuilt Panda3D Windows SDK is compiled with MSVC, whose C++ ABI MinGW g++ "
            "cannot link against. Keep ARENA_PANDA3D_FROM_SOURCE=ON to compile Panda3D with g++.")
    endif()
    find_package(Panda3D 1.10 REQUIRED)
    set(Panda3D_OBJECTS)
endif()
message(STATUS "Panda3D ${Panda3D_VERSION} (shader generator: ${Panda3D_HAVE_CG})")

# Executables get Panda3D's objects directly (from-source build) or its libraries (SDK).
function(arena_link_panda3d target)
    target_sources(${target} PRIVATE ${Panda3D_OBJECTS})
    target_link_libraries(${target} PRIVATE Panda3D::Panda3D)
    if(ARENA_PGO STREQUAL "GEN")
        target_link_options(${target} PRIVATE -fprofile-generate=${ARENA_PGO_DIR})
    endif()
    target_link_options(${target} PRIVATE $<$<CONFIG:Release,MinSizeRel>:-s>)  # RelWithDebInfo keeps symbols
    if(MINGW)
        # One self-contained .exe: no libstdc++/libgcc/winpthread DLLs to ship.
        target_link_options(${target} PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
    if(ANDROID)
        # NativeActivity looks this up in libarena.so (AndroidMain.cpp).
        target_link_options(${target} PRIVATE -Wl,-u,ANativeActivity_onCreate)
    endif()
endfunction()

set(ARENA_BIN_DIR ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${ARENA_BIN_DIR})
foreach(cfg Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER ${cfg} CFG)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CFG} ${ARENA_BIN_DIR})
endforeach()

function(arena_compile_options target)
    if(ANDROID)
        set(_opt -O2)  # see add_compile_options above
    else()
        set(_opt -O3)
    endif()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-unused-parameter
        $<$<CONFIG:Release,RelWithDebInfo>:${_opt}>)
    if(NOT WIN32 AND NOT ANDROID)
        target_compile_options(${target} PRIVATE $<$<CONFIG:Release,RelWithDebInfo>:-fno-plt>)
    endif()
    if(ARENA_FAST_MATH)
        target_compile_options(${target} PRIVATE $<$<CONFIG:Release,RelWithDebInfo>:
            -fno-math-errno -fno-trapping-math -fno-signed-zeros -fassociative-math
            -freciprocal-math -ffp-contract=fast>)
    endif()
    if(ARENA_NATIVE_ARCH)
        target_compile_options(${target} PRIVATE -march=native)
    elseif(ARENA_X86_64_V3)
        target_compile_options(${target} PRIVATE -march=x86-64-v3)
    endif()
    # PGO: build with -DARENA_PGO=GEN, play (or run a --bot client) for a few
    # minutes, rebuild with -DARENA_PGO=USE. The profile decides branch layout
    # and inlining (what [[likely]] guesses).
    if(ARENA_PGO STREQUAL "GEN")
        target_compile_options(${target} PRIVATE -fprofile-generate=${ARENA_PGO_DIR} -fprofile-update=atomic)
        target_link_options(${target} PRIVATE -fprofile-generate=${ARENA_PGO_DIR})
    elseif(ARENA_PGO STREQUAL "USE")
        target_compile_options(${target} PRIVATE -fprofile-use=${ARENA_PGO_DIR} -fprofile-partial-training
                                                 -Wno-missing-profile)
    endif()
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        include(CheckIPOSupported)
        check_ipo_supported(RESULT _ipo OUTPUT _ipo_msg LANGUAGES CXX)
        if(_ipo)
            set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION ON)
        endif()
    endif()
endfunction()

# Panda3D's DC parser and CConnectionRepository. The 1.10 SDKs compile them into the
# Python module only, so we build them here (see third_party/panda3d-direct/README.md).
add_library(p3dc_vendored STATIC
    third_party/panda3d-direct/dcparser/p3dcparser_composite1.cxx
    third_party/panda3d-direct/dcparser/p3dcparser_composite2.cxx
    third_party/panda3d-direct/dcparser/dcParser.cxx
    third_party/panda3d-direct/dcparser/dcLexer.cxx
    third_party/panda3d-direct/distributed/config_distributed.cxx
    third_party/panda3d-direct/distributed/cConnectionRepository.cxx
)
# BEFORE: these headers must win over the same-named ones in the SDK.
target_include_directories(p3dc_vendored BEFORE PUBLIC
    third_party/panda3d-direct/dcparser
    third_party/panda3d-direct/distributed)
# Linked statically: the classes are defined here, not imported from a DLL.
target_compile_definitions(p3dc_vendored PUBLIC BUILDING_DIRECT_DCPARSER BUILDING_DIRECT_DISTRIBUTED)
target_link_libraries(p3dc_vendored PUBLIC Panda3D::Panda3D)
target_compile_options(p3dc_vendored PRIVATE -w $<$<CONFIG:Release,RelWithDebInfo>:-O2>)

# Resources: the DC files and shaders ship in arena.pak, a Panda3D Multifile
# encrypted with ChaCha20 (tools/make_pack.py, client/src/core/ResourcePack.h).
# The key is compiled into the client: a random one per build folder unless
# ARENA_PACK_KEY is set (e.g. to give every release the same key).
set(ARENA_PACK_KEY "" CACHE STRING "Key of arena.pak, 64 hex digits (empty: a random one for this build folder)")
string(LENGTH "${ARENA_PACK_KEY}" _key_length)
if(ARENA_PACK_KEY STREQUAL "")
    string(RANDOM LENGTH 64 ALPHABET 0123456789abcdef _key)
    set(ARENA_PACK_KEY ${_key} CACHE STRING "Key of arena.pak, 64 hex digits (random, made for this build folder)" FORCE)
elseif(NOT _key_length EQUAL 64 OR NOT ARENA_PACK_KEY MATCHES "^[0-9a-fA-F]+$")
    message(FATAL_ERROR "ARENA_PACK_KEY must be 64 hex digits")
endif()
# The executable holds the key XOR a mask, and the mask: no plain key in it.
string(SHA256 _mask "arena.pak mask ${ARENA_PACK_KEY}")
set(_masked "")
set(_mask_bytes "")
foreach(i RANGE 0 31)
    math(EXPR _at "${i} * 2")
    string(SUBSTRING "${ARENA_PACK_KEY}" ${_at} 2 _k)
    string(SUBSTRING "${_mask}" ${_at} 2 _m)
    math(EXPR _v "0x${_k} ^ 0x${_m}" OUTPUT_FORMAT HEXADECIMAL)
    string(APPEND _masked "${_v},")
    string(APPEND _mask_bytes "0x${_m},")
endforeach()
file(CONFIGURE OUTPUT ${CMAKE_BINARY_DIR}/generated/arena_pack_key.h CONTENT
"// Generated by CMake from ARENA_PACK_KEY: the key of arena.pak, as key XOR
// mask and mask (client/src/core/ResourcePack.cpp puts them together).
#pragma once
static const unsigned char kPackKeyMasked[32] = {@_masked@};
static const unsigned char kPackKeyMask[32] = {@_mask_bytes@};
" @ONLY)

# Networking: Panda3D distributed-object client (no graphics), and resources.
add_library(arena_net STATIC
    client/src/core/ChaCha20.cpp
    client/src/core/Log.cpp
    client/src/core/ResourcePack.cpp
    client/src/net/ClockDelta.cpp
    client/src/net/ClientRepository.cpp
    client/src/net/TimeManager.cpp
)
target_include_directories(arena_net PUBLIC client/src PRIVATE ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(arena_net PUBLIC p3dc_vendored Panda3D::Panda3D)
arena_compile_options(arena_net)

# Game: rendering, input, HUD, gameplay.
set(ARENA_CLIENT_SOURCES
    client/src/app/App.cpp
    client/src/app/Options.cpp
    client/src/game/ArenaGame.cpp
    client/src/game/ArenaPlayer.cpp
    client/src/game/DeadReckoning.cpp
    client/src/game/GameClient.cpp
    client/src/game/Hud.cpp
    client/src/game/Meshes.cpp
    client/src/game/Movement.cpp
    client/src/game/RemoteMotion.cpp
    client/src/game/Scenery.cpp
    client/src/game/World.cpp
)
# Phones only: the touch controls, the connect screen with servers found on
# the network, remembered settings, and Android's input and lifecycle (the
# code that uses them in App.cpp is #ifdef __ANDROID__). The unit tests also
# build the platform-independent ones, on the PC.
set(ARENA_PHONE_SOURCES
    client/src/core/Settings.cpp
    client/src/game/ConnectScreen.cpp
    client/src/game/TouchControls.cpp
    client/src/net/LanDiscovery.cpp
)
if(ANDROID)
    # Android runs the game as a library: NativeActivity loads libarena.so and
    # android_main() (AndroidMain.cpp) starts it; see android/.
    add_library(arena_client SHARED ${ARENA_CLIENT_SOURCES} ${ARENA_PHONE_SOURCES}
                client/src/app/AndroidMain.cpp client/src/app/Platform.cpp)
    set_target_properties(arena_client PROPERTIES OUTPUT_NAME arena)
else()
    add_executable(arena_client client/src/main.cpp ${ARENA_CLIENT_SOURCES})
endif()
target_link_libraries(arena_client PRIVATE arena_net)
arena_link_panda3d(arena_client)
arena_compile_options(arena_client)
if(WIN32)
    set_target_properties(arena_client PROPERTIES WIN32_EXECUTABLE OFF)  # keep the log console
endif()

# The client loads the same DC files as the server, and its shaders, from
# bin/arena.pak. Packing needs Python 3 (standard library only); without it
# the files are copied next to the exe as they are (a development build).
find_package(Python3 COMPONENTS Interpreter QUIET)
file(GLOB ARENA_SHADERS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/client/shaders/*.vert
     ${CMAKE_CURRENT_SOURCE_DIR}/client/shaders/*.frag ${CMAKE_CURRENT_SOURCE_DIR}/client/shaders/*.glsl)
set(ARENA_PACK ${ARENA_BIN_DIR}/arena.pak)
if(Python3_FOUND)
    add_custom_command(OUTPUT ${ARENA_PACK}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/make_pack.py
                --key ${ARENA_PACK_KEY} --out ${ARENA_PACK}
                dc/direct.dc=${CMAKE_CURRENT_SOURCE_DIR}/common/direct.dc
                dc/arena.dc=${CMAKE_CURRENT_SOURCE_DIR}/common/arena.dc
                shaders/=${CMAKE_CURRENT_SOURCE_DIR}/client/shaders
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${ARENA_BIN_DIR}/dc ${ARENA_BIN_DIR}/shaders
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/make_pack.py ${CMAKE_CURRENT_SOURCE_DIR}/common/direct.dc
                ${CMAKE_CURRENT_SOURCE_DIR}/common/arena.dc ${ARENA_SHADERS}
        COMMENT "Packing DC files and shaders into arena.pak"
        VERBATIM)
    add_custom_target(arena_pack ALL DEPENDS ${ARENA_PACK})
    add_dependencies(arena_client arena_pack)
elseif(ANDROID)
    message(FATAL_ERROR "The Android build needs Python 3 (it packs arena.pak and the APK)")
else()
    message(WARNING "Python 3 not found: the DC files and shaders are copied next to the "
                    "client unencrypted instead of packed into arena.pak")
    add_custom_command(TARGET arena_client POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${ARENA_BIN_DIR}/dc ${ARENA_BIN_DIR}/shaders
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/common/direct.dc
                ${CMAKE_CURRENT_SOURCE_DIR}/common/arena.dc ${ARENA_BIN_DIR}/dc/
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ARENA_SHADERS} ${ARENA_BIN_DIR}/shaders/
        VERBATIM)
endif()

# Android: bin/PandaArena.apk, ready for "adb install" (tools/make_apk.py; the
# SDK's build tools pack libarena.so, arena.pak and android/AndroidManifest.xml).
if(ANDROID)
    set(_sdk "$ENV{ANDROID_HOME}")
    if(NOT _sdk)
        set(_sdk "$ENV{ANDROID_SDK_ROOT}")
    endif()
    if(NOT _sdk AND ANDROID_NDK)
        get_filename_component(_sdk "${ANDROID_NDK}/../.." ABSOLUTE)  # <sdk>/ndk/<version>
    endif()
    set(ARENA_ANDROID_SDK "${_sdk}" CACHE PATH "Android SDK with build-tools and platforms (for the APK)")
    set(ARENA_ANDROID_KEYSTORE "" CACHE FILEPATH
        "Keystore to sign the APK with (empty: a debug key in bin/); its password in ARENA_KEYSTORE_PASS")
    set(ARENA_ANDROID_KEY_ALIAS "upload" CACHE STRING "Key alias in ARENA_ANDROID_KEYSTORE")
    if(NOT EXISTS "${ARENA_ANDROID_SDK}/build-tools")
        message(WARNING "No Android SDK build-tools in '${ARENA_ANDROID_SDK}': libarena.so is built, but "
                        "not the APK. Set ANDROID_HOME (or -DARENA_ANDROID_SDK=...) to the SDK folder.")
    else()
        math(EXPR _version_code "${PROJECT_VERSION_MAJOR} * 10000 + ${PROJECT_VERSION_MINOR} * 100 + ${PROJECT_VERSION_PATCH}")
        set(_signing)
        if(ARENA_ANDROID_KEYSTORE)
            set(_signing --keystore ${ARENA_ANDROID_KEYSTORE} --key-alias ${ARENA_ANDROID_KEY_ALIAS}
                         --ks-pass env:ARENA_KEYSTORE_PASS)
        endif()
        set(ARENA_APK ${ARENA_BIN_DIR}/PandaArena.apk)
        add_custom_command(OUTPUT ${ARENA_APK}
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/make_apk.py
                    --sdk ${ARENA_ANDROID_SDK} --lib $<TARGET_FILE:arena_client> --abi ${ANDROID_ABI}
                    --pack ${ARENA_PACK} --manifest ${CMAKE_CURRENT_SOURCE_DIR}/android/AndroidManifest.xml
                    --min-sdk ${ANDROID_PLATFORM_LEVEL} --version-code ${_version_code}
                    --version-name ${PROJECT_VERSION} ${_signing} --out ${ARENA_APK}
            DEPENDS arena_client ${ARENA_PACK} ${CMAKE_CURRENT_SOURCE_DIR}/tools/make_apk.py
                    ${CMAKE_CURRENT_SOURCE_DIR}/android/AndroidManifest.xml
            COMMENT "Packing and signing PandaArena.apk"
            VERBATIM)
        add_custom_target(arena_apk ALL DEPENDS ${ARENA_APK})
    endif()
endif()

if(ARENA_BUILD_TESTS)
    enable_testing()
    add_executable(arena_tests
        client/tests/TestMain.cpp
        client/tests/ClockDeltaTests.cpp
        client/tests/MovementTests.cpp
        client/tests/ProtocolTests.cpp
        client/tests/DeadReckoningTests.cpp
        client/tests/ResourcePackTests.cpp
        client/tests/TouchTests.cpp
        client/src/game/ArenaGame.cpp
        client/src/game/ArenaPlayer.cpp
        client/src/game/DeadReckoning.cpp
        client/src/game/Meshes.cpp
        client/src/game/Movement.cpp
        client/src/game/RemoteMotion.cpp
        ${ARENA_PHONE_SOURCES}
    )
    target_link_libraries(arena_tests PRIVATE arena_net)
    arena_link_panda3d(arena_tests)
    target_compile_definitions(arena_tests PRIVATE ARENA_COMMON_DIR="${CMAKE_CURRENT_SOURCE_DIR}/common"
                               ARENA_PACK_FILE="${ARENA_PACK}" ARENA_PACK_KEY="${ARENA_PACK_KEY}")
    arena_compile_options(arena_tests)
    add_test(NAME arena_tests COMMAND arena_tests)
    if(TARGET arena_pack)
        add_dependencies(arena_tests arena_pack)
    endif()

    if(Python3_FOUND)
        add_test(NAME server_tests
                 COMMAND ${Python3_EXECUTABLE} -m unittest discover -s tests -v
                 WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/server)
        add_test(NAME end_to_end
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/server/tests/e2e_cpp_clients.py
                         $<TARGET_FILE:arena_client>
                 WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/server)
        # The same over a simulated internet link (server/netsim.py): honest
        # clients must still never be corrected, and everything must arrive.
        add_test(NAME end_to_end_wan
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/server/tests/e2e_cpp_clients.py
                         $<TARGET_FILE:arena_client> --clients 4 --duration 16 --netsim 150:20
                 WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/server)
        set_tests_properties(server_tests end_to_end end_to_end_wan PROPERTIES TIMEOUT 180)
    endif()
endif()
