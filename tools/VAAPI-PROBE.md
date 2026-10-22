# Standalone smid encoder probe

This tests the existing `encoder.c` using synthetic images. It does not link
USB or EVDI code, capture the desktop, or change the display configuration.
The working Silicon Motion driver can remain running. Run as your normal user.

## Debian 12 quick start

Install build dependencies (the previously configured iHD VA-API driver is also
needed):

```sh
sudo apt install build-essential pkg-config libva-dev libturbojpeg0-dev
```

For the source bundle, extract it and enter its directory:

```sh
tar -xzf smid-vaapi-probe.tar.gz
cd smid-vaapi-probe
```

From that directory, or from a full smid checkout:

```sh
bash tools/run-vaapi-probe.sh
```

The script defaults to `LIBVA_DRIVER_NAME=iHD`, builds the probe, runs it with a
60-second timeout and prints the path of a `build/vaapi-probe-results.*.tar.gz`
archive. Send that archive whether the test passes or fails. It contains a log,
synthetic reference PPMs and generated JPEGs. Build failures are also collected.
Each run creates a new directory, preserving previous results.

The production encoder selects the first openable node from
`/dev/dri/renderD128`, `/dev/dri/renderD129`, `/dev/dri/card0`. This matches the
known Intel `renderD128` configuration on Clayton's machine. The probe retains
that behaviour; it does not add a device-selection override. For other machines,
check the logged DRM paths before interpreting results.

## What it checks

- Actual VA-API initialization, packed JPEG headers and hardware encoding. Any
  fallback to the CPU encoder is a failure in the default test.
- Full-range BGRX upload, conversion to limited-range BT.601 NV12, and baseline
  4:2:0 JPEG (Y=16..235, Cb/Cr=16..240).
- The runtime `USER_PTR` import path with 4096-byte-aligned 1920x1080 buffers and
  7680-byte stride, cropped into 1920x544 bands at y=0 and y=536.
- Two encoder instances, one per band; changing pixels at an unchanged source
  pointer, switching source buffers, and alternating JPEG quality 90/75.
- Software decoding of all 24 images, checking dimensions, subsampling and
  colourspace, and comparing raw Y/Cb/Cr patch interiors with an independent
  limited-range BT.601 reference calculated from the source pixels.
  Per-channel mean absolute error must be at most 4 and maximum channel error
  at most 12 on the 0..255 scale. Patch edges are excluded because JPEG chroma
  subsampling blends colours there.

Probe v2 checks encoded sample levels directly. Ordinary JPEG viewers assume
full-range samples, so these device-targeted JPEGs will look lower-contrast in
such a viewer. The reference PPMs contain the original full-range RGB. A direct
visual comparison in a JPEG viewer does not test the adapter's range handling.
The earlier v1 probe checked full-range JPEG-to-RGB output and could therefore
pass without detecting a mismatch with the adapter's expected range.

The copy path is diagnostic; the region path is the one used for full bands in
smid. Both results are reported independently. A region failure matters even if
the copy path passes. The harness proceeds to the region test after a copy-path
failure unless the process itself crashes or times out.

This is a functional test, not a benchmark. Passing does not establish USB
initialization, dual-display operation, retained-frame behaviour, sustained
performance or CPU savings on a `090c:0768` adapter.

The probe uses the system libjpeg-turbo for decoding and the explicitly selected
CPU self-test. Its VA-API encoder code is the unmodified production `encoder.c`;
the normal smid build still uses its pinned static libjpeg-turbo. No USB/EVDI
development packages or downloads of vendored dependencies are needed here.

## Maintainer commands

Build, then run with explicit output-directory names (directories must be new):

```sh
make -f tools/vaapi-probe.mk
./build/vaapi-probe/smid-vaapi-probe --cpu-self-test build/probe-cpu
LIBVA_DRIVER_NAME=iHD ./build/vaapi-probe/smid-vaapi-probe build/probe-vaapi
LIBVA_DRIVER_NAME=smid_missing_driver ./build/vaapi-probe/smid-vaapi-probe build/probe-no-driver
```

The CPU self-test must say `SELF-TEST PASS`, not `PROBE PASS`. The missing-driver
case must exit nonzero and reject the encoder's automatic CPU fallback.

Create the source-only attachment, containing the existing encoder/logger, probe,
build/run scripts, these instructions and the project license:

```sh
make -f tools/vaapi-probe.mk bundle
```

Output: `build/vaapi-probe/smid-vaapi-probe.tar.gz`. This archive contains source,
not a prebuilt executable or tester data.
