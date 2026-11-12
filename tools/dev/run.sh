#!/bin/sh
# Run a command inside the development image with the repository at /work.
# Usage: tools/dev/run.sh <command...>
set -eu
here=$(cd "$(dirname "$0")/../.." && pwd)
tag=$(python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1]))["dev_image"]["tag"])' "$here/dependencies.lock.json")
exec docker run --rm -v "$here":/work -w /work ${RWSRT_DOCKER_ARGS:-} "$tag" "$@"
