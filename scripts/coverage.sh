#!/usr/bin/env bash
# Measure test-suite coverage with Clang source-based coverage + llvm-cov.
#
# Roadmap CI/CD item 6: replace "the suite is thorough" with a number. This
# builds the coverage preset, runs the whole suite (each test binary writing
# its own profraw via LLVM_ProFILE_FILE), merges them, and prints a per-file
# line/region summary. Pass --html for a browsable report under build/coverage.
#
# Usage:
#   scripts/coverage.sh            # text summary
#   scripts/coverage.sh --html     # also write build/coverage/html/index.html
set -euo pipefail

cd "$(dirname "$0")/.."

BUILD_DIR="build/coverage"
PROFDIR=$BUILD_DIR/profraw
WANT_HTML=0
[[ "${1:-}" == "--html" ]] && WANT_HTML=1

echo "== configure + build (coverage preset) =="
cmake --preset coverage
cmake --build --preset coverage --parallel

echo "== run suite, one profraw per test process =="
rm -rf "$PROFDIR"; mkdir -p "$PROFDIR"
# %p -> pid, %m -> module signature: unique file per process, no clobbering
# when tests fork/exec helpers.
export LLVM_PROFILE_FILE="$PWD/$PROFDIR/cox-%p-%m.profraw"
ctest --preset coverage

echo "== merge profraw -> profdata =="
PROFDATA="$BUILD_DIR/cox.profdata"
mapfile -t RAW < <(find "$PROFDIR" -name '*.profraw')
if [[ ${#RAW[@]} -eq 0 ]]; then
    echo "no .profraw files produced -- did the tests run?" >&2
    exit 1
fi
llvm-profdata merge -sparse "${RAW[@]}" -o "$PROFDATA"

# Report over Cox's own object files only (exclude third_party + tests).
# Use one instrumented binary as the coverage "image" plus the rest via
# -object so llvm-cov sees every TU's counters.
mapfile -t OBJS < <(find "$BUILD_DIR" -name '*.o' \
    -path '*CMakeFiles*' \
    | grep -vE '/third_party/|/tests/|/googletest/')
OBJ_ARGS=()
for o in "${OBJS[@]}"; do OBJ_ARGS+=(-object "$o"); done

IGNORE='(/third_party/|/tests/|/googletest/|/usr/)'

echo "== coverage summary (Cox sources) == "
llvm-cov report "${OBJ_ARGS[@]}" \
    -instr-profile="$PROFDATA" \
    -ignore-filename-regex="$IGNORE"

if [[ $WANT_HTML -eq 1 ]]; then
    echo "== html report =="
    llvm-cov show "${OBJ_ARGS[@]}" \
        -instr-profile="$PROFDATA" \
        -ignore-filename-regex="$IGNORE" \
        -format=html -output-dir="$BUILD_TDIR/html"
    echo "open $BUILD_DIR/html/index.html"
fi