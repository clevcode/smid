#!/usr/bin/env python3
"""Independently decode Clayton's synthetic probe images without extracting tar paths."""
import hashlib
import io
import json
from pathlib import Path
import sys
import tarfile

import numpy as np
from PIL import Image

archive = Path(sys.argv[1])
output = Path(sys.argv[2])
root = Path(__file__).resolve().parents[2]
colours = np.array([[255, 255, 255], [255, 255, 0], [0, 255, 255], [0, 255, 0],
                    [255, 0, 255], [255, 0, 0], [0, 0, 255], [0, 0, 0]], dtype=np.int16)
grey = np.array([0, 16, 32, 64, 128, 192, 235, 255], dtype=np.int16)
rows = []
with tarfile.open(archive) as tf:
    log_names = [n for n in tf.getnames() if n.endswith('/probe.log')]
    assert len(log_names) == 1
    log = tf.extractfile(log_names[0]).read().decode()
    for filename in ['encoder.c', 'tools/vaapi-probe.c']:
        assert hashlib.sha256((root / filename).read_bytes()).hexdigest() in log, filename
    for name in sorted(tf.getnames()):
        if not name.endswith('.jpg'):
            continue
        mode, frame, band = Path(name).stem.split('-')
        assert mode in ('copy', 'region')
        frame, band = int(frame[5:]), int(band[4:])
        assert 0 <= frame < 6 and band in (0, 1)
        raw = tf.extractfile(name).read()
        im = Image.open(io.BytesIO(raw))
        im.load()
        assert im.size == (1920, 544)
        y, x = np.indices((544, 1920))
        sy = y + 536 * band
        patch = (x // 240 + sy // 128 + frame * 3) % 8
        expected = colours[patch] if frame % 2 == 0 else np.repeat(grey[patch][..., None], 3, axis=2)
        mask = ((x >= 8) & (x < 1912) & (y >= 8) & (y < 536) &
                (x % 240 >= 8) & (x % 240 < 232) & (sy % 128 >= 8) & (sy % 128 < 120))
        error = np.abs(np.asarray(im, dtype=np.int16) - expected)[mask]
        mae, maximum = error.mean(axis=0).tolist(), int(error.max())
        assert max(mae) <= 4 and maximum <= 12, name
        rows.append({'file': Path(name).name, 'mae_rgb': mae, 'max_channel_error': maximum,
                     'sha256': hashlib.sha256(raw).hexdigest()})
assert len(rows) == len({r['file'] for r in rows}) == 24
result = {'archive_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
          'verified_images': len(rows), 'worst_channel_error': max(r['max_channel_error'] for r in rows),
          'images': rows}
output.write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps({k: v for k, v in result.items() if k != 'images'}, indent=2))
