#!/bin/sh
# Inside the development image: build Robotweax SRT itself with
# ThreadSanitizer, build the native targets against it, and run the
# end-to-end suite. With an uninstrumented library TSan cannot see the
# library's own synchronisation and reports are not trustworthy.
# Usage: tools/dev/run.sh tools/dev/tsan-e2e.sh
set -eu
out=artifacts/native
mkdir -p "$out"
flags="-fsanitize=thread -fno-omit-frame-pointer -g"
if [ ! -f /tmp/srt-tsan/lib/librobotweax-srt.so ]; then
    cmake -S /opt/src/robotweax-srt -B /tmp/srt-tsan-build -G Ninja \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_SHARED_LIBS=ON \
        -DROBOTWEAX_SRT_BUILD_TESTS=OFF -DROBOTWEAX_SRT_BUILD_BENCHMARKS=OFF \
        -DROBOTWEAX_SRT_BUILD_TOOLS=OFF -DCMAKE_INSTALL_PREFIX=/tmp/srt-tsan \
        -DCMAKE_CXX_FLAGS="$flags" -DCMAKE_C_FLAGS="$flags" \
        -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=thread" > "$out/srt-tsan-build.log" 2>&1
    cmake --build /tmp/srt-tsan-build >> "$out/srt-tsan-build.log" 2>&1
    cmake --install /tmp/srt-tsan-build >> "$out/srt-tsan-build.log" 2>&1
fi
export CMAKE_PREFIX_PATH=/tmp/srt-tsan LD_LIBRARY_PATH=/tmp/srt-tsan/lib
cmake -S tests -B /tmp/rwsrt-tsan-full -G Ninja -DRWSRT_SANITIZE=thread > "$out/tsan-full-build.log" 2>&1
cmake --build /tmp/rwsrt-tsan-full >> "$out/tsan-full-build.log" 2>&1
ldd /tmp/rwsrt-tsan-full/rwsrt_ref_sender | grep -i srt
mkdir -p /tmp/tsan-reports
status=0
TSAN_OPTIONS="log_path=/tmp/tsan-reports/tsan halt_on_error=0 exitcode=66" \
    python3 -I tests/integration/run_e2e.py --sender /tmp/rwsrt-tsan-full/rwsrt_ref_sender \
    --receiver /tmp/rwsrt-tsan-full/rwsrt_ref_receiver --evidence "$out/e2e_tsan_full.jsonl" \
    > "$out/e2e_tsan_full.log" 2>&1 || status=$?
tail -3 "$out/e2e_tsan_full.log"
cat /tmp/tsan-reports/tsan.* > "$out/tsan-reports.txt" 2>/dev/null || : > "$out/tsan-reports.txt"
echo "tsan reports: $(grep -c 'WARNING: ThreadSanitizer' "$out/tsan-reports.txt" || true)"
grep -h "SUMMARY" "$out/tsan-reports.txt" | sed -E 's/\(BuildId[^)]*\)//g; s/\+0x[0-9a-f]+//g' | sort | uniq -c | sort -rn | head -20
exit $status
