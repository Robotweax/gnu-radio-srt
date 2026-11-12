#!/bin/sh
# Inside the development image: build the module and run selected ctest
# targets verbosely. Usage: tools/dev/run.sh tools/dev/qa.sh [ctest -R regex]
set -eu
build=${RWSRT_BUILD_DIR:-/tmp/rwsrt-build}
mkdir -p artifacts
cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${RWSRT_PREFIX:-/tmp/rwsrt-prefix}" > artifacts/configure.log 2>&1 ||
    { tail -30 artifacts/configure.log; exit 1; }
cmake --build "$build" > artifacts/build.log 2>&1 || { grep -E "error" -A8 artifacts/build.log | head -60; exit 1; }
cd "$build" && ctest --timeout 180 -V -R "${1:-qa_}" 2>&1 | grep -vE "^[0-9]+: (\[srt\]|$)" > /work/artifacts/qa.log || true
grep -E "^[0-9]+: (FAIL|ERROR|Traceback|  File|AssertionError|[A-Za-z]+Error)|tests passed|tests failed|Test +#|Ran [0-9]+ tests" /work/artifacts/qa.log | cut -c1-400 | head -80
