#!/bin/sh
# Writes the source package of the release named in CMakeLists.txt
# (RWSRT_RELEASE_VERSION) and its checksum file.
#
#   tools/release/make-source-package.sh [--worktree] [output-dir]
#
# The package is `git archive` of one commit: the same commit always gives
# the same bytes. By default that commit is HEAD and the working tree must be
# clean. --worktree packages the tracked and staged state of the working tree
# instead, for testing before a commit; such a package is not a release.
set -eu
here=$(cd "$(dirname "$0")/../.." && pwd)
worktree=0
if [ "${1:-}" = "--worktree" ]; then worktree=1; shift; fi
out=${1:-"$here/artifacts/release"}
version=$(sed -n 's/^set(RWSRT_RELEASE_VERSION "\(.*\)")$/\1/p' "$here/CMakeLists.txt")
[ -n "$version" ] || { echo "RWSRT_RELEASE_VERSION not found in CMakeLists.txt" >&2; exit 1; }
name="gr-robotweax-srt-$version"

ref=HEAD
if [ "$worktree" = 1 ]; then
    stash=$(git -C "$here" stash create)
    ref=${stash:-HEAD}
elif [ -n "$(git -C "$here" status --porcelain)" ]; then
    echo "working tree is not clean; commit first or use --worktree" >&2
    exit 1
fi
commit=$(git -C "$here" rev-parse "$ref")

# The install guide names the package file; it must name this version.
git -C "$here" show "$ref:docs/install.md" | grep -q "$name.tar.gz" || {
    echo "docs/install.md does not name $name.tar.gz" >&2; exit 1; }

mkdir -p "$out"
git -C "$here" archive --format=tar --prefix="$name/" "$ref" | gzip -n -9 > "$out/$name.tar.gz"
(cd "$out" && { sha256sum "$name.tar.gz" 2>/dev/null || shasum -a 256 "$name.tar.gz"; } > SHA256SUMS)
printf '%s\n' "$commit" > "$out/SOURCE_COMMIT"
[ "$worktree" = 1 ] && echo "worktree state, not a release" >> "$out/SOURCE_COMMIT"
echo "$out/$name.tar.gz"
cat "$out/SHA256SUMS"
