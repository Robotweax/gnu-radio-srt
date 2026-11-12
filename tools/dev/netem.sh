#!/bin/sh
# Starts a privileged, disposable container and runs a qualification plan in
# it. Network namespaces and netem need CAP_NET_ADMIN and CAP_SYS_ADMIN;
# everything is created inside the container and vanishes with it.
#
#   tools/dev/netem.sh PLAN OUT_DIR [run_matrix.py options]
#   tools/dev/netem.sh primary artifacts/qual/primary --parallel 4
set -eu
here=$(cd "$(dirname "$0")/../.." && pwd)
plan=${1:?plan: primary, secondary or smoke}
out=${2:?output directory, relative to the repository}
shift 2
tag=$(python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1]))["dev_image"]["tag"])' "$here/dependencies.lock.json")
exec docker run --rm --privileged -v "$here":/work -w /work "$tag" sh -c '
    set -eu
    modprobe sch_netem 2>/dev/null || true
    cmake -S tests -B /tmp/rwsrt-qual -G Ninja -DCMAKE_BUILD_TYPE=Release > /tmp/qual-build.log 2>&1 &&
    cmake --build /tmp/rwsrt-qual --target rwsrt_ref_sender rwsrt_ref_receiver >> /tmp/qual-build.log 2>&1 ||
        { tail -30 /tmp/qual-build.log; exit 1; }
    plan=$1; out=$2; shift 2
    python3 -I tools/run_matrix.py --bin /tmp/rwsrt-qual --out "$out" --plan "$plan" "$@"
' sh "$plan" "$out" "$@"
