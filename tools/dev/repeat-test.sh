#!/bin/sh
# Inside the development image, after build-and-test.sh: rerun one ctest
# target N times and count failures. Usage: repeat-test.sh <regex> <count>
set -u
build=${RWSRT_BUILD_DIR:-/tmp/rwsrt-build}
fails=0
i=0
while [ "$i" -lt "$2" ]; do
    i=$((i + 1))
    (cd "$build" && ctest --timeout 120 -R "$1" -V) > "artifacts/repeat-$i.log" 2>&1 || {
        fails=$((fails + 1)); echo "run $i FAILED"; }
done
echo "repeat: $2 runs of '$1', $fails failed"
[ "$fails" -eq 0 ]
