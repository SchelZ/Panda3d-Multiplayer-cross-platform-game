# Finds an installed Panda3D C++ SDK (1.10.x, built with g++) and defines the imported
# target Panda3D::Panda3D. Only used with -DARENA_PANDA3D_FROM_SOURCE=OFF on Linux
# (the official Windows SDK is built with MSVC, which g++ cannot link against).
#
# Search order: -DPANDA3D_ROOT=..., $ENV{PANDA3D_ROOT}, then the default install
# locations: /usr/include/panda3d + /usr/lib/x86_64-linux-gnu/panda3d (.deb SDK),
#            /opt/panda3d*, /usr/local (makepanda --installer / install.py)
#
# Panda3D's headers depend on how the SDK was configured (dtool_config.h):
#   HAVE_EIGEN -> linmath includes <Eigen/Dense>; install libeigen3-dev.
# No Python headers are needed: the only Panda3D sources that include <Python.h>
# (dcparser, CConnectionRepository) are vendored with Python support filtered out.
#
# Result variables: Panda3D_FOUND, Panda3D_VERSION, Panda3D_INCLUDE_DIR,
#                   Panda3D_LIBRARY_DIR, Panda3D_HAVE_PYTHON, Panda3D_HAVE_CG.

set(_p3_hints ${PANDA3D_ROOT} $ENV{PANDA3D_ROOT})
file(GLOB _p3_opt_dirs "/opt/panda3d*")
list(APPEND _p3_hints ${_p3_opt_dirs} /usr/local /usr)

find_path(Panda3D_INCLUDE_DIR
    NAMES pandaFramework.h
    HINTS ${_p3_hints}
    PATH_SUFFIXES include include/panda3d panda3d
    DOC "Panda3D include directory (contains pandaFramework.h)")

find_library(Panda3D_PANDA_LIBRARY
    NAMES libpanda panda
    HINTS ${_p3_hints}
    PATH_SUFFIXES lib lib64 lib/panda3d lib/x86_64-linux-gnu/panda3d lib64/panda3d
    DOC "Panda3D core library")

if(Panda3D_PANDA_LIBRARY)
    get_filename_component(Panda3D_LIBRARY_DIR "${Panda3D_PANDA_LIBRARY}" DIRECTORY)
endif()

# Version, from pandaVersion.h.
if(Panda3D_INCLUDE_DIR AND EXISTS "${Panda3D_INCLUDE_DIR}/pandaVersion.h")
    file(STRINGS "${Panda3D_INCLUDE_DIR}/pandaVersion.h" _p3_ver
         REGEX "^#define PANDA_VERSION_STR \"[^\"]+\"")
    string(REGEX REPLACE ".*\"([^\"]+)\".*" "\\1" Panda3D_VERSION "${_p3_ver}")
endif()

set(_p3_required p3framework panda pandaexpress p3dtool p3dtoolconfig p3direct)
set(Panda3D_LIBRARIES)
set(_p3_missing)
foreach(_lib IN LISTS _p3_required)
    find_library(Panda3D_${_lib}_LIBRARY NAMES lib${_lib} ${_lib}
                 HINTS "${Panda3D_LIBRARY_DIR}" NO_DEFAULT_PATH)
    if(Panda3D_${_lib}_LIBRARY)
        list(APPEND Panda3D_LIBRARIES "${Panda3D_${_lib}_LIBRARY}")
    else()
        list(APPEND _p3_missing ${_lib})
    endif()
endforeach()
# Linux SDKs built with Python leave the Python C-API unresolved in the C++
# libraries; p3pystub provides harmless stand-ins for programs without Python.
find_library(Panda3D_p3pystub_LIBRARY NAMES libp3pystub p3pystub
             HINTS "${Panda3D_LIBRARY_DIR}" NO_DEFAULT_PATH)

set(Panda3D_HAVE_PYTHON OFF)
set(Panda3D_HAVE_EIGEN OFF)
set(Panda3D_HAVE_CG OFF)
set(_p3_extra_includes)
if(Panda3D_INCLUDE_DIR AND EXISTS "${Panda3D_INCLUDE_DIR}/dtool_config.h")
    file(STRINGS "${Panda3D_INCLUDE_DIR}/dtool_config.h" _p3_cfg REGEX "^#define HAVE_(PYTHON|EIGEN|CG) ")
    if(_p3_cfg MATCHES "HAVE_PYTHON 1")
        set(Panda3D_HAVE_PYTHON ON)
    endif()
    if(_p3_cfg MATCHES "HAVE_EIGEN 1")
        set(Panda3D_HAVE_EIGEN ON)
    endif()
    if(_p3_cfg MATCHES "HAVE_CG 1")
        set(Panda3D_HAVE_CG ON)
    endif()
endif()

if(Panda3D_HAVE_EIGEN)
    find_path(Panda3D_EIGEN_INCLUDE_DIR NAMES Eigen/Dense
              HINTS "${Panda3D_INCLUDE_DIR}" "${Panda3D_INCLUDE_DIR}/.." /usr/include/eigen3
                    /usr/local/include/eigen3
              PATH_SUFFIXES eigen3)
    if(NOT Panda3D_EIGEN_INCLUDE_DIR)
        message(SEND_ERROR "This Panda3D SDK was built with Eigen; install Eigen 3 "
            "(e.g. libeigen3-dev) or set Panda3D_EIGEN_INCLUDE_DIR.")
    endif()
    list(APPEND _p3_extra_includes ${Panda3D_EIGEN_INCLUDE_DIR})
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Panda3D
    REQUIRED_VARS Panda3D_INCLUDE_DIR Panda3D_PANDA_LIBRARY Panda3D_LIBRARIES
    VERSION_VAR Panda3D_VERSION)

if(_p3_missing)
    message(SEND_ERROR "Panda3D SDK at ${Panda3D_LIBRARY_DIR} lacks: ${_p3_missing}")
endif()

if(Panda3D_FOUND AND NOT TARGET Panda3D::Panda3D)
    add_library(Panda3D::Panda3D INTERFACE IMPORTED)
    # WITHIN_PANDA: the dcparser headers (dcClass.h, dcField.h...) can also be
    # compiled standalone; libp3direct was built with this defined, and it
    # changes class layouts, so every user of those headers must define it.
    set_target_properties(Panda3D::Panda3D PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${Panda3D_INCLUDE_DIR};${_p3_extra_includes}"
        INTERFACE_COMPILE_DEFINITIONS "WITHIN_PANDA")
    set(_p3_link ${Panda3D_LIBRARIES})
    if(Panda3D_p3pystub_LIBRARY)
        list(APPEND _p3_link "${Panda3D_p3pystub_LIBRARY}")
    endif()
    find_package(Threads REQUIRED)
    list(APPEND _p3_link Threads::Threads)
    set_target_properties(Panda3D::Panda3D PROPERTIES INTERFACE_LINK_LIBRARIES "${_p3_link}")
endif()

mark_as_advanced(Panda3D_INCLUDE_DIR Panda3D_PANDA_LIBRARY
                 Panda3D_EIGEN_INCLUDE_DIR Panda3D_p3pystub_LIBRARY)
