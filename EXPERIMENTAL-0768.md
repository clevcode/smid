# Experimental Wavlink 090c:0768 build

This is the full smid driver with two EVDI displays, VA-API JPEG encoding,
native cursor updates and dirty-frame skipping. It selects display interface
4/alternate 1 for the 0768. Retained-frame companion updates remain enabled by
default; `--no-retained-frames` disables them if they cause stale/black regions.

Clayton's Intel HD 520 passed the production encoder probe and, on October 21,
he confirmed both USB monitors, cursors and desktop interaction work correctly
with retained frames enabled. The supplied logs show direct VA-API encoding and
clean shutdown in the timed run. Long-term stability remains under evaluation.
For automatic startup as a regular user, see [the service setup](tools/SYSTEMD.md).

This updated build changes both VA-API and CPU conversion to limited-range
BT.601, matching the vendor's encoded colour levels. It addresses the suspected
cause of light-grey borders disappearing against white, even at fixed Q98.
Local encoder checks pass, including near-white borders and retained-frame
companion rectangles; the improvement on Clayton's physical monitors still
needs confirmation. Limited range is the default; no extra flag is needed.

## Run the supplied Debian 12 amd64 executable

Extract the attachment in your home directory:

```sh
tar -xzf smid-0768-limited-range-debian12-amd64.tar.gz
cd smid-0768-limited-range
sha256sum -c SHA256SUMS
```

The EVDI userspace library is included and loaded relative to the executable.
Keep `vendor/evdi/library/` beside it. This does not install or replace the EVDI
kernel module or any system library. Keep the working vendor installation.

The remaining runtime libraries should already be installed from the previous
tests; if needed:

```sh
sudo apt install libusb-1.0-0 libva2 libva-drm2 libturbojpeg0
```

Use the laptop panel or directly connected HDMI screen for the terminal. Stop
the vendor service, then start smid (keep the adapter connected):

```sh
sudo systemctl mask --runtime --now smiusbdisplay.service
sudo env LIBVA_DRIVER_NAME=iHD ./smid --usb-pid 0768 2>&1 | tee smid-0768.log
```

The two USB displays may briefly go blank during the handover. If GNOME does
not enable the new displays automatically, enable them in Display Settings.
The process runs until Ctrl+C; `--seconds 120` can be added for a timed run.
The runtime mask prevents the vendor service from automatically reclaiming the
USB interface. It lasts until unmasked or the next reboot.

If the picture has stale or black regions, stop smid and rerun with:

```sh
sudo env LIBVA_DRIVER_NAME=iHD ./smid --usb-pid 0768 --no-retained-frames \
  2>&1 | tee smid-0768-full-frames.log
```

This sends both full bands whenever a display changes. It still skips completely
unchanged frames. There is no need to run another diagnostic probe first.

After stopping smid, restore the vendor service:

```sh
sudo systemctl unmask --runtime smiusbdisplay.service
sudo systemctl start smiusbdisplay.service
```

If necessary, unplug/replug the adapter after starting the vendor service. If
the service has a different name, use its actual name in the stop/start commands;
both userspace drivers must not run together. No permanent service/udev changes
are required for the root-run trial above.

For this update, compare the same light-grey input borders against white and
report whether their visibility improves. The startup log should include
`color-range=limited`. Start with normal adaptive quality; `--jpeg-quality 98
--jpeg-fixed-quality` remains available for a fixed-quality comparison.

Send the smid log and whether both desktops and cursors appeared correctly.
The log records USB selection, initialization failures, encoder selection and
retained-frame mode and colour range. CPU savings should be measured during
comparable desktop activity after output is working.

## Build from the included sources

The archive includes the pinned EVDI sources and LGPL library license. The EVDI
module sources are present as part of that upstream tree but are not built or
installed by these commands:

```sh
sudo apt install build-essential pkg-config libusb-1.0-0-dev libva-dev \
  libdrm-dev libturbojpeg0-dev
make clean
make -C vendor/evdi/library clean
make -j4 USE_SYSTEM_TURBOJPEG=1 USE_BUNDLED_EVDI=1
```

Use `make -f tests/Makefile` for tests that do not access USB or EVDI devices.

## Maintainer packaging

With the pinned `vendor/evdi` git checkout present:

```sh
bash tools/package-0768.sh
```

The Docker build uses Debian 12 headers/libraries. Its tests have no USB/GPU
device passthrough. `BUILD-INFO.txt` and `SHA256SUMS` identify the supplied build.
