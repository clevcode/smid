# Light-grey loss and adaptive quality audit, 2026-10-22

Current status: a follow-up shared on 2026-11-14 confirms that the limited-range
build restores light-grey borders on the tested Acer and HQ monitors. The audit
and local validation below retain their original evidence boundaries; see the
physical follow-up at the end for the subsequent tester result.

Initial source inspected: `311cc40`. The hardware test report describes
light-grey borders disappearing against white with smid, consistently across
three monitors, while the vendor driver preserves them. Fixed Q98 and the CPU
encoder reportedly do not resolve
it. The previously prepared user-service reply/archive has not been sent.

## Strongest lead: encoded colour range differs

Before the correction below, the VA-API path explicitly used full-range sRGB
input and full-range BT.601 NV12 output in both its copy and USER_PTR conversion
paths (`encoder.c`, around lines 599 and 876). The CPU path used standard
libjpeg-turbo BGRX-to-JPEG
encoding, also producing full-range values in the baseline tests below.

Reanalysis of the submitted vendor-driver capture included all 820 JPEGs,
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
full-range conversion smid previously used. This is the vendor binary on pony,
not a binary collected from the target system; the submitted capture shows
matching range characteristics. Disassembly is saved locally under `generated/`.

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

## Original full-range production-encoder test

`levels.c` links the production encoder and logger only. These baseline results
used the original full-range encoder. It encodes
synthetic BGRX flat greys and one-pixel grey borders on white through the actual
region API, both 1920x544 bands at y=0 and y=536, using CPU and iHD at Q70/Q98/Q100.
It rejects backend fallback. Pillow independently decodes the resulting JPEGs.
No USB/EVDI devices are opened. This local GPU is not the target system's HD 520.

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

These commands encode the currently checked-out source. Use revision `f581fd5`
to reproduce the full-range baseline above. With the limited-range correction,
the ordinary JPEG-to-RGB errors reported by `check_levels.py` are not acceptance
criteria: that decoder assumes full-range samples. Use the raw-sample tests in
the follow-up below to validate limited range. Preserve earlier generated files
when comparing revisions.

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
At the end of the initial audit, production source, the driver archive and the
user-service archive were unchanged.

## Limited-range correction and local validation

The follow-up changes both VA-API VPP paths to reduced-range BT.601 output while
keeping full-range RGB input. CPU encoding first uses libjpeg-turbo's RGB-to-YUV
conversion/subsampling, then scales Y to 16..235 and Cb/Cr to 16..240 around 128
before compressing those planes. The CPU scratch buffer is allocated at startup
and reused; only a larger variable-size request grows it. This covers the small
CPU companion rectangles as well as full-band CPU fallback. The extra integer
rounding is covered by the decoded-sample tolerances below.

The streaming log now includes `color-range=limited`. Both PIDs use this default;
there is no new range flag. JPEG quality control and retained-frame scheduling
are unchanged, including the separate idle-repaint limitation described above.

Validation uses the actual production encoder, with independent BT.601 expected
samples and raw Y/Cb/Cr JPEG decoding rather than a full-range RGB viewer:

- `tests/encoder_range.c`: 34 JPEGs pass with CPU full bands and another 34 with
  strict hardware full bands. The suite covers BGRX, RGB, writer and cropped
  region APIs at Q98/Q70/Q100/Q98, black/white endpoints, saturated colours,
  near-white one-pixel borders, sixteen 32x32 CPU companions and odd-sized CPU
  requests. For patch interiors, tolerances are two code values at Q98/Q100 and
  four at Q70; black/white endpoints must stay within one. Near-white borders
  through RGB 252 must still differ from white at high quality. Hardware mode
  rejects CPU fallback.
- The updated standalone v2 probe passes all 24 hardware JPEGs across copy and
  USER_PTR paths with maximum patch sample error of one. Its CPU self-test also
  passes 24 JPEGs. A deliberately missing VA driver correctly fails both paths
  rather than reporting CPU fallback as a hardware pass.
- CPU encoder tests pass AddressSanitizer and UndefinedBehaviorSanitizer,
  including the new plane buffer's growth, odd dimensions and destruction.
- The existing retained-frame/ACK and USB-selection tests pass. The complete
  executable builds locally with warnings treated as errors using system
  libjpeg-turbo and bundled userspace EVDI.

Local hardware is Intel Raptor Lake-S UHD Graphics (8086:a788), iHD 24.1.0 and
libva 2.20.0, not the target system's Skylake. Hardware fixtures use production
geometry: 1920x544 bands, and 1920x1080 USER_PTR sources at stride 7680 with crop
y=536.
An initial artificial 512x64 GPU fixture produced a small range offset (white
237 rather than 235); an artificial 1952-wide USER_PTR fixture failed allocation.
Those failed attempts remain in the local build logs. The production-size tests
pass without relaxing tolerances. This does not validate arbitrary VA-API surface
sizes; the runtime uses CPU encoding for 32x32 companions.

Commands for the added regression checks:

```sh
make -f tests/Makefile
LIBVA_DRIVER_NAME=iHD build/tests/encoder-range --vaapi
make -f tools/vaapi-probe.mk
build/vaapi-probe/smid-vaapi-probe --cpu-self-test build/probe-limited-cpu
LIBVA_DRIVER_NAME=iHD build/vaapi-probe/smid-vaapi-probe build/probe-limited-vaapi
```

Local logs are retained in `build/limited-range-tests-final.log`,
`build/limited-range-gpu-final.log`, `build/limited-range-sanitized.log`,
`build/probe-limited-{cpu,vaapi,negative}.log` and
`build/limited-range-driver-build.log`. No USB device or live display service
was touched for these checks. Physical highlight visibility, sustained CPU cost
and the corrected Skylake VPP output remain to be confirmed on the target setup.
The Debian 12 update is packaged separately as
`build/smid-0768-limited-range-debian12-amd64.tar.gz`, preserving the earlier
working driver and user-service archives for rollback.

The delivered archive was built from `0cf186e2b70727be3f0420e26758b7801a8c321f`:
SHA-256 `de6cdf2c99b1fcf611bd319d995e3e10e67129372b1c22231ac3077768a45655`
(239,108 bytes). The Debian 12 build passed all three test suites and CLI
help/invalid-PID/no-device checks with no GPU/USB passthrough. All 25 manifest
checksums, committed encoder/test source, executable mode and bundled EVDI
library/link were verified from the archive. The original archive's SHA-256
remains `6486c50fe2b75171a01bda6c36689219f6730bfb03bacc84aa8b177cb8855c6b`.
Packaging output and verification are retained in
`build/package-0768-limited-range.log` and
`build/limited-range-package-verification.json`.

## Physical follow-up shared 2026-11-14

The follow-up report confirms that the limited-range build substantially
improves image quality on the tested Acer and HQ monitors, with light-grey
borders and details now visible as expected. This supplies the physical output
confirmation that was pending above. The Rise Mode monitor still differs when connected through the
adapter; using direct HDMI for that monitor works well. Its remaining issue is
unresolved, so the report does not establish identical output on every monitor.

The latest approximate process CPU measurements are 31% for smid and 136% for
the Silicon Motion driver, about 77% lower. These are tester-reported values
from ordinary use, not a new controlled benchmark or independently inspected
pidstat log. They supersede neither the earlier exact samples nor their caveats.

After a Debian kernel update, EVDI was initially not loaded. The report states
that a reboot, stopping the proprietary driver and starting smid restored
operation.
The exact module-loading cause and automatic service recovery were not tested.
This follow-up was supplied as email text; no new raw logs or photos were
provided with it.
