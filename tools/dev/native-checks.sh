#!/bin/sh
# Inside the development image: native unit tests under sanitizers, and the
# fuzz targets for a bounded time. Usage:
#   tools/dev/run.sh tools/dev/native-checks.sh [fuzz-seconds-per-target]
# Logs go to artifacts/native/. Exits nonzero on any failure.
set -u
fuzz_seconds=${1:-60}
out=artifacts/native
mkdir -p "$out"
status=0
step() { # name command...
    name=$1; shift
    if "$@" > "$out/$name.log" 2>&1; then echo "$name pass"; else echo "$name FAIL"; status=1; fi
}
build() { # dir, extra cmake args...
    dir=$1; shift
    cmake -S tests -B "$dir" -G Ninja "$@" && cmake --build "$dir"
}

step build_plain build /tmp/rwsrt-native
step unit_plain /tmp/rwsrt-native/rwsrt_unit_tests
step golden_vectors python3 -I tools/gen_golden_vectors.py --check
e2e() { # build dir, evidence name
    python3 -I tests/integration/run_e2e.py --sender "$1/rwsrt_ref_sender" \
        --receiver "$1/rwsrt_ref_receiver" --evidence "$out/$2.jsonl"
}
step e2e_plain e2e /tmp/rwsrt-native e2e

step build_asan_ubsan build /tmp/rwsrt-asan -DRWSRT_SANITIZE=address,undefined
step unit_asan_ubsan env ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 \
    /tmp/rwsrt-asan/rwsrt_unit_tests

step e2e_asan_ubsan env ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 \
    sh -c "python3 -I tests/integration/run_e2e.py --sender /tmp/rwsrt-asan/rwsrt_ref_sender \
        --receiver /tmp/rwsrt-asan/rwsrt_ref_receiver --evidence $out/e2e_asan.jsonl"

step build_tsan build /tmp/rwsrt-tsan -DRWSRT_SANITIZE=thread
step unit_tsan /tmp/rwsrt-tsan/rwsrt_unit_tests
# The end-to-end suite under TSan needs a TSan-instrumented SRT library:
# see tools/dev/tsan-e2e.sh.

step build_fuzz build /tmp/rwsrt-fuzz -DCMAKE_CXX_COMPILER=clang++ \
    -DRWSRT_SANITIZE=address,undefined -DRWSRT_BUILD_FUZZERS=ON
mkdir -p /tmp/corpus-decode /tmp/corpus-stream
# Seed the decoder corpus with the golden vectors (bound byte 0 = 1200).
for f in tests/fixtures/rwsr_v1/*.bin; do
    { printf '\000'; cat "$f"; } > "/tmp/corpus-decode/$(basename "$f")"
done
step fuzz_decode /tmp/rwsrt-fuzz/rwsrt_fuzz_decode -max_total_time="$fuzz_seconds" \
    -max_len=1400 -print_final_stats=1 /tmp/corpus-decode
step fuzz_record_stream /tmp/rwsrt-fuzz/rwsrt_fuzz_record_stream \
    -max_total_time="$fuzz_seconds" -max_len=512 -print_final_stats=1 /tmp/corpus-stream
for t in fuzz_decode fuzz_record_stream; do
    grep -hE "stat::number_of_executed_units|stat::new_units_added" "$out/$t.log" |
        sed "s/^/$t /"
    tail -200 "$out/$t.log" | grep -oE "cov: [0-9]+" | tail -1 | sed "s/^/$t /"
done
exit $status
