# Toolchain of the android-* presets: finds the Android NDK and uses its own
# toolchain file. Looks at ANDROID_NDK_HOME, ANDROID_NDK_ROOT and ANDROID_NDK,
# then for the newest NDK in <SDK>/ndk/ (the SDK from ANDROID_HOME or
# ANDROID_SDK_ROOT, or Android Studio's default folder). The build folder
# remembers it (ARENA_ANDROID_NDK), so a later build that re-runs CMake finds
# it again without those variables (a new console).

set(_arena_ndk "")
if(ARENA_ANDROID_NDK AND EXISTS "${ARENA_ANDROID_NDK}/build/cmake/android.toolchain.cmake")
    set(_arena_ndk "${ARENA_ANDROID_NDK}")
endif()
foreach(_var ANDROID_NDK_HOME ANDROID_NDK_ROOT ANDROID_NDK)
    if(NOT _arena_ndk AND DEFINED ENV{${_var}})
        file(TO_CMAKE_PATH "$ENV{${_var}}" _dir)
        if(EXISTS "${_dir}/build/cmake/android.toolchain.cmake")
            set(_arena_ndk "${_dir}")
        endif()
    endif()
endforeach()

if(NOT _arena_ndk)
    set(_sdks "$ENV{ANDROID_HOME}" "$ENV{ANDROID_SDK_ROOT}")
    if(CMAKE_HOST_WIN32)
        list(APPEND _sdks "$ENV{LOCALAPPDATA}/Android/Sdk")
    elseif(CMAKE_HOST_APPLE)
        list(APPEND _sdks "$ENV{HOME}/Library/Android/sdk")
    else()
        list(APPEND _sdks "$ENV{HOME}/Android/Sdk")
    endif()
    foreach(_sdk IN LISTS _sdks)
        if(_arena_ndk OR NOT _sdk)
            continue()
        endif()
        file(TO_CMAKE_PATH "${_sdk}" _sdk)
        file(GLOB _found "${_sdk}/ndk/*/build/cmake/android.toolchain.cmake"
                         "${_sdk}/ndk-bundle/build/cmake/android.toolchain.cmake")
        # The newest by the NDK's own version (source.properties), not by the
        # folder name: "android-ndk-r27d" and "29.0.14206865" both happen.
        set(_best_version "")
        foreach(_file IN LISTS _found)
            get_filename_component(_dir "${_file}/../../.." ABSOLUTE)
            set(_version 0)
            if(EXISTS "${_dir}/source.properties")
                file(STRINGS "${_dir}/source.properties" _rev REGEX "Pkg\\.Revision")
                if(_rev MATCHES "([0-9]+(\\.[0-9]+)*)")
                    set(_version "${CMAKE_MATCH_1}")
                endif()
            endif()
            if(NOT _arena_ndk OR _version VERSION_GREATER _best_version)
                set(_arena_ndk "${_dir}")
                set(_best_version "${_version}")
            endif()
        endforeach()
    endforeach()
endif()

if(NOT _arena_ndk)
    message(FATAL_ERROR
        "Android NDK not found. Install \"NDK (Side by side)\" with Android Studio's SDK Manager "
        "(SDK Tools tab), or set ANDROID_NDK_HOME to the NDK folder (the one that contains "
        "build/cmake/android.toolchain.cmake).")
endif()

set(ARENA_ANDROID_NDK "${_arena_ndk}" CACHE PATH "The Android NDK this build folder uses")
set(ENV{ANDROID_NDK_HOME} "${_arena_ndk}")  # for CMake's test projects (try_compile)
include("${_arena_ndk}/build/cmake/android.toolchain.cmake")
