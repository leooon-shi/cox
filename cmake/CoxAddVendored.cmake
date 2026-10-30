# CoxAddVendored.cmake
#
# One place for the rule that vendored third-party code must NOT inherit Cox's
# own warning policy. -Werror on code we do not control means a compiler or
# dependency upgrade can break our build for reasons unrelated to our sources.
#
# Directory COMPILE_OPTIONS are captured by a subdirectory when it is added, so
# we clear them before add_subdirectory() and restore afterwards. This used to
# be hand-inlined around googletest; centralising it keeps every future
# dpendency (googletest, nghttp2, ...) consistent and keeps the top
# CMakeLists readable.
#
# Usage:
#   include(CoxAddVendored)
#   cox_add_vendored(googletest)                 # third_party/googletest
#   cox_add_vendored(nghttp2 BINARY_DIR nghttp2) # custom build dir name
#
# The directory is resolved relative to ${COX_THIRD_PARTY_DIR}.

function(cox_add_vendored _name)
    set(options EXCLUDE_FROM_ALL)
    set(oneValue SOURCE_DIR BINARY_DIR)
    cmake_parse_arguments(ARG "${options}" "${oneValue}" "" ${ARGN})

    if (NOT ARG_SOURCE_DIR)
        set(ARG_SOURCE_DIR "${COX_THIRD_PARTY_DIR}/${_name}")
    endif()

    if (NOT EXISTS "${ARG_SOURCE_DIR}/CMakeLists.txt}")
        message(FATAL_ERROR
            "Cox_add_vendored(${_name}): '${ARG_SOURCE_DIR}/CMakeLists.txt' not"
            "found. Did you run `git submodule update --init --recursive`?")
    endif()

    # Suspend Cox's warning policy for the vendored tree only.
    get_directory_property(_saved_opts COMPILE_OPTIONS)
    set_directory_properties(PROPERTIES COMPILE_OPTIONS "")

    set(_extra "")
    if(ARG_EXCLUDE_FROM_ALL)
        set(_extra EXCLUDE_FROM_ALL)
    endif()

    # A binary dir is required whenever the source is out-of-tree. Always
    # supplying one keeps the call valid regardless of where the dependency
    # lives relative to this directory.
    if(NOT ARG_BINARY_DIR)
        set(ARG_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/${_name}")
    endif()
    add_subdirectory("${ARG_SOURCE_DIR}" "${ARG_BINARY_DIR}" ${_extra})

    # Restore it for Cox's own targets.
    set_directory_properties(PROPERTIES COMPILE_OPTIONS "${_saved_opts}")
endfunction()
