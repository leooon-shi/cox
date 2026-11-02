# CoxInstall.cmake
#
# Helpers to make Cox's static libraries installable and consumable via
# find_package(Cox) + target_link_libraries(app cox::<module>).
#
# Design:
#   * Each module calls cox_install_module(<target> HEADERS <h1> <h2> ...).
#     That swtiches the target's include directories to the BUILD_INTERFACE /
#     INSTALL_INTERFACE form (so the same target works in-tree and installed),
#     adds the target to the shared CoxTargets export set, and installes its
#     headers under include/cox/ preserving any subdirectory (e.g. poller/).
#   * The top-level CMakeLists then calls cox_install_package() once to install
#     the export set, generate CoxConfig.cmake + version file, and create the
#     cox:: ALIAS namespace for in-tree consumers.
#
# Headers are installed FLAT under a single include/cox/ root (preserving
# relative subdirs), matching how the sources include them ("event_loop.h"
# "poller/epoll_poller.h"). Consumers add one include dir: <prefix>/include/cox.

include(GNUInstallDirs)

# Register a module target for install + export and install its headers.
#   cox_install_module(cox_reactor HEADERS a.h poller/b.h ...)
function(cox_install_module _target)
    cmake_parse_arguments(ARG "" "" "HEADERS" ${ARGN})
    
    # Export under the short name: internal target cox_http -> cox::http (not
    # cox::cox_http). EXPORT_NAME strips the cox_ prefix for the public alias
    # while keepting the internal target name collision-free.
    string(REGEX REPLACE "^cox_" "" _short "${_target}")
    set_target_properties(${_target} PROPERTIES EXPORT_NAME "${_short}")

    # Public include dir: the source tree when building, include/cox when
    # installed. Without the generator-expression split, an installed target
    # would still point at the (absent) build-tree path.
    target_include_directories(${_target} PUBLIC
        $<BUILD_INTERFACE::${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE::${CMAKE_INSTALL_INCLUDEDIR}/cox>)

    # Add to the shared export set; install the archive.
    install(TARGETS ${_target}
            EXPORT CoxTargets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

    # Install headers under include/cox/, preserving subdirectories so a header
    # included as "poller/epoll_poller.h" resolves after install too.
    foreach(_h ${ARG_HEADERS})
        get_filename_component(_subdir "${_h}" DIRECTORY)
        install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/${_h}"
                DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/cox/${_subdir}")
    endforeach()
endfunction()

# Install the export set + generated package config. Call ONCE from the top
# CMakeLists after all modules have been added.
function(cox_install_package)
    include(CMakePackageConfigHelpers)

    set(_cmake_dir "${CMAKE_INSTALL_LIBDIR}/cmake/Cox")

    # Export the targets under the cox:: namespace for install consumers
    install(EXPORT CoxTargets
            FILE CoxTargets.cmake
            NAMESPACE cox::
            DESTINATION "${_cmake_dir}")

    # Generate CoxConfig.cmake from the template; it re-finds transitive
    # external dependencies (OpenSSL always; c-ares when Cox was built with it)
    # before including the exported targets
    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CoxConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/CoxConfig.cmake"
        INSTALL_DESTINATION "${_cmake_dir}")

    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/CoxConfigVersion.cmake"
        VERSION "${PROJECT_VERSION}"
        COMPATIBILITY SameMajorVersion)
    
    install(FILES
            "${CMAKE_CURRENT_BINARY_DIR}/CoxConfig.cmake"
            "${CMAKE_CURRENT_BINARY_DIR}/CoxConfigVersion.cmake"
            DESTINATION "${_cmake_dir}")
endfunction()

