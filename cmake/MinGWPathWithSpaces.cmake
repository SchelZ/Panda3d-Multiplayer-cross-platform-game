# Lets MinGW GCC link from a folder whose path has a space (included from
# CMakeLists.txt before project()). See the comment there.

set(_arena_cc "${CMAKE_C_COMPILER}")
if(NOT _arena_cc)
    set(_arena_cc gcc)
endif()
if(NOT IS_ABSOLUTE "${_arena_cc}")
    find_program(_arena_cc_path NAMES "${_arena_cc}")
    set(_arena_cc "${_arena_cc_path}")
    unset(_arena_cc_path CACHE)
endif()

if(_arena_cc MATCHES " ")
    execute_process(COMMAND "${_arena_cc}" -dumpspecs
                    OUTPUT_VARIABLE _arena_specs RESULT_VARIABLE _arena_rc ERROR_QUIET)
    string(REPLACE "\r" "" _arena_specs "${_arena_specs}")
    if(_arena_rc EQUAL 0 AND _arena_specs MATCHES "\\*endfile:\n([^\n]*)")
        set(_arena_endfile "${CMAKE_MATCH_1}")
        if(_arena_endfile MATCHES "%:if-exists\\(default-manifest\\.o%s\\)")
            execute_process(COMMAND "${_arena_cc}" -print-file-name=default-manifest.o
                            OUTPUT_VARIABLE _arena_manifest OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
            # %s finds it (one argument, spaces and all); no file, no manifest.
            set(_arena_with "")
            if(IS_ABSOLUTE "${_arena_manifest}" AND EXISTS "${_arena_manifest}")
                set(_arena_with "default-manifest.o%s")
            endif()
            string(REPLACE "%:if-exists(default-manifest.o%s)" "${_arena_with}" _arena_endfile "${_arena_endfile}")
            set(_arena_specs_file "${CMAKE_BINARY_DIR}/mingw-path-with-spaces.specs")
            file(WRITE "${_arena_specs_file}" "*endfile:\n${_arena_endfile}\n\n")
            set(_arena_flag "\"-specs=${_arena_specs_file}\"")
            foreach(_arena_kind EXE SHARED MODULE)
                string(APPEND CMAKE_${_arena_kind}_LINKER_FLAGS_INIT " ${_arena_flag}")
                # A cache from an earlier, failed configure ignores the _INIT value.
                if(DEFINED CACHE{CMAKE_${_arena_kind}_LINKER_FLAGS} AND
                   NOT "$CACHE{CMAKE_${_arena_kind}_LINKER_FLAGS}" MATCHES "mingw-path-with-spaces")
                    set_property(CACHE CMAKE_${_arena_kind}_LINKER_FLAGS PROPERTY VALUE
                                 "$CACHE{CMAKE_${_arena_kind}_LINKER_FLAGS} ${_arena_flag}")
                endif()
            endforeach()
            message(STATUS "MinGW in a path with a space (${_arena_cc}): linking with ${_arena_specs_file}")
        endif()
    endif()
endif()
