#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Golden byte vectors for RWSR version 1.

Written from docs/protocol-v1.md with Python's struct module only. It shares
no code with the C++ codec, so agreement between the two is evidence that
both follow the document rather than each other.

  gen_golden_vectors.py --write   regenerate fixtures, manifest and doc example
  gen_golden_vectors.py --check   fail if the committed files differ

The fixtures are immutable once released: a change here is a protocol change.
"""

import argparse
import hashlib
import json
import pathlib
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
FIXTURES = ROOT / "tests" / "fixtures" / "rwsr_v1"
DOC = ROOT / "docs" / "protocol-v1.md"
BEGIN = "<!-- BEGIN GENERATED EXAMPLE -->"
END = "<!-- END GENERATED EXAMPLE -->"

KIND = {"DATA": 1, "END": 2}
FORMAT = {"f32": 1, "cf32": 2}
SAMPLE_BYTES = {"f32": 4, "cf32": 8}


def header(v):
    timed = v.get("capture_time_ns") is not None
    payload_bytes = v["sample_count"] * SAMPLE_BYTES[v["format"]]
    fields = [
        (0, "magic", b"RWSR"),
        (4, "version", struct.pack(">B", 1)),
        (5, "kind", struct.pack(">B", KIND[v["kind"]])),
        (6, "flags", struct.pack(">H", 1 if timed else 0)),
        (8, "header_bytes", struct.pack(">H", 96)),
        (10, "sample_format", struct.pack(">B", FORMAT[v["format"]])),
        (11, "clock_domain", struct.pack(">B", 1 if timed else 0)),
        (12, "payload_bytes", struct.pack(">I", payload_bytes)),
        (16, "run_id", bytes.fromhex(v["run_id"])),
        (32, "segment_id", struct.pack(">Q", v["segment_id"])),
        (40, "first_sample_index", struct.pack(">Q", v["first_sample_index"])),
        (48, "sample_count", struct.pack(">I", v["sample_count"])),
        (52, "sample_rate_num", struct.pack(">I", v["rate_num"])),
        (56, "sample_rate_den", struct.pack(">I", v["rate_den"])),
        (60, "reserved_a", struct.pack(">I", 0)),
        (64, "capture_time_ns", struct.pack(">q", v["capture_time_ns"] if timed else 0)),
        (72, "capture_uncertainty_ns", struct.pack(">Q", v["capture_uncertainty_ns"] if timed else 0)),
        (80, "record_sequence", struct.pack(">Q", v["record_sequence"])),
        (88, "reserved_b", struct.pack(">Q", 0)),
    ]
    offset = 0
    for off, _name, raw in fields:
        assert off == offset, (off, offset)
        offset += len(raw)
    assert offset == 96
    return fields


def payload(v):
    # Samples are given as IEEE-754 bit patterns so that NaN payloads and
    # signed zero are exact. cf32 lists I then Q for each sample.
    bits = v["sample_bits"]
    per_sample = 2 if v["format"] == "cf32" else 1
    assert len(bits) == v["sample_count"] * per_sample
    return b"".join(struct.pack("<I", int(b, 16)) for b in bits)


VECTORS = [
    {
        "name": "f32_data",
        "kind": "DATA", "format": "f32",
        "run_id": "000102030405060708090a0b0c0d0e0f",
        "segment_id": 0, "first_sample_index": 0, "sample_count": 4,
        "rate_num": 48000, "rate_den": 1, "record_sequence": 0,
        # +0.0, -0.0, 1.5, quiet NaN with payload 0x1234
        "sample_bits": ["00000000", "80000000", "3fc00000", "7fc01234"],
    },
    {
        "name": "cf32_data",
        "kind": "DATA", "format": "cf32",
        "run_id": "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf",
        "segment_id": 2, "first_sample_index": 1000, "sample_count": 3,
        "rate_num": 100000, "rate_den": 1, "record_sequence": 57,
        # (1.0, -1.0), (-0.0, signalling NaN payload 1), (+inf, smallest subnormal)
        "sample_bits": ["3f800000", "bf800000", "80000000", "7fa00001", "7f800000", "00000001"],
    },
    {
        "name": "timed_data",
        "kind": "DATA", "format": "cf32",
        "run_id": "ffeeddccbbaa99887766554433221100",
        "segment_id": 1, "first_sample_index": (1 << 40) + 5, "sample_count": 2,
        "rate_num": 1000000, "rate_den": 3, "record_sequence": (1 << 33) + 7,
        "capture_time_ns": 1794484426201407000, "capture_uncertainty_ns": 250,
        "sample_bits": ["3e800000", "be800000", "40490fdb", "c0490fdb"],
    },
    {
        "name": "end",
        "kind": "END", "format": "cf32",
        "run_id": "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf",
        "segment_id": 2, "first_sample_index": 1003, "sample_count": 0,
        "rate_num": 100000, "rate_den": 1, "record_sequence": 58,
        "sample_bits": [],
    },
    {
        "name": "empty_run_end",
        "kind": "END", "format": "f32",
        "run_id": "000102030405060708090a0b0c0d0e0f",
        "segment_id": 0, "first_sample_index": 0, "sample_count": 0,
        "rate_num": 48000, "rate_den": 1, "record_sequence": 0,
        "sample_bits": [],
    },
]


def build(v):
    return b"".join(raw for _o, _n, raw in header(v)) + payload(v)


def doc_example():
    v = next(x for x in VECTORS if x["name"] == "timed_data")
    lines = [
        "The record below is the fixture `tests/fixtures/rwsr_v1/timed_data.bin`,",
        "a timed cf32 DATA record with two samples. It is produced by",
        "`tools/gen_golden_vectors.py`, which does not use the C++ codec.",
        "",
        "| Offset | Bytes | Field | Hex | Value |",
        "|---:|---:|---|---|---|",
    ]
    values = {
        "magic": "`RWSR`", "version": "1", "kind": "1 (DATA)",
        "flags": "bit 0 set: capture timestamp valid", "header_bytes": "96",
        "sample_format": "2 (cf32)", "clock_domain": "1 (Unix nanoseconds)",
        "payload_bytes": "16", "run_id": "opaque",
        "segment_id": str(v["segment_id"]),
        "first_sample_index": "%d (2^40 + 5)" % v["first_sample_index"],
        "sample_count": "2",
        "sample_rate_num": "1000000", "sample_rate_den": "3 (333333.3 samples/s)",
        "reserved_a": "0",
        "capture_time_ns": "%d" % v["capture_time_ns"],
        "capture_uncertainty_ns": "250",
        "record_sequence": "%d (2^33 + 7)" % v["record_sequence"],
        "reserved_b": "0",
    }
    for off, name, raw in header(v):
        lines.append("| %d | %d | `%s` | `%s` | %s |" % (off, len(raw), name, raw.hex(" "), values[name]))
    p = payload(v)
    notes = ["sample 0 I = 0.25", "sample 0 Q = -0.25", "sample 1 I = 3.1415927", "sample 1 Q = -3.1415927"]
    for i in range(0, len(p), 4):
        lines.append("| %d | 4 | payload | `%s` | %s, little-endian |" % (96 + i, p[i:i + 4].hex(" "), notes[i // 4]))
    lines += ["", "Total message length: %d bytes. SHA-256: `%s`." % (len(build(v)), hashlib.sha256(build(v)).hexdigest())]
    return "\n".join(lines)


def expected_files():
    files = {}
    manifest = {"protocol": "RWSR", "version": 1, "vectors": []}
    for v in VECTORS:
        raw = build(v)
        files[FIXTURES / (v["name"] + ".bin")] = raw
        entry = dict(v)
        entry.update(file=v["name"] + ".bin", length=len(raw), sha256=hashlib.sha256(raw).hexdigest())
        manifest["vectors"].append(entry)
    files[FIXTURES / "manifest.json"] = (json.dumps(manifest, indent=2) + "\n").encode()
    return files


def doc_with_example(text):
    a, b = text.index(BEGIN) + len(BEGIN), text.index(END)
    return text[:a] + "\n" + doc_example() + "\n" + text[b:]


def main():
    ap = argparse.ArgumentParser()
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--write", action="store_true")
    g.add_argument("--check", action="store_true")
    args = ap.parse_args()
    files = expected_files()
    doc_text = DOC.read_text()
    new_doc = doc_with_example(doc_text)
    if args.write:
        FIXTURES.mkdir(parents=True, exist_ok=True)
        for path, raw in files.items():
            path.write_bytes(raw)
        DOC.write_text(new_doc)
        print("wrote %d fixture files and the example in %s" % (len(files), DOC.name))
        return 0
    bad = [str(p.relative_to(ROOT)) for p, raw in files.items() if not p.exists() or p.read_bytes() != raw]
    if new_doc != doc_text:
        bad.append(str(DOC.relative_to(ROOT)))
    for b in bad:
        print("differs from the generator: " + b)
    print("golden vectors: %s" % ("FAIL" if bad else "ok"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
