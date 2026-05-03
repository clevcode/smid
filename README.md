# smid

`smid` is a Linux userspace driver for the Silicon Motion USB display path used
by portable triple-monitor adapters such as the Triple Aero Pro Max by Aura
Displays. One panel is usually driven by normal USB-C DisplayPort Alt Mode;
`smid` drives the two USB-attached panels exposed by the adapter's `090c:0760`
USB device.

The driver creates EVDI virtual displays, captures their framebuffers, encodes
the required JPEG bands, and sends the adapter protocol over USB. The default
runtime path uses VA-API for hardware accelerated JPEG encoding, adaptive JPEG
quality, cursor commands, USB recovery, and per-frame dirty detection.

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

Useful cleanup targets:

```sh
make clean
make distclean
```

## Permissions

Install local udev rules once:

```sh
sudo ./install-udev-rules.sh
```

The rule grants access to the `090c:0760` USB device and EVDI DRM devices for an
active local seat. Log out and back in if the installer adds your user to a new
group.

If you have already installed the vendor userspace helper, disable or uninstall
that before running `smid`; both drivers cannot own the same USB device at the
same time.

## Run

```sh
./smid
```

By default this starts two EVDI streams, enables cursor events, uses direct
VA-API JPEG encoding when available, and adapts quality around a 36 MiB/s target.

Common options:

```text
--encoder cpu|direct-vaapi
--jpeg-target-mib-s N
--jpeg-quality N
--jpeg-fixed-quality
--evdi-streams 1|2
--seconds N
```

Dirty detection uses the tested hash-bands path. The top and bottom bands are
hashed independently, and unchanged bands can be represented by a small
companion update when the adapter's retained-frame basis is known.

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
