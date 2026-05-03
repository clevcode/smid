#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
RULE_SRC="$ROOT/99-smid.rules"
RULE_DST=/etc/udev/rules.d/99-smid.rules
USER_NAME=${SUDO_USER:-${USER:-}}

if [ "$(id -u)" -ne 0 ]; then
  exec sudo "$0" "$@"
fi

if [ ! -r "$RULE_SRC" ]; then
  echo "missing rule file: $RULE_SRC" >&2
  exit 1
fi

install -m 0644 "$RULE_SRC" "$RULE_DST"
udevadm control --reload-rules
udevadm trigger --subsystem-match=usb --attr-match=idVendor=090c --attr-match=idProduct=0760 --action=change || true
udevadm trigger --subsystem-match=drm --action=change || true

if [ -n "$USER_NAME" ]; then
  usermod -aG plugdev,video "$USER_NAME"
fi

echo "installed $RULE_DST"
echo "added ${USER_NAME:-current user} to plugdev,video if available"
echo "log out and back in for group membership to affect new shells"
echo "current matching nodes:"
for dev in /dev/bus/usb/*/*; do
  [ -e "$dev" ] || continue
  props=$(udevadm info -q property -n "$dev" 2>/dev/null || true)
  if grep -q '^ID_VENDOR_ID=090c$' <<<"$props" &&
      grep -q '^ID_MODEL_ID=0760$' <<<"$props"; then
    ls -l "$dev"
  fi
done
ls -l /dev/dri/card* 2>/dev/null || true
