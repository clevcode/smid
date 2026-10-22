# Light-grey loss and adaptive quality audit, 2026-10-22

Source inspected: `311cc40`. Clayton reports light-grey borders disappearing
against white with smid, consistently across three monitors, while the vendor
driver preserves them. Fixed Q98 and the CPU encoder reportedly do not resolve
it. The previously prepared user-service reply/archive has not been sent.

## Strongest lead: encoded colour range differs

The production VA-API path explicitly uses full-range sRGB input and full-range
BT.601 NV12 output in both its copy and USER_PTR conversion paths (`encoder.c`,
around lines 599 and 876). The CPU path uses standard libjpeg-turbo BGRX-to-JPEG
encoding, also producing full-range values in the tests below.

Reanalysis of Clayton's original vendor-driver capture included all 820 JPEGs,
sampling every eighth pixel horizontally and vertically. Neutral means decoded
RGB channel spread <= 2; no screen images were saved or displayed. Among
1,795,758 sampled neutral pixels:

- None decode below 16.
- Only 175 decode above 235, all to 236 (consistent with JPEG overshoot).
- 195,840 decode to 16 and 67,984 to 235.
- The largest populations are 232 (949,066) and 231 (197,321).
- All 820 luma quantization tables are identical: DC=1, range 1..7.

This strongly resembles limited-range luma. The source RGB pixels are unknown,
so capture histograms alone cannot prove the intended conversion.

Read-only disassembly of `pony:/opt/siliconmotion/SMIUSBDisplayManager`
(SHA-256 `2d112bf7f8cbc2cdad55d5207ecf6ed6bbaaf7a64e9ff1cabde277ebe1528ba4`)
provides independent corroboration. The binary's
`jsimd_extbgrx_ycc_convert_avx2` at 0xc4920 uses:

- `PW_F0257_F0254` at 0x107fe0: coefficients 16842 and 16646.
- `PW_F0098_F0250` at 0x108000: coefficients 6422 and 16384.
- `PD_Y_16` at 0x108080: 0x00100000, adding 16 before the fixed-point shift.
- Chroma coefficients including `PW_F0000_F0439`, consistent with reduced range.

The luma coefficients sum to 56294/65536, approximately 219/255, with a +16
offset. This is a modified limited-range RGB-to-YCbCr converter, not the ordinary
full-range conversion smid uses. This is the vendor binary on pony, not a binary
collected from Clayton; his actual capture independently shows matching range
characteristics. Disassembly is saved locally under `generated/`.

If the adapter interprets incoming luma as limited range, full-range values from
smid would be expanded as `(Y - 16) * 255 / 219`, then clipped. Thus distinct
greys 240, 248 and 255 could all become white. That would explain the symptom
with both encoders and why increasing JPEG quality does not fix it. The exact
stage applying that expansion and a physical corrected-output result remain
unverified. Do not call this a validated fix yet.

Intel's [VA-API range documentation](https://intel.github.io/libva/group__api__vpp.html)
defines full range as 0..255, and reduced range as Y=16..235 and Cb/Cr=16..240.
A correction must handle luma and chroma correctly, including CPU-generated
companion rectangles; merely changing JPEG quality or scaling all three RGB
channels is not an equivalent colour conversion.

## Local production-encoder test

`levels.c` links the current production encoder and logger only. It encodes
synthetic BGRX flat greys and one-pixel grey borders on white through the actual
region API, both 1920x544 bands at y=0 and y=536, using CPU and iHD at Q70/Q98/Q100.
It rejects backend fallback. Pillow independently decodes the resulting JPEGs.
No USB/EVDI devices are opened. This local GPU is not Clayton's HD 520.

| Backend / quality | Maximum flat-patch error | Maximum one-pixel border error |
| --- | ---: | ---: |
| CPU / 70 | 1 | 3 |
| CPU / 98 | 0 | 0 |
| CPU / 100 | 0 | 0 |
| VA-API / 70 | 1 | 6 |
| VA-API / 98 | 0 | 1 |
| VA-API / 100 | 0 | 0 |

Errors are channel code values out of 255. Both bands agree. All tested flat
near-whites (235, 236, 238, 240, 242, 244, 246, 248, 250, 252, 253, 254, 255)
survive Q98 exactly. This verifies the encoded JPEG, not what the adapter emits.

The earlier standalone probe compared JPEGs against ordinary full-range source
colours. It therefore could pass even if the adapter expects limited-range JPEG
samples; it did not exercise the adapter's interpretation.

## What the adaptive controller currently does

- One global quality controller, initially Q98 with a Q70 floor by default.
- Approximately one-second statistics windows, using delivered frames, source
  damage, packet bytes, encoding/build time, transport time and ACK waits.
- Above 105% of the configured 36 MiB/s target: reduce quality by 2 immediately
  at that statistics update.
- Sustained encoder pressure: reduce by 2 after four pressure windows.
- Sustained service/delivery pressure: reduce by 2 after two pressure windows.
- Below 95% of the byte target with no delivery deficit: increase by 2 after
  two spare windows, capped at the requested ceiling.
- ACK pacing with wire headroom suppresses the encoder-pressure classification;
  it does not suppress the separate service-pressure branch.
- With no dirty frame and at least 500 ms since the last completed transfer,
  reset the next-encode quality to the ceiling. The idle wait can be up to one
  second, so this is not a precisely scheduled 500 ms refresh.
- `--jpeg-fixed-quality` bypasses quality updates entirely; Q98 remains Q98.

There is **no idle high-quality repaint**. `quality_controller_note_idle()`
changes the quality setting, but sets no dirty bits and schedules no frame.
The next no-damage branch waits for new damage. Retained-band bookkeeping tracks
source hashes but not the quality of the encoded contents, so unchanged regions
can also retain lower-quality content after the controller recovers.

Quality refinement would need explicit per-display/band quality state and a
bounded repaint after motion settles, using the established same-sequence
top/bottom protocol. It must not invent a slot-selection rule from the sequence
number. That is a separate improvement; it cannot explain a fresh fixed-Q98
session losing near-white detail.

## Reproduce offline

```sh
mkdir -p research/quality-20261022/generated
cc -O2 -Wall -Wextra -Werror -std=c11 -D_GNU_SOURCE -iquote . \
  $(pkg-config --cflags libva libva-drm libturbojpeg) \
  research/quality-20261022/levels.c encoder.c log.c \
  -o research/quality-20261022/generated/levels \
  $(pkg-config --libs libva libva-drm libturbojpeg) -lpthread
research/quality-20261022/generated/levels cpu research/quality-20261022/generated
LIBVA_DRIVER_NAME=iHD research/quality-20261022/generated/levels direct-vaapi \
  research/quality-20261022/generated
python3 research/quality-20261022/check_levels.py \
  research/quality-20261022/generated \
  research/quality-20261022/generated/encoder-levels.json
python3 research/quality-20261022/capture_levels.py \
  /home/je/Downloads/smi-0768.pcap.gz \
  research/quality-20261022/generated/vendor-levels.json
```

The Python checks require Pillow and NumPy. The original capture has SHA-256
`da72155f87094c4d6d723745a7379338af3c0557e199c8c344dd99d17b3cdc31`.
Production source, the driver archive and the user-service archive are unchanged.
