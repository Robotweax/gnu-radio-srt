#!/bin/sh
# Inside the development image, after build-and-test.sh: verify the module
# as installed into a clean prefix, from outside the source and build trees.
set -eu
prefix=${RWSRT_PREFIX:-/tmp/rwsrt-prefix}
src=$(pwd)
pydir=$(find "$prefix" -type d -path '*gnuradio/robotweax_srt' | grep -v include | head -1)
pyroot=$(dirname "$(dirname "$pydir")")
libdir=$(dirname "$(find "$prefix" -name 'libgnuradio-robotweax_srt.so' | head -1)")
export PYTHONPATH="$pyroot" LD_LIBRARY_PATH="$libdir:${LD_LIBRARY_PATH:-}"
export GRC_BLOCKS_PATH="$prefix/share/gnuradio/grc/blocks"
work=$(mktemp -d)
cd "$work"

echo "== installed files"
(cd "$prefix" && find . -type f | sort)

echo "== python import outside source/build"
python3 - "$prefix" <<'PY2'
import sys
from gnuradio import robotweax_srt
path = robotweax_srt.__file__
print("module:", path)
assert path.startswith(sys.argv[1]), "imported from outside the prefix"
print("blocks:", sorted(n for n in dir(robotweax_srt) if n.startswith("m0_")))
PY2

echo "== SRT implementations linked"
ldd "$libdir/libgnuradio-robotweax_srt.so" | grep -i srt
test "$(ldd "$libdir/libgnuradio-robotweax_srt.so" | grep -ci 'srt\.so')" -eq 1

echo "== no SRT header reachable from installed public headers"
! grep -rn 'srt/srt.h\|robotweax_srt.h>' "$prefix/include" || exit 1

echo "== Companion block discovery and generated flowgraph"
python3 - <<'PY'
from gnuradio import gr
from gnuradio.grc.core.platform import Platform
p = Platform(version=gr.version(), version_parts=(gr.major_version(), gr.api_version(), gr.minor_version()),
             prefs=gr.prefs())
p.build_library()
found = sorted(k for k in p.blocks if k.startswith("robotweax_srt_"))
print("companion blocks:", found)
assert found == ["robotweax_srt_m0_probe_sink", "robotweax_srt_m0_probe_source"], found
PY
grcc -o "$work" "$src/examples/m0_companion_check.grc"
timeout 20 python3 "$work/m0_companion_check.py"
echo "generated flowgraph exit=$?"

echo "== C++ consumer of the installed package"
cmake -S "$src/tests/install_consumer" -B "$work/consumer" -G Ninja \
    -DCMAKE_PREFIX_PATH="$prefix;${CMAKE_PREFIX_PATH:-}" > "$work/consumer.log" 2>&1 \
    || { tail -30 "$work/consumer.log"; exit 1; }
cmake --build "$work/consumer" >> "$work/consumer.log" 2>&1 || { tail -30 "$work/consumer.log"; exit 1; }
timeout 20 "$work/consumer/rwsrt_install_consumer"
echo "== install check passed"
