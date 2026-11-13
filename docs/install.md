# Installing from the source package

This is the only supported way to install release 0.1.0-rc1. There are no
binary packages. The steps are for Ubuntu 24.04 LTS, the one qualified
platform (x86-64 or aarch64); see [compatibility.md](compatibility.md).

Every `sh` block on this page is executed, in order and unchanged, in a
fresh Ubuntu 24.04 container by `tools/release/check-clean-install.sh`. If
that check passes for a release, these instructions worked for it.

Robotweax SRT and the module are installed below one directory in your
home, `~/rwsrt`; they write nothing to `/usr`. To remove them, delete that
directory and the two source directories. Step 1 installs Ubuntu packages
system-wide, GNU Radio among them; those stay until you remove them with
`apt`.

You need about 2 GB of disk for the GNU Radio packages, network access to
the Ubuntu archive and to GitHub, and the file
`gr-robotweax-srt-0.1.0-rc1.tar.gz` in the directory where you run the
commands.

The package is attached to the release on GitHub:

```text
https://github.com/Robotweax/gnu-radio-srt/releases/tag/v0.1.0-rc1
```

Download `gr-robotweax-srt-0.1.0-rc1.tar.gz` and `SHA256SUMS` from there and
check the file with `sha256sum -c --ignore-missing SHA256SUMS`. Do not use
GitHub's automatic "Source code" archives: their directory name differs
from the one used below.

## 1. System packages

```sh
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
    build-essential ca-certificates cmake ninja-build pkg-config git \
    gnuradio gnuradio-dev \
    python3 python3-dev python3-numpy python3-pybind11 pybind11-dev python3-packaging \
    libssl-dev libspdlog-dev libfmt-dev libgmp-dev libboost-all-dev
```

This installs GNU Radio 3.10.9.2 from the Ubuntu archive.

Messages you can ignore in steps 1 to 3: in a container, `Failed to resolve
user 'systemd-network'` and similar from apt; Python `SyntaxWarning: invalid
escape sequence` from GNU Radio's own files; git's "detached HEAD" advice;
CMake's `Policy CMP0148` warnings, `Could NOT find Doxygen` and `No C++ unit
tests... skipping`; `lto-wrapper: warning: using serial compilation`.

## 2. Robotweax SRT 0.3.2

The module is qualified against exactly this release. The second command
stops if the tag does not point at the pinned commit.

```sh
git clone --branch v0.3.2 --depth 1 https://github.com/Robotweax/srt.git robotweax-srt
test "$(git -C robotweax-srt rev-parse HEAD)" = e45d0edd346e5c427d85e4c3b1ac605c81a23397
cmake -S robotweax-srt -B robotweax-srt/build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON \
    -DROBOTWEAX_SRT_BUILD_TESTS=OFF -DROBOTWEAX_SRT_BUILD_BENCHMARKS=OFF \
    -DROBOTWEAX_SRT_BUILD_TOOLS=OFF \
    -DCMAKE_INSTALL_PREFIX="$HOME/rwsrt"
cmake --build robotweax-srt/build
cmake --install robotweax-srt/build
```

Build it as a shared library, as shown. A process may contain only one SRT
implementation; do not load another SRT library into the same flowgraph.

## 3. The module

```sh
tar -xzf gr-robotweax-srt-0.1.0-rc1.tar.gz
cmake -S gr-robotweax-srt-0.1.0-rc1 -B gr-robotweax-srt-0.1.0-rc1/build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$HOME/rwsrt" \
    -DCMAKE_INSTALL_PREFIX="$HOME/rwsrt"
cmake --build gr-robotweax-srt-0.1.0-rc1/build
cmake --install gr-robotweax-srt-0.1.0-rc1/build
```

## 4. Environment

GNU Radio, Python and the dynamic loader do not look in `~/rwsrt` on their
own. Put these lines in a file and source it in every shell that runs a
flowgraph or GNU Radio Companion:

```sh
cat > "$HOME/rwsrt/env.sh" <<'EOF'
RWSRT_HOME="$HOME/rwsrt"
RWSRT_ARCH="$(dpkg-architecture -qDEB_HOST_MULTIARCH)"
export LD_LIBRARY_PATH="$RWSRT_HOME/lib:$RWSRT_HOME/lib/$RWSRT_ARCH${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PYTHONPATH="$RWSRT_HOME/lib/python3.12/site-packages${PYTHONPATH:+:$PYTHONPATH}"
export GRC_BLOCKS_PATH="$RWSRT_HOME/share/gnuradio/grc/blocks"
EOF
. "$HOME/rwsrt/env.sh"
```

A new shell does not have these settings until you run
`. ~/rwsrt/env.sh` in it. The same holds for `RWSRT_PASSPHRASE` in step 6.

## 5. Check the installation

From a directory outside the source tree:

```sh
cd "$HOME"
python3 -c 'from gnuradio import robotweax_srt as m; print(m.__file__); print(m.srt_sink, m.srt_source)'
```

The printed path must be below `~/rwsrt`.

## 6. First transfer

The examples encrypt with AES-CTR and read the passphrase from the
environment variable `RWSRT_PASSPHRASE` (10 to 79 bytes) when they start.
The passphrase below is a placeholder for this loopback test; choose your
own for anything else, and do not put it into a flowgraph file.

This starts a receiver, sends two seconds of a test signal at 100 kS/s to it
over loopback and prints what each end counted:

```sh
cd "$HOME/gr-robotweax-srt-0.1.0-rc1/examples"
export RWSRT_PASSPHRASE='replace-this-loopback-test-passphrase'
python3 headless_receiver.py --port 9000 > "$HOME/receiver.jsonl" &
sleep 1
python3 headless_sender.py --port 9000 --seconds 2 > "$HOME/sender.jsonl"
wait
tail -n 1 "$HOME/sender.jsonl" | python3 -c 'import json,sys; print("sent", json.load(sys.stdin)["input_samples_consumed"])'
tail -n 1 "$HOME/receiver.jsonl" | python3 -c 'import json,sys; d=json.load(sys.stdin); print("received", d["consumer"]["samples"], "end", d["session"]["end"])'
```

Expected output:

```text
sent 200000
received 200000 end complete
```

Both programs exit with status 0 only if the run was complete. Each line of
the two `.jsonl` files is one status snapshot; the fields are described in
[block-reference.md](block-reference.md).

## 7. GNU Radio Companion

With the environment from step 4, the two blocks appear in Companion under
"Robotweax SRT". This generates and runs the two example flowgraphs without
the graphical editor:

```sh
cd "$HOME"
grcc -o "$HOME" "$HOME/gr-robotweax-srt-0.1.0-rc1/examples/live_receiver.grc" \
    "$HOME/gr-robotweax-srt-0.1.0-rc1/examples/live_sender.grc"
python3 "$HOME/live_receiver.py" &
sleep 1
python3 "$HOME/live_sender.py"
wait
```

Both flowgraphs print nothing. The sender runs for about three seconds and
both end with exit status 0. `grcc` warns that the receiver "may not have
flow control"; that is expected, because the SRT Source is paced by the
sender.

To open them in the editor instead, run
`gnuradio-companion examples/live_receiver.grc`. The editor needs a display
and has not been run in the test environment.

## Optional: run the module's tests

The flowgraph test suites run from the build directory and take a few
minutes. They use loopback UDP ports.

```sh
cd "$HOME/gr-robotweax-srt-0.1.0-rc1/build"
ctest --timeout 300 --output-on-failure
```

The complete qualification gate (sanitizers, fuzzing, fault injection) needs
the development image; see the README.

## Two machines

Run the receiver on one machine and the sender on the other, with the same
passphrase and the same `--rate` on both:

```text
receiver$ python3 headless_receiver.py --bind 0.0.0.0 --port 9000
sender$   python3 headless_sender.py --host RECEIVER_ADDRESS --port 9000 --seconds 10
```

The transport is UDP: open the port for UDP in any firewall between them.
The default bind address is loopback; `--bind 0.0.0.0` is a deliberate
choice. This setup has not been run between two machines by the project;
see [limitations.md](limitations.md).

## If something fails

| Symptom | Cause |
|---|---|
| `Could not find a package configuration file provided by "RobotweaxSRT"` | Step 2 was skipped or installed elsewhere; `CMAKE_PREFIX_PATH` must name its prefix |
| `ModuleNotFoundError: No module named 'gnuradio.robotweax_srt'` | The environment of step 4 is not loaded in this shell |
| `ImportError: librobotweax-srt.so...: cannot open shared object file` | Same; `LD_LIBRARY_PATH` is missing |
| `"failure": "configuration"` | Read `failure_detail` next to it. Usual causes: `RWSRT_PASSPHRASE` is unset or not 10 to 79 bytes long; the receiver was started with another sample rate or format than the sender |
| The sender ends with `"failure": "credentials"` | The two passphrases differ. The receiver shows nothing: it keeps listening and ends with `startup_timeout` |
| The sender ends with `connection_lost` right after connecting, or sends for its whole duration and ends with `"completed": false` | Look at the receiver's status: it refused the stream, usually for a rate or format mismatch |
| The sender ends with `startup_timeout` and the receiver stays in `listening` | Port, address or a firewall between the two |

The last line of each `.jsonl` file is the final status. `session.failure`
and `session.failure_detail` in it say why a run ended; check both ends,
because some causes are visible on one end only.
