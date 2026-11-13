# Wavlink 0768: run smid as your desktop user

This setup targets Debian 12 with GNOME on X11. It uses the already working
executable and starts it at graphical login. It does not provide
USB monitors at the login screen. The service file has been syntax-checked;
login, reboot, suspend/resume and hotplug still need normal-use validation on the
target machine. No additional encoder or USB capture probe is needed.

Keep the working Silicon Motion installation, including its EVDI kernel module.
Only its userspace service needs to be masked. All administrative commands below
are one-time setup; the smid process runs as your regular user.

## 1. Keep the working build at a stable path

Stop any manually running smid with Ctrl+C. From the directory containing your
working `smid` executable and its `vendor/evdi/library/` directory:

```sh
mkdir -p "$HOME/.local/opt/smid-0768"
cp -a ./smid ./vendor "$HOME/.local/opt/smid-0768/"
```

The bundled EVDI library must stay beside the executable at that relative path.
Do not replace the executable with a new build just to install this service.

## 2. Keep the vendor service stopped

Use a terminal on the laptop panel or directly connected HDMI screen:

```sh
sudo systemctl mask --now smiusbdisplay.service
```

Unlike the temporary `--runtime` mask, this survives a reboot. Keep the mask in
place while smid is your default driver. A plain stop was insufficient on the
tested installation. Masking prevents systemd activation, including requests
from vendor hotplug rules. The logs do not identify the exact restart trigger.
If masking reports an existing file instead of succeeding, stop here and inspect
`systemctl cat smiusbdisplay.service`; do not delete the vendor unit file.

## 3. Grant device access and prepare EVDI for boot

From the repository or driver package root, run its permission installer:

```sh
sudo bash ./install-udev-rules.sh
```

It installs `99-smid.rules` and adds your user to `plugdev`, `video`, and (where
present) `render`. These cover the USB device, EVDI card nodes and Intel's VA-API
render node. The reboot below refreshes group membership for the user service
manager as well as your shell.

The EVDI devices must already exist when smid starts as a regular user. Creating
them through `/sys/devices/evdi/add` requires root. The `modinfo` output supplied
for the tested installation confirms support for `initial_device_count`.

Arrange for the installed module to load at boot:

```sh
printf '%s\n' evdi | sudo tee /etc/modules-load.d/smid.conf
/usr/sbin/modprobe -c | grep '^options evdi '
```

If the displayed configuration already sets `initial_device_count=2` or higher,
keep it. Otherwise add:

```sh
printf '%s\n' 'options evdi initial_device_count=2' \
  | sudo tee /etc/modprobe.d/zz-smid.conf
sudo update-initramfs -u
```

This pre-creates two reusable EVDI displays without granting the user write
access to sysfs or unloading a module used by the current desktop. The initramfs
update also carries the option into early boot if EVDI is loaded there.

## 4. Install the user service and reboot when convenient

From the repository or full driver package root:

```sh
mkdir -p "$HOME/.config/systemd/user"
install -m 0644 ./tools/smid-0768.service "$HOME/.config/systemd/user/"
systemctl --user daemon-reload
systemctl --user enable smid-0768.service
```

In the older service-only setup bundle, use `./smid-0768.service` instead.
Save your work and reboot when convenient. At the next GNOME login,
smid starts with iHD and retained-frame optimization enabled. The service uses
no `sudo` and has no `--seconds` limit.

Check it from your desktop session:

```sh
systemctl --user status smid-0768.service
journalctl --user -u smid-0768.service -b --no-pager
```

Look for both EVDI modes and `encoder=direct-vaapi` in the log. A systemd
`active` status alone does not establish that the displays or VA-API are working.
After a failure (including an absent adapter), the service retries every 20
seconds. An explicit `systemctl --user stop smid-0768.service` stops these
retries. Reconnecting an adapter can require re-enabling its displays in GNOME.

Normal controls:

```sh
systemctl --user stop smid-0768.service
systemctl --user start smid-0768.service
systemctl --user restart smid-0768.service
```

If retained-frame artifacts ever appear, add `--no-retained-frames` to `ExecStart`
in `~/.config/systemd/user/smid-0768.service`, then run `systemctl --user
daemon-reload` and `systemctl --user restart smid-0768.service`.

## Restore the original driver

```sh
systemctl --user disable --now smid-0768.service
sudo systemctl unmask smiusbdisplay.service
sudo systemctl unmask --runtime smiusbdisplay.service
sudo systemctl start smiusbdisplay.service
```

If needed, unplug/replug the adapter after the vendor service starts. The
permission rules and pre-created EVDI cards can remain. To also undo the boot
configuration added above, remove only `/etc/modules-load.d/smid.conf` and
`/etc/modprobe.d/zz-smid.conf` if you created them for this setup, then run
`sudo update-initramfs -u`. Preserve any pre-existing vendor EVDI configuration.

References: [systemd graphical session lifecycle](https://www.freedesktop.org/software/systemd/man/252/systemd.special.html#graphical-session.target),
[service restart behavior](https://www.freedesktop.org/software/systemd/man/252/systemd.service.html#Restart=),
[service masking](https://www.freedesktop.org/software/systemd/man/latest/systemctl.html#mask%20UNIT%E2%80%A6),
[Debian modprobe options](https://manpages.debian.org/bookworm/kmod/modprobe.d.5.en.html).
