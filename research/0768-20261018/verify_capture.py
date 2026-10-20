#!/usr/bin/env python3
"""Compare captured packets with the checked-out C builders, without USB I/O."""
import argparse
import collections
import ctypes as c
import io
import json
from pathlib import Path
import subprocess

from analyze_capture import records, MAGIC, jpeg_info

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


class Rect(c.Structure):
    _fields_ = [(k, c.c_uint16) for k in ("x", "y", "w", "h", "stride_bytes")] + [("band_marker", c.c_uint8)]


class Rawcmd(c.Structure):
    _fields_ = [("name", c.c_char_p), ("endpoint", c.c_uint8), ("xfer", c.c_uint8),
                ("length", c.c_uint32), ("wait_code", c.c_uint8), ("data", c.c_uint8 * 96)]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("capture", type=Path)
    ap.add_argument("output", type=Path)
    args = ap.parse_args()
    dest = HERE / "generated"
    dest.mkdir(exist_ok=True)
    libpath = dest / "libsmid-protocol.so"
    cmd = ["cc", "-shared", "-fPIC", "-O2", "-Wall", "-Wextra", "-Werror", "-std=c11",
           "-D_GNU_SOURCE", "-D_POSIX_C_SOURCE=200809L", "-iquote", str(ROOT),
           *(str(ROOT / f) for f in ("cnm.c", "protocol.c", "frame_packet.c", "log.c", "time.c")),
           "-pthread", "-o", str(libpath)]
    subprocess.run(cmd, check=True)
    lib = c.CDLL(str(libpath))
    ptr = c.POINTER(c.c_uint8)
    lib.smid_cnm_build_header_base.argtypes = [c.c_void_p, ptr, c.c_uint32, ptr, c.c_bool, c.c_uint32]
    lib.smid_cnm_build_header_base.restype = c.c_int
    lib.smid_frame_header_build.argtypes = [ptr, c.c_int, c.c_uint32, Rect, c.c_uint32, c.c_uint32]
    lib.smid_frame_header_build.restype = None
    lib.smid_make_semantic_init.argtypes = [c.POINTER(Rawcmd), c.POINTER(c.c_size_t)]
    lib.smid_make_semantic_init.restype = None
    lib.smid_parse_frame_ack.argtypes = [ptr, c.c_size_t, c.POINTER(c.c_int), c.POINTER(c.c_uint32)]
    lib.smid_parse_frame_ack.restype = c.c_bool
    init = (Rawcmd * 96)()
    ninit = c.c_size_t()
    lib.smid_make_semantic_init(init, c.byref(ninit))
    init_rows = [{"name": r.name.decode(), "endpoint": r.endpoint, "wait_code": r.wait_code,
                  "hex": bytes(r.data[:r.length]).hex()} for r in init[:ninit.value]]

    # Optional independent JPEG decoding (no images are saved or displayed).
    try:
        from PIL import Image
    except ImportError:
        Image = None
    stats = collections.Counter()
    cnm_diffs = collections.Counter()
    header_diffs = collections.Counter()
    bases = collections.Counter()
    pairs = collections.defaultdict(list)
    pair_pending = {}
    pair_issues = []
    paired_updates = 0
    cnm_examples, header_examples, jpeg_failures, ack_examples, captured_controls = [], [], [], [], []
    for r in records(args.capture):
        b = r["payload"]
        if not b.startswith(MAGIC) or len(b) < 24:
            continue
        word = int.from_bytes(b[16:20], "little")
        if r["event"] == "C" and r["ep"] == 0x81 and b[16] == 0xe9:
            target = 1 if word & 0x10000 else 0
            stats[f"ack_target_{target}"] += 1
            stream, sequence = c.c_int(), c.c_uint32()
            accepted = lib.smid_parse_frame_ack((c.c_uint8 * len(b)).from_buffer_copy(b),
                                                len(b), c.byref(stream), c.byref(sequence))
            if accepted:
                assert stream.value == target
                assert sequence.value == int.from_bytes(b[20:24], 'little')
            stats[f"current_rx_predicate_accepts_target_{target}"] += accepted
            if not accepted and len(ack_examples) < 4:
                ack_examples.append({"record": r["record"], "word16": hex(word), "target": target})
        if r["event"] != "S" or r["ep"] & 0x80:
            continue
        if b[20:22] != b"\x04\x01" or len(b) < 48:
            if r["ep"] == 2 and b[20] != 0xec:
                captured_controls.append({"record": r["record"], "dev": r["dev"], "hex": b[:48].hex()})
            continue
        if b[26] != 0xe0:
            stats["non_jpeg_images"] += 1
            continue
        stats["jpeg_packets"] += 1
        wire = int.from_bytes(b[12:16], "little")
        encoded = int.from_bytes(b[38:42], "little")
        if len(b) != wire or len(b) < 48 + encoded + 0x5000:
            stats["incomplete_image_packets"] += 1
            continue
        slot = b[48:48+encoded]
        original = b[48+encoded:48+encoded+0x5000]
        base = int.from_bytes(original[13:17], "little")
        bases[hex(base)] += 1
        out = (c.c_uint8 * 0x5000)()
        rc = lib.smid_cnm_build_header_base(None, (c.c_uint8 * encoded).from_buffer_copy(slot), encoded, out, True, base)
        if rc:
            stats["cnm_builder_errors"] += 1
            continue
        differing = [i for i, (a, b_) in enumerate(zip(bytes(out), original)) if a != b_]
        stats["cnm_exact_matches"] += not differing
        stats["cnm_structured_prefix_matches"] += not any(i < 0x1651 for i in differing)
        cnm_diffs.update(map(hex, differing))
        if differing and len(cnm_examples) < 3:
            cnm_examples.append({"record": r["record"], "difference_count": len(differing),
                                 "examples": [[hex(i), out[i], original[i]] for i in differing[:30]]})
        seq = int.from_bytes(b[22:26], "little")
        stream = 1 if word == 0xb0000002 else 0
        rect = Rect(*(int.from_bytes(b[o:o+2], "little") for o in (27,29,31,33,36)), b[42])
        h = (c.c_uint8 * 48)()
        lib.smid_frame_header_build(h, stream, seq, rect, wire, encoded)
        hd = [i for i,(a,b_) in enumerate(zip(bytes(h), b[:48])) if a != b_]
        header_diffs.update(map(str, hd))
        stats["headers_match_defined_fields"] += not any(i < 43 and i != 35 for i in hd)
        if hd and len(header_examples) < 3:
            header_examples.append({"record": r["record"], "differences": [[i,h[i],b[i]] for i in hd]})
        pairs[(r["bus"],r["dev"],stream,seq)].append((r["record"],b[42]))
        key = (r["bus"],r["dev"],stream)
        prior = pair_pending.pop(key, None)
        if prior is None:
            pair_pending[key] = (r["record"],seq,b[42])
        elif prior[1] == seq and {prior[2], b[42]} == {0x21,0x22}:
            paired_updates += 1
        else:
            pair_issues.append({"prior": prior, "current": [r["record"],seq,b[42]]})
            pair_pending[key] = (r["record"],seq,b[42])
        ji = jpeg_info(slot)
        if ji and ji.get("eoi_length"):
            stats["jpeg_eoi_present"] += 1
            if Image is not None:
                try:
                    with Image.open(io.BytesIO(slot[:ji["eoi_length"]])) as im:
                        im.load()
                        assert im.size == (rect.w,rect.h)
                    stats["jpeg_decode_ok"] += 1
                except Exception as e:
                    jpeg_failures.append({"record": r["record"], "error": str(e)})
    reused_sequences = [{"key": key, "bands": bands} for key,bands in pairs.items() if len(bands)>2]
    result = {"source_revision": subprocess.check_output(["git","rev-parse","HEAD"],cwd=ROOT,text=True).strip(),
              "compile_command": cmd, "stats": dict(stats), "jpeg_decoder_available": Image is not None,
              "cnm_base_values": dict(bases), "cnm_diff_offsets": dict(cnm_diffs), "cnm_examples": cnm_examples,
              "header_diff_offsets": dict(header_diffs), "header_examples": header_examples,
              "paired_updates": paired_updates, "pair_issues": pair_issues,
              "unpaired_at_end": list(pair_pending.values()), "reused_sequences": reused_sequences,
              "jpeg_failures": jpeg_failures,
              "ack_rejected_examples": ack_examples, "source_init": init_rows, "captured_controls": captured_controls}
    args.output.write_text(json.dumps(result,indent=2)+"\n")
    print(json.dumps({k:v for k,v in result.items() if k not in ("source_init","captured_controls","compile_command","cnm_diff_offsets")},indent=2))


if __name__ == "__main__":
    main()
