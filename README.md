# smid

`smid` is a Linux userspace driver for the Silicon Motion USB display path used
by portable triple-monitor adapters such as the Triple Aero Pro Max by Aura
Displays. One panel is usually driven by normal USB-C DisplayPort Alt Mode;
`smid` drives the two USB-attached panels exposed by the adapter's `090c:0760`
USB device.

Experimental support for Wavlink `090c:0768` adapters is available with
`--usb-pid 0768`. See [the 0768 build and run instructions](EXPERIMENTAL-0768.md).
Clayton has confirmed dual 1080p output, native cursors and retained-frame
updates on his Debian 12 / Intel HD 520 setup; longer-term validation continues.

The driver creates EVDI virtual displays, captures their framebuffers, encodes
the required JPEG bands, and sends the adapter protocol over USB. The default
runtime path uses VA-API for hardware accelerated JPEG encoding, adaptive JPEG
quality, cursor commands, USB recovery, and per-frame dirty detection.

Both encoders convert desktop RGB into limited-range BT.601 samples before
JPEG compression, matching the vendor driver's colour levels. This also
applies to CPU-generated companion rectangles. JPEG quality controls compression
loss separately; increasing it cannot correct a colour-range mismatch.

## Requirements

- Linux with the EVDI kernel module and libevdi development files
- `libusb-1.0`, `libva`, and `libva-drm` development files
- `pkg-config`, `git`, `cmake`, `ninja`, a C compiler, and `nasm` or `yasm`
- A VA-API JPEG-capable driver for the fast path

The CPU encoder remains available as a fallback, but it uses noticeably more CPU
than the VA-API path.

## Build

```sh
make -j"$(nproc)"
```

The first build fetches a pinned libjpeg-turbo revision into
`vendor/libjpeg-turbo` and builds a static `libturbojpeg.a` with SIMD enabled.
That vendor checkout is a build artifact, not part of the release source.

For distributions without libevdi development packages, an optional pinned
userspace library can be built locally. This does not build or replace the EVDI
kernel module. `libdrm` development headers are needed for the library:

```sh
make -j"$(nproc)" USE_SYSTEM_TURBOJPEG=1 USE_BUNDLED_EVDI=1
```

This option uses system libjpeg-turbo (including its development headers) and
keeps the EVDI library under `vendor/evdi`. Run `make clean` before switching
build configurations.

Useful cleanup targets:

```sh
make clean
make distclean
```

To test the colour-conversion and JPEG pipeline independently of USB/EVDI, use
the [standalone encoder probe](tools/VAAPI-PROBE.md). It builds the existing
encoder against system VA-API/libjpeg-turbo libraries and checks synthetic
images through both the upload and runtime memory-import paths:

```sh
bash tools/run-vaapi-probe.sh
```

## Permissions

Install local udev rules once:

```sh
sudo ./install-udev-rules.sh
```

The rules grant access to the `090c:0760` and `090c:0768` USB devices and EVDI
DRM devices. The installer adds your user to `plugdev`, `video`, and `render`
(if present) for VA-API. Log out and back in after group changes.

If you have already installed the vendor userspace helper, mask its service
before running `smid`; both drivers cannot own the same USB device at the same
time. Keep its working EVDI kernel module installed.

For GNOME login-time startup without running smid as root, see the
[0768 user-service setup](tools/SYSTEMD.md). It also covers EVDI device creation
at boot, masking a vendor service that reactivates itself, and rollback.

## Run

```sh
./smid
```

By default this starts two EVDI streams, enables cursor events, uses direct
VA-API JPEG encoding when available, and adapts quality around a 36 MiB/s target.

Common options:

```text
--encoder cpu|direct-vaapi
--usb-pid 0760|0768
--no-retained-frames
--jpeg-target-mib-s N
--jpeg-quality N
--jpeg-fixed-quality
--evdi-streams 1|2
--seconds N
```

Dirty detection uses the tested hash-bands path. The top and bottom bands are
hashed independently, and unchanged bands can be represented by a small
companion update when the adapter's retained-frame basis is known.
`--no-retained-frames` sends both full JPEG bands for each changed display,
while still skipping completely unchanged frames. Retained-frame updates are
enabled by default for both supported PIDs.

## Notes

- The adapter expects each USB display frame as two JPEG bands.
- Cursor updates are sent through native cursor commands instead of repainting
  the framebuffer.
- USB endpoint recovery keeps the EVDI displays open, so window placement should
  survive transient adapter reconnects.
- Sequence wrap is handled by reconnecting the USB session and forcing a full
  refresh.
- Reverse-engineering tools, packet experiments, and capture-analysis helpers
  have been intentionally excluded from this release tree.

## License

MIT License. See [LICENSE](LICENSE).
