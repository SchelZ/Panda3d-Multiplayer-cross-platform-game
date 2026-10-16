# Cross-compile the Windows client from Linux with MinGW-w64 g++ (posix threads).
#
#   sudo apt install g++-mingw-w64-x86-64-posix      # Debian/Ubuntu
#   cmake --preset windows-mingw-cross && cmake --build build/mingw-cross
#
# On Windows itself, use the MSYS2 MinGW-w64 g++ directly (preset windows-mingw).

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(_triple x86_64-w64-mingw32)

find_program(CMAKE_C_COMPILER NAMES ${_triple}-gcc-posix ${_triple}-gcc REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES ${_triple}-g++-posix ${_triple}-g++ REQUIRED)
find_program(CMAKE_RC_COMPILER NAMES ${_triple}-windres)

set(CMAKE_FIND_ROOT_PATH /usr/${_triple})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Lets ctest run the Windows test binaries through Wine, if installed.
find_program(_wine NAMES wine64 wine)
if(_wine)
    set(CMAKE_CROSSCOMPILING_EMULATOR ${_wine})
endif()
