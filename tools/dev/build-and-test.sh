#!/bin/sh
# Inside the development image: configure, build, test, install into a clean
# prefix. Logs go to artifacts/ (not tracked). Usage (from the host):
#   tools/dev/run.sh tools/dev/build-and-test.sh
set -eu
build=${RWSRT_BUILD_DIR:-/tmp/rwsrt-build}
prefix=${RWSRT_PREFIX:-/tmp/rwsrt-prefix}
mkdir -p artifacts
rm -rf "$prefix"
cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$prefix" > artifacts/configure.log 2>&1
cmake --build "$build" > artifacts/build.log 2>&1
status=0
(cd "$build" && ctest --timeout 120 --output-on-failure -V) > artifacts/ctest.log 2>&1 || status=$?
cmake --install "$build" > artifacts/install.log 2>&1
grep -E "M0-EVIDENCE|tests passed|tests failed|\*\*\*|^FAIL|^ERROR|Ran [0-9]+ tests" artifacts/ctest.log || true
exit $status
