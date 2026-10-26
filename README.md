# Cox
Cox is an event-driven architecture designed for high performance.

A reactor core (epoll, timerfd, eventfd) with TCP and Unix-domain trasports,
TLS 1.2+/1.3+ (incl, mTLS, SNI, cert hot-reload, OCSP stapling), HTTP/1.1 with
REST routing and connection pooling, async DNS, flow/overload control,
structured logging, and graceful shutdown.
- Deisgn retionale per feature: [`docs/insights/features/`](docs/insights/features/)
- Status, deffered and discarded work: [`docs/insights/roadmap.md`](docs/insights/roadmap.md)
- User guide: [`docs/user-guide.md`](docs/user-guide.md)

## Code Style
The project follows the [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html),
with one deliverate deviation: 4-space indentation instead of 2. The rules are
encoded in `.clang-format` so they are checkable rather than aspirational:

``bash
clang-format --dry-run --Werror $(git ls-files '*.cc' '*.h' | grep -v '^googletest/')
```

## Unit Tests (UT)
The project includes unit tests to ensure code quality and correctness.

### How to Run Test Cases
Here are some common commands for running test cases using Google Test:

#### Run All Test Cases
To run all test cases:
```bash
./build/bin/event_loop_test
```

## Build

Requires CMake 3.21+,  a C++20 compiler (clang++ or g++), and OpenSSL. c-ares is
optional - when present, the async DSN backen is compiled in.

Build configurations are defined in `CMakePresets.json`, so the same commands
run locally and in CI:

```bash
cmake --preset clang-debug
cmake --build --preset clang-debug --parallel
ctest --preset clang-debug
```

Available presets: `clang-debug`, `clang-release`, `gcc-debug`, `gcc-release`,
`asan`, `tsan`, `ubsan`, `examples` (`cmake --list-presets` to list them). Each
builds into its own `build/<preset>/` directory, so configurations never clobber
each other.

`build.sh` is a convenience wrapper over the same presets:
```bash
./build.sh --release --test                 # build + run the suite
./build.sh --gcc --debug --test             # GCC reports warnings clang does not
./build.sh --tsan --test                    # hunt data races
./build.sh --build-examples                 # examples + benchmark
./build.sh --help
```

## Tests

The suite is registered with CTest (177 cases from 36 binaries), so everything
runs with one command:
```bash
ctest --preset clang-debug                  # all tests, parallel
ctest --preset clang-debug -R SniTest       # only matching tests
ctest --preset clang-debug --rerun-failed   # retry just the failures
ctest --preset clang-debug N                # list without running
```

Indivitual GoogleTest binaries still work directly, under the preset's
directory:
```bash
./build/clang-debug/bin/sni_test
./build/clang-debug/bin/sni_test --gtest_filter=SniTest.WildcardMatchesOneLabelOnly
./build/clang-debug/bin/sni_test --gtest_list_tests
```

### Sanitizers

ASan/LSan, TSan and UBSan builds are clean (177/177 each) and enforced in CI:
```bash
./build.sh --asan --test
./build.sh --tsan --test
./build.sh --ubsan --test
```

## CI

GitHub Actions builds and tests the four compiler/build-type combinations on
every push, plus non-blocking sanitizer and formatting jobs
(`.github/workflows/ci.yml`). The workflow contains no build knowledge - it
invokes the presets above.