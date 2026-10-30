# third_party/ -- vendored dependencies.
#
# This file is the SIGNLE place that wires third_party code into the build.
# Each dependency is either always-on (test infrastructure) or feature-gated
# behind a COX_WITH_* option so optional dependencies (HTTP/2) are opt-in and
# never required to build the core library.
#
# Integration policy (see third_party/README.md for the full rationale):
#   * Submodules are pinned by commit in .gitmodules.
#   * CMake-native deps are added with cox_add_vendored() (warning-isolated).
#   * Non-CMake deps (autotools, Rust, ...) are integrated per-dependency and
#     exposed as an IMPORTED target so the rest of the tree depends on a target,
#     not on paths.

include(CoxAddVendored)

# --- Test infrastructure (added only when BUILD_TESTS is ON) ----------------
# googletest is CMake-native, so cox_add_vendored() handles it directly.
if(BUILD_TESTS)
    cox_add_vendored(googletest)
endif()

# --- Optional, feature-gated dependencies -----------------------------------
# These are scaffolding: the options and the wiring pattern exist now so that
# adding the submodule later is a localised change, not a refactor. Each block
# stays inert until both the option is ON and the submodule is checked out.

if(COX_WITH_HTTP2)
    # nghttp2: CMake-capable. Disable its apps/examples/tests; we only want the
    # library target (nghttp2::nghttp2 / nghttp2_static).
    set(ENABLE_LIB_ONLY ON CACHE BOOL "" FORCE)
    set(ENABLE_APP OFF CACHE BOOL "" FORCE)
    cox_add_vendored(nghttp2 EXCLUDE_FROM_ALL)
endif()
