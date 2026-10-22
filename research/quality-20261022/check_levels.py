#!/usr/bin/env python3
import json
from pathlib import Path
import sys
import numpy as np
from PIL import Image
levels = [0,1,2,4,8,15,16,17,32,64,128,192,224,230,234,235,236,238,240,242,244,246,248,250,252,253,254,255]
rows = []
for p in sorted(Path(sys.argv[1]).glob("*-q*-band*.jpg")):
    a = np.asarray(Image.open(p).convert("RGB")).astype(np.int16)
    assert a.shape == (544,1920,3)
    patches, borders = [], []
    for i, level in enumerate(levels):
        patch = a[16:112, i*64+8:i*64+56]
        patches.append({"input":level,"mean":float(patch.mean()),"min":int(patch.min()),"max":int(patch.max())})
        border = a[160:240, i*64+31]
        borders.append({"input":level,"mean":float(border.mean()),"min":int(border.min()),"max":int(border.max())})
    worst = max(max(abs(x["min"]-x["input"]),abs(x["max"]-x["input"])) for x in patches)
    rows.append({"file":p.name,"worst_flat_error":worst,"patches":patches,"borders":borders})
    print(p.name,"flat worst error:",worst,"near-white flat:",[(x["input"],x["mean"]) for x in patches if x["input"]>=235],"borders:",[(x["input"],x["mean"]) for x in borders if x["input"]>=235])
Path(sys.argv[2]).write_text(json.dumps(rows, indent=2)+"\n")
