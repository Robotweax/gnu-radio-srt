#!/bin/sh
# Follows docs/install.md in a fresh container of the pinned base image:
# no development image, no source checkout, only the source package.
#
#   tools/release/check-clean-install.sh PACKAGE.tar.gz [output-dir]
#
# Every ```sh block of the install guide inside the package is executed in
# order, unchanged, by an unprivileged user with sudo. The check then
# compares the output of the first transfer with the guide's "Expected
# output" block and records what the installed library links against.
# Exits nonzero if any step fails. Needs Docker and network access.
set -eu
here=$(cd "$(dirname "$0")/../.." && pwd)
package=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
out=${2:-"$here/artifacts/release"}
mkdir -p "$out"
out=$(cd "$out" && pwd)
base=$(python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1]))["base_image"]["reference"])' "$here/dependencies.lock.json")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cp "$package" "$work/"
name=$(basename "$package" .tar.gz)
tar -xzf "$package" -C "$work" "$name/docs/install.md"

# The guide's sh blocks become one script; the text block after "Expected
# output:" becomes the expectation.
python3 -I - "$work/$name/docs/install.md" "$work" <<'PY'
import re, sys
text = open(sys.argv[1]).read()
blocks = re.findall(r"^```sh\n(.*?)^```$", text, re.S | re.M)
assert len(blocks) >= 7, "install guide has %d sh blocks" % len(blocks)
with open(sys.argv[2] + "/steps.sh", "w") as f:
    f.write("set -eu\n")
    for i, b in enumerate(blocks, 1):
        f.write("echo '== install guide block %d'\n%s" % (i, b))
expected = re.search(r"^Expected output:\n\n```text\n(.*?)^```$", text, re.S | re.M)
assert expected, "no expected output in the install guide"
open(sys.argv[2] + "/expected.txt", "w").write(expected.group(1))
print("%d blocks" % len(blocks))
PY

# After the guide: record what the installed library links against (exactly
# one SRT implementation) and the versions the block reports.
cat > "$work/post.sh" <<'POST'
set -eu
. "$HOME/rwsrt/env.sh"
lib=$(find "$HOME/rwsrt" -name 'libgnuradio-robotweax_srt.so' | head -1)
ldd "$lib" | awk '$3 ~ /^\// {print $3}' | sort -u | while read -r f; do
    real=$(readlink -f "$f")
    pkg=$(dpkg -S "$real" 2>/dev/null | head -1 | cut -d: -f1) || true
    if [ -n "$pkg" ]; then
        printf '%s\t%s\t%s\n' "$(basename "$f")" "$pkg" "$(dpkg-query -W -f '${Version}' "$pkg")"
    else
        printf '%s\t%s\t%s\n' "$(basename "$f")" "(not from a package)" "$real"
    fi
done > "$HOME/linked-libraries.tsv"
n=$(cut -f1 "$HOME/linked-libraries.tsv" | grep -ci 'srt\.so' || true)
[ "$n" -eq 1 ] || { echo "expected exactly one SRT library, found $n" >&2; exit 1; }
python3 -c 'from gnuradio import robotweax_srt as m; import json; print(json.dumps(m.srt_sink(m.make_transport_config(role="listener", security="none"), m.make_stream_config()).status()["versions"]))' > "$HOME/versions.json"
POST

status=0
docker run --rm -e DEBIAN_FRONTEND=noninteractive -v "$work":/in:ro -v "$out":/out "$base" sh -c '
    set -eu
    apt-get update -qq > /dev/null
    apt-get install -y -qq --no-install-recommends sudo > /dev/null
    useradd -m dev
    echo "dev ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/dev
    echo "Defaults env_keep += DEBIAN_FRONTEND" >> /etc/sudoers.d/dev
    cp /in/*.tar.gz /in/steps.sh /in/post.sh /home/dev/
    chown dev /home/dev/*
    rc=0
    su dev -c "cd /home/dev && sh steps.sh" > /out/clean-install.log 2>&1 || rc=$?
    if [ "$rc" = 0 ]; then su dev -c "cd /home/dev && sh post.sh" >> /out/clean-install.log 2>&1 || rc=$?; fi
    cp /home/dev/linked-libraries.tsv /home/dev/versions.json /out/ 2>/dev/null || true
    { . /etc/os-release; echo "$PRETTY_NAME $(uname -m)"; dpkg-query -W -f "gnuradio \${Version}\n" gnuradio 2>/dev/null || true; } > /out/clean-install-platform.txt
    chmod -R a+rwX /out
    exit $rc
' || status=$?

if [ "$status" != 0 ]; then
    echo "clean install FAILED (exit $status); last lines of $out/clean-install.log:" >&2
    tail -40 "$out/clean-install.log" >&2
    exit "$status"
fi
# The first transfer must have printed exactly what the guide promises.
python3 -I - "$out/clean-install.log" "$work/expected.txt" <<'PY'
import sys
log = open(sys.argv[1]).read()
expected = open(sys.argv[2]).read()
assert expected in log, "the guide's expected output is not in the log:\n" + expected
print("first transfer printed the expected output")
PY
grep -c "== install guide block" "$out/clean-install.log" | sed 's/^/blocks executed: /'
cat "$out/clean-install-platform.txt" "$out/versions.json"
echo "clean install passed"
