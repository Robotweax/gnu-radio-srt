# Compatibility statement for 0.1.0-rc2

## What this release was built and tested with

| Component | Version | Evidence |
|---|---|---|
| Operating system | Ubuntu 24.04 LTS | Every check runs in a container of the base image pinned by digest in `dependencies.lock.json` |
| Architecture | aarch64 and x86-64 | aarch64: local gate, soak and network measurements. x86-64: the gate and a short network-harness check on a hosted CI runner |
| GNU Radio | 3.10.9.2 (Ubuntu package `3.10.9.2-1.1ubuntu2`) | Installed versions are recorded in each gate run's `manifest.json` |
| Robotweax SRT | 0.3.2, commit `e45d0edd346e5c427d85e4c3b1ac605c81a23397`, shared library, OpenSSL backend | The gate fails if the installed commit differs from the lockfile |
| Compiler and tools | GCC 13.3, CMake 3.28, Ninja 1.11, Python 3.12, pybind11 2.11, NumPy 1.26 | `dependencies.lock.json` |

Nothing else is known to work. In particular these were never tried: other
Linux distributions, macOS, Windows, GNU Radio 3.9 or earlier, GNU Radio 4,
other Robotweax SRT releases, and SRT implementations other than Robotweax
SRT as the library the module links against.

The build accepts any Robotweax SRT 0.3.x through
`find_package(RobotweaxSRT 0.3)`. Only 0.3.2 is qualified.

x86-64 has less evidence than aarch64: the 30-minute soak and the full
network matrix were run on aarch64 only.

## Peers

A Sink and a Source of this release interoperate with each other and with
the reference sender and receiver in `tools/reference_peer/`. Both ends must
use the same sample format, sample rate, `stream_id` and passphrase.

0.1.0-rc2 interoperates with 0.1.0-rc1 in both directions:
the records are unchanged. Two additions are simply absent when the other
end is 0.1.0-rc1: it sends END once instead of three times, and it neither
sends nor reads the receiver notice.

The transport is standard SRT live mode with AES-CTR. Another SRT
implementation can therefore carry or terminate the connection, but the
payload is RWSR records, not MPEG-TS: a generic SRT tool can relay the
stream and cannot interpret it. Interoperation with other SRT
implementations was not tested.

## What may change

| Interface | Promise in the 0.x series |
|---|---|
| RWSR wire protocol version 1 | Frozen. The header layout and the golden vectors do not change. An incompatible change gets a new protocol version number, and a receiver rejects versions it does not know |
| Tag names and tag fields (`rwsrt.*`) | Fields may be added. Existing fields keep their meaning within 0.1.x |
| Block parameters, Python and C++ API | May change in any 0.x minor release. Changes are listed in the changelog |
| Status document | Fields may be added in any release. Existing fields keep their meaning within 0.1.x |
| ABI of `libgnuradio-robotweax_srt` | Not stable. Rebuild dependents with every release |
| Reference peers and tools | No promise |

## Licences

The module is GPL-3.0-or-later. What it links against is listed in
[../THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).
