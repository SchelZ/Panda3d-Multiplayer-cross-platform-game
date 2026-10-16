# Builds the parts of Panda3D 1.10.16 that the client uses, from source, with the
# project's own compiler: g++ on Linux, MinGW-w64 g++ on Windows, the NDK's
# clang on Android. Everything is
# compiled into object libraries and linked statically into the executables
# (LINK_ALL_STATIC, as makepanda does for its static builds), so there are no
# Panda3D DLLs to ship and no ABI to match against a prebuilt SDK.
#
# File lists, defines and flags mirror makepanda/makepanda.py of v1.10.16.
#
# Inputs:  PANDA3D_SOURCE_DIR (optional; otherwise the v1.10.16 tag is fetched)
#          ARENA_GLES: render with OpenGL ES 2/3 (gles2gsg) instead of desktop
#          OpenGL; always on for Android (androiddisplay), optional on Linux
#          (egldisplay on X11) to try the Android renderer on a PC
# Output:  target Panda3D::Panda3D (INTERFACE: objects, includes, defines, libs)

include(FetchContent)

set(ARENA_PANDA3D_TAG "v1.10.16")
set(PANDA3D_SOURCE_DIR "" CACHE PATH "Panda3D source tree (empty: fetch ${ARENA_PANDA3D_TAG} from GitHub)")

if(NOT PANDA3D_SOURCE_DIR)
    FetchContent_Declare(panda3d_src
        GIT_REPOSITORY https://github.com/panda3d/panda3d.git
        GIT_TAG ${ARENA_PANDA3D_TAG}
        GIT_SHALLOW TRUE
        GIT_PROGRESS TRUE
        SOURCE_SUBDIR no-cmake-build)  # 1.10 has no CMake build; just download it
    message(STATUS "Fetching Panda3D ${ARENA_PANDA3D_TAG} source (once, ~150 MB)")
    FetchContent_MakeAvailable(panda3d_src)
    set(_p3src "${panda3d_src_SOURCE_DIR}")
else()
    set(_p3src "${PANDA3D_SOURCE_DIR}")
endif()
if(NOT EXISTS "${_p3src}/panda/src/pgraph/nodePath.cxx")
    message(FATAL_ERROR "${_p3src} is not a Panda3D source tree")
endif()
set(Panda3D_SOURCE_DIR "${_p3src}")

if(ANDROID)
    set(ARENA_GLES ON)
elseif(WIN32)
    set(ARENA_GLES OFF)
else()
    option(ARENA_GLES "Render with OpenGL ES through EGL, like on Android (to test that renderer)" OFF)
endif()

# Small, idempotent source patches (MinGW portability); see cmake/panda3d-patches/.
file(GLOB _p3_patches "${CMAKE_CURRENT_LIST_DIR}/panda3d-patches/*.patch")
list(SORT _p3_patches)
find_package(Git QUIET)
foreach(_patch IN LISTS _p3_patches)
    execute_process(COMMAND ${GIT_EXECUTABLE} apply --reverse --check --ignore-whitespace "${_patch}"
                    WORKING_DIRECTORY "${_p3src}" RESULT_VARIABLE _already OUTPUT_QUIET ERROR_QUIET)
    if(NOT _already EQUAL 0)
        execute_process(COMMAND ${GIT_EXECUTABLE} apply --ignore-whitespace "${_patch}"
                        WORKING_DIRECTORY "${_p3src}" RESULT_VARIABLE _applied)
        if(NOT _applied EQUAL 0)
            message(FATAL_ERROR "could not apply ${_patch} to ${_p3src}")
        endif()
        get_filename_component(_name "${_patch}" NAME)
        message(STATUS "Panda3D: applied ${_name}")
    endif()
endforeach()

# ----------------------------------------------------------------- dependencies

# The Windows executables are fully static, while MSYS2's FreeType/zlib/libpng are DLLs
# with dependencies of their own, so there FreeType is built from source by default.
if(WIN32 OR ANDROID)
    set(_p3_system_libs_default OFF)
else()
    set(_p3_system_libs_default ON)
endif()
option(ARENA_PANDA3D_SYSTEM_LIBS "Link the system's FreeType, zlib and libpng"
       ${_p3_system_libs_default})

# FreeType renders the HUD text. Use the system's, or build it.
if(ARENA_PANDA3D_SYSTEM_LIBS)
    find_package(Freetype QUIET)
endif()
if(NOT FREETYPE_FOUND)
    message(STATUS "Building FreeType from source (static)")
    FetchContent_Declare(freetype
        GIT_REPOSITORY https://github.com/freetype/freetype.git
        GIT_TAG VER-2-13-3
        GIT_SHALLOW TRUE)
    set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF)
    FetchContent_MakeAvailable(freetype)
    set(_p3_freetype freetype)
else()
    set(_p3_freetype Freetype::Freetype)
endif()

# libpng/zlib are optional (PNG screenshots, compressed files); without them
# screenshots can still be saved as .bmp, .tga or .ppm.
set(_p3_have_zlib OFF)
set(_p3_have_png OFF)
if(ARENA_PANDA3D_SYSTEM_LIBS)
    find_package(ZLIB QUIET)
    find_package(PNG QUIET)
    set(_p3_have_zlib ${ZLIB_FOUND})
    set(_p3_have_png ${PNG_FOUND})
endif()

find_package(Threads REQUIRED)
if(WIN32)
    set(_p3_platform_libs opengl32 gdi32 user32 kernel32 advapi32 shell32 ws2_32 winmm
                          iphlpapi imm32 ole32 oleaut32 uuid version setupapi)
elseif(ANDROID)
    set(_p3_platform_libs android log EGL GLESv2 ${CMAKE_DL_LIBS} m)
elseif(ARENA_GLES)
    find_package(X11 REQUIRED)
    find_library(_p3_egl_lib EGL REQUIRED)
    find_library(_p3_gles2_lib GLESv2 REQUIRED)
    find_path(_p3_gles2_include GLES2/gl2.h REQUIRED)
    set(_p3_platform_libs ${_p3_egl_lib} ${_p3_gles2_lib} X11::X11 ${CMAKE_DL_LIBS} m)
else()
    find_package(OpenGL REQUIRED)
    find_package(X11 REQUIRED)
    set(_p3_platform_libs OpenGL::GL X11::X11 ${CMAKE_DL_LIBS} m)
endif()

# ------------------------------------------------------------ generated headers

set(_p3gen "${CMAKE_BINARY_DIR}/panda3d-generated/include")
file(MAKE_DIRECTORY "${_p3gen}")

# dtool_config.h: makepanda's DTOOL_CONFIG table (Windows / Unix columns) with
# the optimize-3 additions, minus everything this build does not include.
set(_cfg_common
    "COMPILE_IN_DEFAULT_FONT 1" "USE_PANDAFILESTREAM 1" "USE_DELETED_CHAIN 1"
    "HAVE_THREADS 1" "OS_SIMPLE_THREADS 1" "HAVE_AUDIO 1" "SUPPORT_FIXED_FUNCTION 1"
    "DO_PIPELINING 1" "HAVE_IOS_TYPEDEFS 1" "HAVE_IOS_BINARY 1"
    "PHAVE_IOSTREAM 1" "PHAVE_MALLOC_H 1" "PHAVE_SSTREAM 1" "PHAVE_NEW 1"
    "PHAVE_SYS_TYPES_H 1" "PHAVE_STDINT_H 1" "HAVE_RTTI 1" "LINMATH_ALIGN 1"
    "HAVE_SGI_RGB 1" "HAVE_TGA 1" "HAVE_IMG 1" "HAVE_SOFTIMAGE_PIC 1" "HAVE_BMP 1"
    "HAVE_PNM 1" "HAVE_STB_IMAGE 1" "HAVE_FREETYPE 1" "HAVE_NET 1" "WANT_NATIVE_NET 1"
    "PRC_SAVE_DESCRIPTIONS 1" "DO_DCAST 1" "NOTIFY_DEBUG 1" "DO_MEMORY_USAGE 1"
    "DO_COLLISION_RECORDING 1")
if(WIN32)
    list(APPEND _cfg_common
        "HAVE_WGL 1" "DEFAULT_PATHSEP @SEMICOLON@" "SIMPLE_STRUCT_POINTERS 1" "STATIC_INIT_GETENV 1"
        "PHAVE_IO_H 1")
else()
    list(APPEND _cfg_common
        "HAVE_POSIX_THREADS 1" "DEFAULT_PATHSEP \":\"" "HAVE_GETOPT 1" "HAVE_GETOPT_LONG_ONLY 1"
        "PHAVE_GETOPT_H 1" "PHAVE_LINUX_INPUT_H 1" "IOCTL_TERMINAL_WIDTH 1"
        "HAVE_PROC_SELF_EXE 1" "HAVE_PROC_SELF_MAPS 1" "HAVE_PROC_SELF_ENVIRON 1"
        "HAVE_PROC_SELF_CMDLINE 1" "PHAVE_STRING_H 1" "PHAVE_LIMITS_H 1" "PHAVE_STDLIB_H 1"
        "PHAVE_ALLOCA_H 1" "PHAVE_LOCALE_H 1" "PHAVE_SYS_TIME_H 1" "PHAVE_UNISTD_H 1"
        "PHAVE_UTIME_H 1" "PHAVE_DIRENT_H 1" "IS_LINUX 1")
    # makepanda's Android column: no glob(), lockf() or ucontext in older Bionic.
    if(NOT ANDROID)
        list(APPEND _cfg_common "PHAVE_LOCKF 1" "PHAVE_GLOB_H 1" "PHAVE_UCONTEXT_H 1" "HAVE_X11 1")
    endif()
endif()
if(_p3_have_zlib)
    list(APPEND _cfg_common "HAVE_ZLIB 1")
endif()
if(_p3_have_png AND _p3_have_zlib)
    list(APPEND _cfg_common "HAVE_PNG 1")
else()
    set(_p3_have_png OFF)
endif()
set(_cfg "/* dtool_config.h: generated by cmake/Panda3DFromSource.cmake */\n")
foreach(_def IN LISTS _cfg_common)
    string(APPEND _cfg "#define ${_def}\n")
endforeach()
# ";" cannot live inside a CMake list, hence the placeholder.
string(REPLACE "@SEMICOLON@" "\";\"" _cfg "${_cfg}")
file(CONFIGURE OUTPUT "${_p3gen}/dtool_config.h" CONTENT "${_cfg}")

# The client embeds its configuration, so there is no etc/ directory to search for
# .prc files; PANDA_PRC_DIR / PANDA_PRC_PATH still work for overrides.
file(CONFIGURE OUTPUT "${_p3gen}/prc_parameters.h" CONTENT [[
/* prc_parameters.h: generated by cmake/Panda3DFromSource.cmake */
#define DEFAULT_PRC_DIR ""
#define PRC_DCONFIG_TRUST_LEVEL 0
#define PRC_DIR_ENVVARS "PANDA_PRC_DIR"
#define PRC_ENCRYPTED_PATTERNS "*.prc.pe"
#define PRC_ENCRYPTION_KEY ""
#define PRC_EXECUTABLE_ARGS_ENVVAR "PANDA_PRC_XARGS"
#define PRC_EXECUTABLE_PATTERNS ""
#define PRC_INC_TRUST_LEVEL 0
#define PRC_PATH2_ENVVARS ""
#define PRC_PATH_ENVVARS "PANDA_PRC_PATH"
#define PRC_PATTERNS "*.prc"
#define PRC_PUBLIC_KEYS_FILENAME ""
#undef PRC_RESPECT_TRUST_LEVEL
]])

file(CONFIGURE OUTPUT "${_p3gen}/pandaVersion.h" CONTENT [[
/* pandaVersion.h: generated by cmake/Panda3DFromSource.cmake */
#define PANDA_MAJOR_VERSION 1
#define PANDA_MINOR_VERSION 10
#define PANDA_SEQUENCE_VERSION 16
#define PANDA_VERSION 1010016
#define PANDA_NUMERIC_VERSION 1010016
#define PANDA_VERSION_STR "1.10.16"
#define PANDA_ABI_VERSION_STR "1.10"
#define PANDA_DISTRIBUTOR "panda-arena"
#define PANDA_PACKAGE_VERSION_STR ""
#define PANDA_PACKAGE_HOST_URL ""
#define PANDA_OFFICIAL_VERSION
]])

file(CONFIGURE OUTPUT "${_p3gen}/checkPandaVersion.h" CONTENT [[
#ifndef CHECKPANDAVERSION_H
#define CHECKPANDAVERSION_H
#include "dtoolbase.h"
extern EXPCL_DTOOL_DTOOLBASE int panda_version_1_10;
template<typename T>
class CheckPandaVersion {
public:
  int check_version() { return panda_version_1_10; }
};
template class CheckPandaVersion<void>;
#endif
]])
file(CONFIGURE OUTPUT "${_p3gen}/checkPandaVersion.cxx" CONTENT [[
#include "dtoolbase.h"
EXPCL_DTOOL_DTOOLBASE int panda_version_1_10 = 0;
]])

# -------------------------------------------------------------------- sources

# Every header directory makepanda copies into built/include, in one list.
set(_p3_incdirs "${_p3gen}")
foreach(_dir
        dtool/src/dtoolbase dtool/src/dtoolutil dtool/src/prc dtool/src/dconfig
        dtool/src/interrogatedb dtool/metalibs/dtool
        dtool/metalibs/dtoolconfig
        panda/src/pandabase panda/src/express panda/src/downloader panda/src/pipeline
        panda/src/linmath panda/src/putil panda/src/audio panda/src/event panda/src/mathutil
        panda/src/gsgbase panda/src/pnmimage panda/src/nativenet panda/src/net
        panda/src/pstatclient panda/src/gobj panda/src/pgraphnodes panda/src/pgraph
        panda/src/cull panda/src/dgraph panda/src/device panda/src/display panda/src/chan
        panda/src/char panda/src/pnmtext panda/src/text panda/src/movies panda/src/grutil
        panda/src/tform panda/src/collide panda/src/parametrics panda/src/pgui
        panda/src/pnmimagetypes panda/src/recorder panda/src/framework panda/src/glstuff
        panda/metalibs/panda panda/metalibs/pandaexpress
        direct/src/directbase direct/src/deadrec)
    list(APPEND _p3_incdirs "${_p3src}/${_dir}")
endforeach()
if(ARENA_GLES)
    list(APPEND _p3_incdirs "${_p3src}/panda/src/gles2gsg")
else()
    list(APPEND _p3_incdirs "${_p3src}/panda/src/glgsg" "${_p3src}/panda/metalibs/pandagl")
endif()
if(WIN32)
    list(APPEND _p3_incdirs "${_p3src}/panda/src/windisplay" "${_p3src}/panda/src/wgldisplay")
elseif(ANDROID)
    list(APPEND _p3_incdirs "${_p3src}/panda/src/android" "${_p3src}/panda/src/androiddisplay")
elseif(ARENA_GLES)
    list(APPEND _p3_incdirs "${_p3src}/panda/src/x11display" "${_p3src}/panda/src/egldisplay"
                            "${_p3src}/panda/metalibs/pandagles2" "${_p3_gles2_include}")
else()
    list(APPEND _p3_incdirs "${_p3src}/panda/src/x11display" "${_p3src}/panda/src/glxdisplay")
endif()

# Common compile settings: makepanda's GCC flags for optimize level 3.
add_library(p3_settings INTERFACE)
target_include_directories(p3_settings SYSTEM INTERFACE ${_p3_incdirs})
target_compile_definitions(p3_settings INTERFACE LINK_ALL_STATIC)
if(ARENA_GLES)
    target_compile_definitions(p3_settings INTERFACE ARENA_GLES)
endif()
if(WIN32)
    # Panda3D uses WIN32_VC to mean "the Windows code path" (makepanda passes
    # /DWIN32_VC /DWIN64_VC); MSVC-only syntax is guarded by _MSC_VER instead.
    # Some of these switches change class layouts, so every user of the headers
    # (not just Panda's own sources) must see them: INTERFACE.
    target_compile_definitions(p3_settings INTERFACE _WIN32_WINNT=0x0601 WINVER=0x0601
                                                     WIN32_VC WIN64_VC WIN64)
endif()

if(ANDROID)
    set(_p3_pic ON)
else()
    set(_p3_pic OFF)
endif()

function(_p3_module name building)
    cmake_parse_arguments(M "EXCEPTIONS" "" "SOURCES;DEFINES;LIBS" ${ARGN})
    add_library(${name} OBJECT)
    foreach(_src IN LISTS M_SOURCES)
        if(IS_ABSOLUTE "${_src}")
            target_sources(${name} PRIVATE "${_src}")
        else()
            target_sources(${name} PRIVATE "${_p3src}/${_src}")
        endif()
    endforeach()
    target_link_libraries(${name} PRIVATE p3_settings Threads::Threads ${M_LIBS})
    target_compile_definitions(${name} PRIVATE BUILDING_${building} ${M_DEFINES})
    # Android loads the game as a shared library: position-independent code.
    set_target_properties(${name} PROPERTIES CXX_STANDARD 11 CXX_EXTENSIONS ON
                                             POSITION_INDEPENDENT_CODE ${_p3_pic})
    target_compile_options(${name} PRIVATE -w -ftemplate-depth-70 -fno-strict-aliasing
        -ffast-math -fno-finite-math-only -fno-unsafe-math-optimizations
        -fno-stack-protector $<$<COMPILE_LANGUAGE:CXX>:-Wno-reorder>
        $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:-O2>)
    if(NOT M_EXCEPTIONS)
        target_compile_options(${name} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>)
    endif()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i.86")
        target_compile_options(${name} PRIVATE -msse2)
    endif()
    if(WIN32)
        target_compile_options(${name} PRIVATE -Wa,-mbig-obj)  # glgsg/pgraph are huge
    endif()
    set(_p3_objects ${_p3_objects} $<TARGET_OBJECTS:${name}> PARENT_SCOPE)
endfunction()

set(_p3_objects)

_p3_module(p3dtool DTOOL SOURCES
    dtool/src/dtoolbase/p3dtoolbase_composite1.cxx dtool/src/dtoolbase/p3dtoolbase_composite2.cxx
    dtool/src/dtoolbase/lookup3.c dtool/src/dtoolbase/indent.cxx
    dtool/src/dtoolutil/p3dtoolutil_composite1.cxx dtool/src/dtoolutil/p3dtoolutil_composite2.cxx
    dtool/metalibs/dtool/dtool.cxx)  # checkPandaVersion.cxx is #included by a composite
_p3_module(p3dtoolconfig DTOOLCONFIG SOURCES
    dtool/src/prc/p3prc_composite1.cxx dtool/src/prc/p3prc_composite2.cxx
    dtool/metalibs/dtoolconfig/dtoolconfig.cxx)
_p3_module(p3pandaexpress PANDAEXPRESS SOURCES
    panda/src/pandabase/pandabase.cxx
    panda/src/express/p3express_composite1.cxx panda/src/express/p3express_composite2.cxx
    panda/src/downloader/p3downloader_composite1.cxx panda/src/downloader/p3downloader_composite2.cxx
    panda/metalibs/pandaexpress/pandaexpress.cxx
    LIBS $<$<BOOL:${_p3_have_zlib}>:ZLIB::ZLIB>)

set(_p3_panda_sources)
foreach(_m pipeline:2 linmath:2 putil:2 audio:1 event:2 mathutil:2 gsgbase:1 pnmimage:2
           nativenet:1 net:2 pstatclient:2 gobj:2 pgraphnodes:2 pgraph:4 cull:2 dgraph:2
           device:2 display:2 chan:2 char:2 text:2 movies:1 grutil:2 tform:2 collide:2
           parametrics:2 pgui:2 recorder:2)
    string(REPLACE ":" ";" _m "${_m}")
    list(GET _m 0 _dir)
    list(GET _m 1 _count)
    foreach(_i RANGE 1 ${_count})
        list(APPEND _p3_panda_sources panda/src/${_dir}/p3${_dir}_composite${_i}.cxx)
    endforeach()
endforeach()
list(APPEND _p3_panda_sources
    panda/src/pipeline/contextSwitch.c
    panda/src/pnmimage/convert_srgb_sse2.cxx
    panda/src/pgraph/nodePath.cxx
    panda/src/display/graphicsStateGuardian.cxx
    panda/src/grutil/multitexReducer.cxx
    panda/src/pnmtext/p3pnmtext_composite1.cxx
    panda/metalibs/panda/panda.cxx)
if(WIN32 OR ANDROID)
    set(_p3_x11)
else()
    set(_p3_x11 USE_X11)
endif()
_p3_module(p3panda PANDA SOURCES ${_p3_panda_sources}
    DEFINES ${_p3_x11}
    LIBS ${_p3_freetype} $<$<BOOL:${_p3_have_zlib}>:ZLIB::ZLIB>)
# pnmimagetypes is the one module makepanda builds with exceptions (libpng).
_p3_module(p3pnmimagetypes PANDA EXCEPTIONS SOURCES
    panda/src/pnmimagetypes/p3pnmimagetypes_composite1.cxx
    panda/src/pnmimagetypes/p3pnmimagetypes_composite2.cxx
    LIBS $<$<BOOL:${_p3_have_png}>:PNG::PNG> $<$<BOOL:${_p3_have_zlib}>:ZLIB::ZLIB>)

if(WIN32)
    _p3_module(p3windisplay PANDAWIN SOURCES
        panda/src/windisplay/p3windisplay_composite1.cxx panda/src/windisplay/winDetectDx9.cxx)
    _p3_module(p3pandagl PANDAGL SOURCES
        panda/src/glgsg/config_glgsg.cxx panda/src/glgsg/glgsg.cxx
        panda/src/wgldisplay/p3wgldisplay_composite1.cxx panda/metalibs/pandagl/pandagl.cxx)
elseif(ANDROID)
    # OpenGL ES 2/3 in an Android window (EGL). makepanda 1.10 only builds this
    # display for OpenGL ES 1; the sources support ES 2 with OPENGLES_2. The
    # app glue runs android_main() (client/src/app/AndroidMain.cpp) on its
    # own thread; the window reads its events.
    _p3_module(p3pandagles2 PANDAGLES2 SOURCES
        panda/src/gles2gsg/config_gles2gsg.cxx panda/src/gles2gsg/gles2gsg.cxx
        panda/src/androiddisplay/p3androiddisplay_composite1.cxx
        panda/src/android/android_native_app_glue.c
        DEFINES OPENGLES_2 HAVE_EGL)
else()
    _p3_module(p3x11display PANDAX11 SOURCES panda/src/x11display/p3x11display_composite1.cxx
        DEFINES USE_X11)
    if(ARENA_GLES)
        _p3_module(p3pandagles2 PANDAGLES2 SOURCES
            panda/src/gles2gsg/config_gles2gsg.cxx panda/src/gles2gsg/gles2gsg.cxx
            panda/src/egldisplay/p3egldisplay_composite1.cxx panda/metalibs/pandagles2/pandagles2.cxx
            DEFINES OPENGLES_2 HAVE_EGL USE_X11)
    else()
        _p3_module(p3pandagl PANDAGL SOURCES
            panda/src/glgsg/config_glgsg.cxx panda/src/glgsg/glgsg.cxx
            panda/src/glxdisplay/p3glxdisplay_composite1.cxx panda/metalibs/pandagl/pandagl.cxx
            DEFINES HAVE_GLX USE_X11)
    endif()
endif()
# HAVE_GL: in static builds PandaFramework calls init_libpandagl() itself. The
# OpenGL ES display registers itself (its static initialisers; the app also
# calls its init function, see App.cpp).
if(ARENA_GLES)
    set(_p3_framework_gl)
else()
    set(_p3_framework_gl HAVE_GL)
endif()
_p3_module(p3framework FRAMEWORK SOURCES panda/src/framework/p3framework_composite1.cxx
    DEFINES ${_p3_framework_gl})
_p3_module(p3deadrec DIRECT SOURCES direct/src/deadrec/p3deadrec_composite1.cxx)

# ----------------------------------------------------------------- the target

# Panda3D::Panda3D carries includes, defines and system libraries. The object
# files themselves are linked into executables only (Panda3D_OBJECTS, see
# arena_link_panda3d() in CMakeLists.txt), so no static library in between
# drops an object whose static initialisers registers types or config.
add_library(panda3d_from_source INTERFACE)
target_link_libraries(panda3d_from_source INTERFACE p3_settings ${_p3_freetype}
    $<$<BOOL:${_p3_have_png}>:PNG::PNG> $<$<BOOL:${_p3_have_zlib}>:ZLIB::ZLIB>
    ${_p3_platform_libs} Threads::Threads)
# The dcparser headers (vendored) must be read the way Panda builds them.
target_compile_definitions(panda3d_from_source INTERFACE WITHIN_PANDA)
add_library(Panda3D::Panda3D ALIAS panda3d_from_source)
set(Panda3D_OBJECTS ${_p3_objects})

set(Panda3D_FOUND TRUE)
set(Panda3D_VERSION 1.10.16)
set(Panda3D_INCLUDE_DIR "${_p3gen}")
set(Panda3D_HAVE_CG OFF)
set(Panda3D_FROM_SOURCE ON)
if(ARENA_GLES)
    set(_p3_renderer "OpenGL ES")
else()
    set(_p3_renderer "OpenGL")
endif()
message(STATUS "Panda3D ${Panda3D_VERSION} will be built from ${_p3src} "
               "(${_p3_renderer}, zlib: ${_p3_have_zlib}, png: ${_p3_have_png})")
