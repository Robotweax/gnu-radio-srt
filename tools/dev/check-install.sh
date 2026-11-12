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
names = sorted(n for n in dir(robotweax_srt) if not n.startswith("_"))
print("names:", names)
for needed in ("srt_sink", "srt_source", "transport_config", "stream_config",
               "make_transport_config", "make_stream_config"):
    assert needed in names, needed
assert not any(n.startswith("m0_") for n in names), "probe scaffolding is still installed"
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
assert found == ["robotweax_srt_srt_sink", "robotweax_srt_srt_source"], found
PY
grcc -o "$work" "$src/examples/live_receiver.grc" "$src/examples/live_sender.grc" > "$work/grcc.log" 2>&1 \
    || { cat "$work/grcc.log"; exit 1; }
grep -E "Generating|Error|error" "$work/grcc.log" || true

echo "== generated Companion flowgraphs, end to end"
# The examples read the passphrase from the environment at start.
export RWSRT_PASSPHRASE="install-check-passphrase"
timeout 30 python3 "$work/live_receiver.py" > "$work/rx.log" 2>&1 &
rx_pid=$!
sleep 1
timeout 30 python3 "$work/live_sender.py" > "$work/tx.log" 2>&1
echo "generated sender exit=$?"
wait "$rx_pid"
echo "generated receiver exit=$?"

echo "== headless examples, end to end"
timeout 30 python3 "$src/examples/headless_receiver.py" --port 9021 > "$work/hrx.jsonl" 2> "$work/hrx.err" &
rx_pid=$!
sleep 1
timeout 30 python3 "$src/examples/headless_sender.py" --port 9021 --seconds 2 > "$work/htx.jsonl" 2> "$work/htx.err"
echo "headless sender exit=$?"
wait "$rx_pid"
echo "headless receiver exit=$?"
python3 - "$work/hrx.jsonl" "$work/htx.jsonl" <<'PY'
import json, sys
rx = json.loads(open(sys.argv[1]).read().splitlines()[-1])
tx = json.loads(open(sys.argv[2]).read().splitlines()[-1])
print("headless: sent %d, received %d, end %s, stretches %d" % (
    tx["input_samples_consumed"], rx["consumer"]["samples"], rx["session"]["end"],
    rx["consumer"]["contiguous_stretches"]))
assert tx["input_samples_consumed"] == rx["consumer"]["samples"] == 200000
assert rx["session"]["end"] == "complete" and rx["consumer"]["contiguous_stretches"] == 1
assert "install-check-passphrase" not in open(sys.argv[1]).read() + open(sys.argv[2]).read()
PY
unset RWSRT_PASSPHRASE

echo "== C++ consumer of the installed package"
cmake -S "$src/tests/install_consumer" -B "$work/consumer" -G Ninja \
    -DCMAKE_PREFIX_PATH="$prefix;${CMAKE_PREFIX_PATH:-}" > "$work/consumer.log" 2>&1 \
    || { tail -30 "$work/consumer.log"; exit 1; }
cmake --build "$work/consumer" >> "$work/consumer.log" 2>&1 || { tail -30 "$work/consumer.log"; exit 1; }
timeout 30 "$work/consumer/rwsrt_install_consumer" | tail -1
echo "== install check passed"
