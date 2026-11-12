#!/bin/sh
# Starts a privileged, disposable container, builds and installs the module
# in it and runs the two blocks, encrypted, across netem paths.
#
#   tools/dev/blocks-netem.sh OUT_DIR [blocks_netem.py options]
#   tools/dev/blocks-netem.sh artifacts/blocks-netem --seconds 60 --reps 3
set -eu
here=$(cd "$(dirname "$0")/../.." && pwd)
out=${1:?output directory, relative to the repository}
shift
tag=$(python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1]))["dev_image"]["tag"])' "$here/dependencies.lock.json")
exec docker run --rm --privileged -v "$here":/work -w /work "$tag" sh -c '
    set -eu
    modprobe sch_netem 2>/dev/null || true
    prefix=/tmp/rwsrt-bn-prefix
    cmake -S . -B /tmp/rwsrt-bn -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$prefix > /tmp/bn-build.log 2>&1 &&
    cmake --build /tmp/rwsrt-bn >> /tmp/bn-build.log 2>&1 &&
    cmake --install /tmp/rwsrt-bn >> /tmp/bn-build.log 2>&1 || { tail -30 /tmp/bn-build.log; exit 1; }
    export PYTHONPATH=$prefix/lib/python3.12/site-packages
    export LD_LIBRARY_PATH=$prefix/lib/$(dpkg-architecture -qDEB_HOST_MULTIARCH):$LD_LIBRARY_PATH
    out=$1; shift
    python3 tools/blocks_netem.py --out "$out" "$@"
' sh "$out" "$@"
