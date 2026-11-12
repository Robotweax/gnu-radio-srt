#!/bin/sh
# Inside the development image: build the GNU Radio module with
# AddressSanitizer and UndefinedBehaviorSanitizer and run the flowgraph QA
# suites under them. GNU Radio and Python themselves are not instrumented,
# so the sanitizer runtime is preloaded and leak detection is off (the
# interpreter never frees everything). Usage:
#   tools/dev/run.sh tools/dev/block-asan.sh [ctest -R regex]
set -eu
build=/tmp/rwsrt-asan-blocks
out=artifacts/native
mkdir -p "$out"
flags="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined"
cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_CXX_FLAGS="$flags" -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address,undefined" \
    -DCMAKE_MODULE_LINKER_FLAGS="-fsanitize=address,undefined" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" > "$out/block-asan-build.log" 2>&1
cmake --build "$build" >> "$out/block-asan-build.log" 2>&1
export LD_PRELOAD="$(gcc -print-file-name=libasan.so) $(gcc -print-file-name=libubsan.so)"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0:exitcode=97:log_path=/tmp/block-asan"
export UBSAN_OPTIONS="print_stacktrace=1:log_path=/tmp/block-ubsan"
status=0
(cd "$build" && ctest --timeout 600 -V -R "${1:-qa_(config|transfer|tags|lifecycle|robustness)}") \
    > "$out/block-asan.log" 2>&1 || status=$?
grep -E "Test +#|tests passed|tests failed" "$out/block-asan.log" || true
reports=$(ls /tmp/block-asan.* /tmp/block-ubsan.* 2>/dev/null | wc -l)
cat /tmp/block-asan.* /tmp/block-ubsan.* > "$out/block-asan-reports.txt" 2>/dev/null || : > "$out/block-asan-reports.txt"
echo "sanitizer report files: $reports"
grep -hE "ERROR: AddressSanitizer|runtime error:" "$out/block-asan-reports.txt" | sort | uniq -c | head -20 || true
[ "$status" -eq 0 ] && [ "$reports" -eq 0 ]
