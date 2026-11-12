#!/bin/sh
# Inside the development image: build and install the module, then run the
# long-run check. Usage: tools/dev/run.sh tools/dev/soak.sh [seconds]
set -eu
build=/tmp/rwsrt-soak-build
prefix=/tmp/rwsrt-soak-prefix
mkdir -p artifacts/soak
cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
    > artifacts/soak/build.log 2>&1
cmake --build "$build" >> artifacts/soak/build.log 2>&1
cmake --install "$build" >> artifacts/soak/build.log 2>&1
pydir=$(find "$prefix" -type d -path '*gnuradio/robotweax_srt' | grep -v include | head -1)
export PYTHONPATH="$(dirname "$(dirname "$pydir")")"
export LD_LIBRARY_PATH="$(dirname "$(find "$prefix" -name 'libgnuradio-robotweax_srt.so' | head -1)"):${LD_LIBRARY_PATH:-}"
exec python3 tools/dev/soak.py "${1:-1800}"
