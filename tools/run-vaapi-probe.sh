#!/usr/bin/env bash
# Invoke with bash; output stays under this source tree, including on failure.
set -uo pipefail
cd -- "$(dirname -- "$0")/.." || exit 2
mkdir -p build || exit 2
results=$(mktemp -d "$PWD/build/vaapi-probe-results.XXXXXX") || exit 2
(
    set -e
    printf 'smid standalone encoder probe\n'
    uname -sr
    printf '\nSource checksums:\n'
    sha256sum encoder.c tools/vaapi-probe.c
    printf '\nDependency versions (libva, libva-drm, libturbojpeg):\n'
    pkg-config --modversion libva libva-drm libturbojpeg
    printf '\nDRM device paths:\n'
    ls -l /dev/dri/by-path/ || true
    printf '\nBuild:\n'
    make -f tools/vaapi-probe.mk
    printf '\nRun (60 second timeout):\n'
    LIBVA_DRIVER_NAME="${LIBVA_DRIVER_NAME:-iHD}" \
        timeout 60s ./build/vaapi-probe/smid-vaapi-probe "$results/images"
) >"$results/probe.log" 2>&1
status=$?
printf '\nBuild/probe exit status: %d (124 means timeout)\n' "$status" >>"$results/probe.log"
cat "$results/probe.log"
archive="$results.tar.gz"
if ! tar -czf "$archive" -C "$(dirname -- "$results")" "$(basename -- "$results")"; then
    printf '\nCould not package results; files remain in %s\n' "$results" >&2
    exit 2
fi
printf '\nPlease send this results archive, whether the probe passed or failed:\n%s\n' "$archive"
exit "$status"
