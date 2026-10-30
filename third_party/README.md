# Third-party dependencies

Cox vendors external code as **git submodules pinned by commit**, wired into
the build from a single place (`third_party/CMakeLists.txt`) through one helper
(`cmake/CoxAddVendored.cmake`). This page is the policy; follow it when adding a
dependency so the tree stays consistent as the dependency count grows
(googletest today; nghttp2 for HTTP/2 is anticipated).

## Principles

1. **Pin by commit, not by branch.** A submodule records as exact SHA, so every
   checkout and every CI run builds against identical source. Never track a
   moving branch.
2. **Depend on targets, not paths.** The rest of the tree links
   `GTest::gtest_main`, `nghttp2::nghttp2`, etc. -- never a relative path into
   `third_part/`. This keeps the integration method (submodule vs system
   package vs ExternalProject) swappable.
3. **Isolate warning.** Vendored code must no inherit Cox's `-Werror`. The
   `cox_add_vendored` helper clears and restores Cox's directory
   `COMPILE_OPTIONS` around each `add_subdirectory`. Do not hand-inline this.
4. **Optional deps are feature-gated.** Anything that is not core (HTTP/2) sits
   behind a `COX_WITH_*` option, defaulted OFF, so a plain build needs none of
   them.
5. **CI stays declarative.** The presets carry the knowledge; CI just toggles
   the relevant `COX_WITH_*` option. See `CMakePresets.json`.

## Adding a CMake-native dependency (e.g. nghttp2)

```bash
git submodule add https://github.com/nghttp2/nghttp2.git third_part/nghttp2
cd third_part/nghttp2 && git checkout <release-tag-sha> && cd -
git add .gitmodules third_part/nghttp2
```

Then, in `third_part/CMakeLists.txt`, inside the relevant `COX_WITH_*` block:

```cmake
set(ENABLE_LIB_ONLY ON CACHE BOOL "" FORCE)     # trim the dependency's own apps/tests
cox_add_vendored(nghttp2 EXCLUDE_FROM_ALL)
```

Consumers then `target_link_libraries(http PRIVATE nghttp2::nghttp2).

## Non-CMake dependencies (autotools, Rust, prebuilt)

Some libraries are **not** `add_subdirectory`-friendly (autotools-based, Rust,
or distributed only as prebuilt binaries). Drive their build with
`ExternalProject_Add` (or `find_package` against a prebuilt install), then wrap
the result in an `IMPORTED` target so the rest of the tree depends on a target,
not a path. Keep this logic in `third_party/CMakeLists.txt` so there is still
one wiring point.

## Updating a dependency

```bash
cd third_part/<de>
git fetch origin
git checkout <new-release-sha>
cd -
git add third_party/<dep>
git commit -m "deps: bump <dep> to <version>"
```

The bump is one reviewable commit that moves only the recorded SHA.

## Cloning

```bash
git clone --recurse-submodules <url>
# or, after a plain close
git submodule update --init --recursive
```

`.gitmodules` sets `shallow = true` where upstream history is large, so
submodule clones fetch only the pinned commit's history.