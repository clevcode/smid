#!/usr/bin/env python3
"""Read-only, dependency-free usbmon pcap inspection. Does not replay USB data.

Record numbers are one-based (as in Wireshark). Output contains protocol headers
and JPEG metadata, not captured screen images. Supports plain or gzipped pcap.
"""
import argparse
import collections
import gzip
import hashlib
import json
from pathlib import Path
import struct

MAGIC = b"smifalconsta"


def records(path):
    opener = gzip.open if str(path).endswith(".gz") else open
    with opener(path, "rb") as f:
        h = f.read(24)
        formats = {b"\xd4\xc3\xb2\xa1": "<", b"\xa1\xb2\xc3\xd4": ">",
                   b"\x4d\x3c\xb2\xa1": "<", b"\xa1\xb2\x3c\x4d": ">"}
        if len(h) != 24 or h[:4] not in formats:
            raise ValueError("unsupported or incomplete pcap header")
        endian = formats[h[:4]]
        _, _, _, _, snaplen, linktype = struct.unpack(endian + "HHiIII", h[4:])
        if linktype != 220:
            raise ValueError(f"expected DLT_USB_LINUX_MMAPPED (220), got {linktype}")
        n = 0
        while True:
            rh = f.read(16)
            if not rh:
                break
            if len(rh) != 16:
                raise ValueError("partial pcap record header")
            sec, frac, captured, original = struct.unpack(endian + "IIII", rh)
            b = f.read(captured)
            if len(b) != captured or captured < 64:
                raise ValueError("partial pcap record or short usbmon header")
            n += 1
            urb, event, xfer, ep, dev, bus, setup_flag, data_flag, ts, us, status, length, caplen = struct.unpack_from(
                endian + "QBBBBHBBqiiII", b)
            ndesc = struct.unpack_from(endian + "I", b, 60)[0]
            offset = 64 + (16 * ndesc if xfer == 0 else 0)
            yield {"record": n, "ts_us": ts * 1000000 + us, "urb": urb,
                   "event": chr(event), "xfer": xfer, "ep": ep, "dev": dev,
                   "bus": bus, "status": status, "length": length, "caplen": caplen,
                   "captured": captured, "original": original, "snaplen": snaplen,
                   "data_flag": data_flag, "setup": b[40:48] if setup_flag == 0 else None,
                   "payload": b[offset:offset + caplen]}


def jpeg_info(b):
    if not b.startswith(b"\xff\xd8"):
        return None
    i = 2
    info = {}
    while i + 4 <= len(b):
        if b[i] != 255:
            break
        while i < len(b) and b[i] == 255:
            i += 1
        if i >= len(b):
            break
        marker = b[i]
        i += 1
        if marker in (0xd8, 0xd9) or 0xd0 <= marker <= 0xd7:
            continue
        if i + 2 > len(b):
            break
        size = int.from_bytes(b[i:i+2], "big")
        if size < 2 or i + size > len(b):
            break
        segment = b[i+2:i+size]
        if marker in (0xc0, 0xc1, 0xc2) and len(segment) >= 6:
            info = {"sof": hex(marker), "precision": segment[0],
                    "height": int.from_bytes(segment[1:3], "big"),
                    "width": int.from_bytes(segment[3:5], "big"),
                    "components": [list(segment[k:k+3]) for k in range(6, len(segment), 3)]}
        if marker == 0xda:
            # JPEG entropy data escapes FF; the first unescaped FF D9 is EOI.
            eoi = b.find(b"\xff\xd9", i + size)
            info["eoi_length"] = eoi + 2 if eoi >= 0 else None
            break
        i += size
    return info


def analyze(path):
    counters = {name: collections.Counter() for name in (
        "devices", "transfers", "completion_status", "smi_kinds", "image_shapes",
        "jpeg_shapes", "reply_codes", "control_commands", "image_trailers")}
    controls, replies, frames, usb_setup, truncated = [], [], [], [], []
    n = total_bytes = smi_count = smi_truncated = 0
    first = last = None
    pending = {}
    submit_complete_mismatches = []
    for r in records(path):
        n += 1
        first = r["ts_us"] if first is None else first
        last = r["ts_us"]
        total_bytes += 16 + r["captured"]
        loc = f'{r["bus"]}:{r["dev"]}'
        counters["devices"][loc] += 1
        counters["transfers"][f'{loc}/{r["event"]}/type={r["xfer"]}/ep={r["ep"]:02x}'] += 1
        if r["event"] != "S":
            counters["completion_status"][str(r["status"])] += 1
            submit = pending.pop(r["urb"], None)
            if submit and r["status"] == 0 and not (r["ep"] & 128) and submit[1] != r["length"]:
                submit_complete_mismatches.append([submit[0], r["record"], submit[1], r["length"]])
        else:
            pending[r["urb"]] = (r["record"], r["length"])
        metadata = {k: r[k] for k in ("record", "event", "xfer", "bus", "dev", "ep", "status", "length", "caplen")}
        metadata["t"] = round((r["ts_us"] - first) / 1000000, 6)
        if r["setup"] is not None:
            usb_setup.append({**metadata, "setup_hex": r["setup"].hex(), "data_prefix": r["payload"][:32].hex()})
        b = r["payload"]
        is_truncated = r["captured"] < r["original"] or (r["data_flag"] == 0 and r["caplen"] < r["length"])
        if is_truncated:
            truncated.append(metadata)
        if not b.startswith(MAGIC):
            continue
        smi_count += 1
        smi_truncated += is_truncated
        metadata["header_hex"] = b[:48].hex()
        if len(b) < 24:
            continue
        word0, word1, word2 = struct.unpack_from("<III", b, 12)
        metadata.update(wire_length=word0, word16=f"{word1:08x}", word20=f"{word2:08x}")
        counters["smi_kinds"][f'{r["event"]}/ep={r["ep"]:02x}/word16={word1:08x}'] += 1
        if r["event"] == "C" and r["ep"] & 128:
            counters["reply_codes"][f'{word1:08x}'] += 1
            replies.append(metadata)
        elif r["event"] == "S" and not (r["ep"] & 128):
            if len(b) >= 48 and word2 & 65535 == 0x0104:
                seq = int.from_bytes(b[22:26], "little")
                x, y, w, h = (int.from_bytes(b[o:o+2], "little") for o in (27, 29, 31, 33))
                encoded = int.from_bytes(b[38:42], "little")
                j = jpeg_info(b[48:48+encoded])
                f = {**metadata, "seq": seq, "opcode": b[26], "rect": [x,y,w,h],
                     "stride": int.from_bytes(b[36:38], "little"), "encoded_length": encoded,
                     "band": b[42], "jpeg": j, "truncated": is_truncated}
                frames.append(f)
                counters["image_shapes"][f'{word1:08x}/op={b[26]:02x}/rect={x},{y},{w},{h}/stride={f["stride"]}/band={b[42]:02x}'] += 1
                counters["image_trailers"][str(word0 - 48 - encoded)] += 1
                if j:
                    counters["jpeg_shapes"][json.dumps({k:v for k,v in j.items() if k != "eoi_length"}, sort_keys=True)] += 1
            else:
                metadata["prefix_hex"] = b[:96].hex()
                controls.append(metadata)
                counters["control_commands"][f'word16={word1:08x}/cmd={b[20]:02x}'] += 1
    with open(path, "rb") as f:
        digest = hashlib.file_digest(f, "sha256").hexdigest()
    return {"source": str(path), "sha256": digest, "records": n,
            "duration_seconds": (last-first)/1000000 if first is not None else 0,
            "uncompressed_pcap_bytes": total_bytes + 24, "smi_records": smi_count,
            "truncated_records": len(truncated), "truncated_smi_records": smi_truncated,
            "truncation_examples": truncated[:15], "submit_complete_mismatches": submit_complete_mismatches,
            **{k:dict(sorted(v.items())) for k,v in counters.items()},
            "controls": controls, "replies": replies, "frames": frames, "usb_setup": usb_setup}


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("capture", type=Path)
    ap.add_argument("output", type=Path)
    args = ap.parse_args()
    result = analyze(args.capture)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({k:v for k,v in result.items() if k not in ("controls", "replies", "frames", "usb_setup")}, indent=2))
