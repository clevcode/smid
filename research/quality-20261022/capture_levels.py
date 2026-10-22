#!/usr/bin/env python3
"""Summarize neutral JPEG levels without saving or displaying captured pixels."""
import collections
import io
import json
from pathlib import Path
import sys
import numpy as np
from PIL import Image
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "0768-20261018"))
from analyze_capture import records, MAGIC
hist = np.zeros(256, dtype=np.int64)
quant = collections.Counter()
count = 0
for r in records(sys.argv[1]):
    b = r["payload"]
    if r["event"] != "S" or r["ep"] & 128 or not b.startswith(MAGIC) or b[48:50] != b"\xff\xd8":
        continue
    im = Image.open(io.BytesIO(b[48:]))
    quant[tuple(im.quantization.get(0, []))] += 1
    # Spatial sampling only; every captured JPEG is included.
    a = np.asarray(im.convert("RGB"))[::8, ::8].astype(np.int16)
    neutral = a.max(axis=2) - a.min(axis=2) <= 2
    values = np.rint(a.mean(axis=2)).astype(np.int16)[neutral]
    hist += np.bincount(values, minlength=256)
    count += 1
result = {"jpeg_count": count, "neutral_samples": int(hist.sum()),
          "neutral_histogram": hist.tolist(),
          "top_neutral_levels": sorted(enumerate(hist.tolist()), key=lambda x: -x[1])[:20],
          "luma_quant_tables": [{"count": n, "min": min(q), "max": max(q), "dc": q[0]} for q,n in quant.most_common()]}
Path(sys.argv[2]).write_text(json.dumps(result, indent=2) + "\n")
print(json.dumps({k:v for k,v in result.items() if k != "neutral_histogram"}, indent=2))
