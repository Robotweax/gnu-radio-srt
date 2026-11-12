#!/bin/sh
# Build the development image from the pins in dependencies.lock.json.
# Usage: tools/dev/build-image.sh [path-to-robotweax-srt-git-checkout]
# The SRT source is exported with `git archive` at the pinned commit, so the
# state of that checkout's working tree and current branch does not matter.
set -eu
here=$(cd "$(dirname "$0")/../.." && pwd)
srt_repo=${1:-${RWSRT_SRT_REPO:-"$here/../srt"}}
lock="$here/dependencies.lock.json"
pin() { python3 -I -c 'import json,sys; d=json.load(open(sys.argv[1]));
for k in sys.argv[2].split("."): d=d[k]
print(d)' "$lock" "$1"; }
commit=$(pin robotweax_srt.commit)
base=$(pin base_image.reference)
tag=$(pin dev_image.tag)
git -C "$srt_repo" cat-file -e "$commit^{commit}" || {
    echo "pinned SRT commit $commit not found in $srt_repo" >&2; exit 1; }
ctx=$(mktemp -d)
trap 'rm -rf "$ctx"' EXIT
git -C "$srt_repo" archive "$commit" | tar -x -C "$ctx"
docker build \
    --build-arg BASE_IMAGE="$base" \
    --build-arg SRT_COMMIT="$commit" \
    --build-context srt="$ctx" \
    -t "$tag" -f "$here/docker/Dockerfile" "$here/docker"
echo "built $tag (SRT $commit)"
