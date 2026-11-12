#!/bin/sh
# Release check, run from the host (it starts containers itself):
#   1. write the source package of the current commit;
#   2. follow docs/install.md from that package in a fresh container.
#
#   tools/release/check.sh [--worktree] [output-dir]
#
# Leaves in the output directory: the package, SHA256SUMS, SOURCE_COMMIT,
# clean-install.log, linked-libraries.tsv, versions.json and
# release-check.json. Exits nonzero if a step fails. This does not replace
# the gate (tools/dev/gate.sh); a release needs both.
set -eu
here=$(cd "$(dirname "$0")/../.." && pwd)
mode=
if [ "${1:-}" = "--worktree" ]; then mode=--worktree; shift; fi
out=${1:-"$here/artifacts/release"}
rm -rf "$out"
mkdir -p "$out"
package=$("$here/tools/release/make-source-package.sh" $mode "$out" | head -1)
result=pass
"$here/tools/release/check-clean-install.sh" "$package" "$out" || result=fail
python3 -I - "$out" "$result" "$(basename "$package")" <<'PY'
import json, pathlib, sys
out = pathlib.Path(sys.argv[1])
def read(name):
    p = out / name
    return p.read_text().strip() if p.exists() else None
log = read("clean-install.log") or ""
doc = {
    "package": sys.argv[3],
    "sha256": (read("SHA256SUMS") or "").split(" ")[0],
    "source_commit": (read("SOURCE_COMMIT") or "").splitlines(),
    "platform": read("clean-install-platform.txt"),
    "install_guide_blocks_executed": log.count("== install guide block"),
    "module_tests": [l.strip() for l in log.splitlines() if "tests passed" in l or "tests failed" in l],
    "reported_versions": json.loads(read("versions.json") or "null"),
    "clean_install": sys.argv[2],
}
(out / "release-check.json").write_text(json.dumps(doc, indent=2) + "\n")
print(json.dumps(doc, indent=2))
PY
[ "$result" = pass ]
