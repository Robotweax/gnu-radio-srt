#!/bin/sh
# Inside the development image: run every qualification check of the
# milestones M0 to M3 and write an evidence directory. Separate, because
# they need more time, privileges or Docker itself: the 30-minute soak
# (tools/dev/soak.sh), the network runs (tools/dev/netem.sh,
# tools/dev/blocks-netem.sh) and the release check (tools/release/check.sh).
# Usage (from the host):
#   tools/dev/run.sh tools/dev/gate.sh [output-dir]
# Exits nonzero if any gate fails. A missing prerequisite is a failure, never
# a simulated pass.
set -u
out=${1:-artifacts/gate}
rm -rf "$out"
mkdir -p "$out"
gate() { # name command...
    name=$1; shift
    if "$@" > "$out/$name.log" 2>&1; then r=pass; else r=fail; fi
    echo "$name $r" | tee -a "$out/gates.txt"
}

build_spike() {
    cmake -S spikes/m0_srt_api -B /tmp/rwsrt-spike -G Ninja -DCMAKE_BUILD_TYPE=Release &&
        cmake --build /tmp/rwsrt-spike
}
run_spike() { /tmp/rwsrt-spike/rwsrt_m0_srt_api; }

gate srt_package_discovery_and_header_compile build_spike
gate srt_public_api run_spike
gate module_build_and_all_tests tools/dev/build-and-test.sh
gate block_qa_repeat_lifecycle tools/dev/repeat-test.sh qa_lifecycle 3
gate block_qa_repeat_transfer_tags tools/dev/repeat-test.sh "qa_transfer|qa_tags" 3
gate block_qa_repeat_robustness tools/dev/repeat-test.sh qa_robustness 2
# M3: the same suites with the block code under ASan and UBSan.
gate block_qa_asan_ubsan tools/dev/block-asan.sh
gate clean_prefix_install_python_companion_cxx tools/dev/check-install.sh
# M1: native core and transport, without GNU Radio.
gate native_unit_e2e_sanitizers_fuzz tools/dev/native-checks.sh "${RWSRT_FUZZ_SECONDS:-60}"
gate e2e_with_tsan_instrumented_srt tools/dev/tsan-e2e.sh

grep '^{' "$out/srt_public_api.log" > "$out/srt_api.jsonl" 2>/dev/null
cp artifacts/native/e2e.jsonl "$out/e2e.jsonl" 2>/dev/null
grep -hE "^(fuzz_|[a-z_0-9]+ (pass|FAIL))" "$out/native_unit_e2e_sanitizers_fuzz.log" \
    > "$out/native_checks.txt" 2>/dev/null
grep -h 'M2-EVIDENCE' artifacts/ctest.log 2>/dev/null | grep -E '^[0-9]+: ' |
    sed -E 's/^[0-9]+: [.F]*M2-EVIDENCE //' > "$out/blocks.jsonl"

python3 -I - "$out" <<'PY'
import json, platform, subprocess, sys, pathlib
out = pathlib.Path(sys.argv[1])
def sh(cmd):
    try:
        return subprocess.run(cmd, shell=True, capture_output=True, text=True).stdout.strip()
    except Exception as e:
        return "unavailable: %s" % e
def dpkg(p):
    return sh("dpkg-query -W -f '${Version}' %s" % p)
lock = json.load(open("dependencies.lock.json"))
manifest = {
    "milestones": ["M0", "M1", "M2", "M3"],
    "module_release": sh("sed -n 's/^set(RWSRT_RELEASE_VERSION \"\\(.*\\)\")$/\\1/p' CMakeLists.txt"),
    "machine": {"arch": platform.machine(), "kernel": platform.release(),
                "cpu_count": sh("nproc"), "cpu_model": sh("lscpu | sed -n 's/^Model name: *//p' | head -1"),
                "virtualization": "Docker container; see docs/decisions.md D2"},
    "os": sh(". /etc/os-release; echo $PRETTY_NAME"),
    "base_image": lock["base_image"]["reference"],
    "gnuradio": {"version": sh("gnuradio-config-info --version"), "package": dpkg("gnuradio"),
                 "dev_package": dpkg("gnuradio-dev")},
    "toolchain": {"gxx": sh("g++ -dumpfullversion"), "cmake": sh("cmake --version | head -1"),
                  "ninja": sh("ninja --version"), "build_type": "Release"},
    "python": {"version": platform.python_version(), "pybind11": dpkg("pybind11-dev"),
               "numpy": dpkg("python3-numpy")},
    "robotweax_srt": {"commit": sh("cat /opt/robotweax-srt/SOURCE_COMMIT"),
                      "pinned_commit": lock["robotweax_srt"]["commit"],
                      "package_version": sh("PKG_CONFIG_PATH=/opt/robotweax-srt/lib/pkgconfig pkg-config --modversion robotweax-srt"),
                      "library": sh("basename $(readlink -f /opt/robotweax-srt/lib/librobotweax-srt.so)"),
                      "crypto_backend": "openssl " + dpkg("libssl3t64"),
                      "build": lock["robotweax_srt"].get("build", {})},
    "secrets": "none recorded; the spike passphrase is a fixed test literal",
}
(out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
gates = {}
for line in (out / "gates.txt").read_text().splitlines():
    name, result = line.split()
    gates[name] = result
pin_ok = manifest["robotweax_srt"]["commit"] == manifest["robotweax_srt"]["pinned_commit"]
gates["srt_commit_matches_lock"] = "pass" if pin_ok else "fail"
verdict = {"gates": gates, "overall": "pass" if all(v == "pass" for v in gates.values()) else "fail"}
(out / "verdict.json").write_text(json.dumps(verdict, indent=2) + "\n")
print(json.dumps(verdict))
sys.exit(0 if verdict["overall"] == "pass" else 1)
PY
