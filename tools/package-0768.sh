#!/usr/bin/env bash
# Build a source + amd64 executable bundle against Debian 12, without device access.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
repo=$PWD
if [ -n "$(git status --porcelain)" ]; then
    printf 'Commit the reviewed source state before packaging.\n' >&2
    exit 1
fi
ref=9905b9bfed8b7e49befa0530bfb6e464380d938f
test "$(git -C vendor/evdi rev-parse HEAD)" = "$ref"
mkdir -p build
stage=$(mktemp -d "$repo/build/0768-package.XXXXXX")
dest="$stage/smid-0768"
mkdir -p "$dest/tools" "$dest/tests" "$dest/vendor/evdi"
cp -- *.c *.h Makefile LICENSE README.md EXPERIMENTAL-0768.md \
    99-smid.rules install-udev-rules.sh "$dest/"
cp tools/*.c tools/*.mk tools/*.md tools/*.sh tools/*.service tools/Dockerfile.bookworm "$dest/tools/"
cp tests/Makefile tests/runtime.c tests/usb_selection.c "$dest/tests/"
git -C vendor/evdi archive "$ref" | tar -xf - -C "$dest/vendor/evdi"
touch "$dest/vendor/evdi/.smid-source-$ref"
{
    printf 'smid source revision: '
    git rev-parse HEAD
    printf 'EVDI source revision: %s (upstream 1.14.2, LGPL-2.1 library)\n' "$ref"
    printf 'Build: Debian 12 amd64; system libusb, VA-API and libjpeg-turbo.\n'
    printf 'Physical 090c:0768 operation remains experimental.\n'
} > "$dest/BUILD-INFO.txt"
docker build -f tools/Dockerfile.bookworm -t smid-bookworm-builder:local .
docker run --rm --network none --platform linux/amd64 \
    --user "$(id -u):$(id -g)" --mount "type=bind,src=$dest,dst=/src" \
    smid-bookworm-builder:local sh -c '
        set -e
        make -j4 USE_SYSTEM_TURBOJPEG=1 USE_BUNDLED_EVDI=1
        make -f tests/Makefile
        ./smid --help
        if ./smid --usb-pid invalid; then exit 1; else test "$?" = 2; fi
        if ./smid --usb-pid 0768; then exit 1; else test "$?" = 1; fi
        dpkg-query -W libc6 libusb-1.0-0 libva2 libturbojpeg0 >> BUILD-INFO.txt
        ldd ./smid >> BUILD-INFO.txt
        make -C vendor/evdi/library -q
    '
# Keep the complete EVDI sources/licenses and its shared library, but no object files.
rm -f "$dest"/*.o "$dest"/*.d "$dest/vendor/evdi/library/evdi_lib.o"
rm -rf "$dest/build"
(
    cd "$dest"
    sha256sum -- *.c *.h Makefile smid vendor/evdi/library/libevdi.so.1.14.2 > SHA256SUMS
)
archive="$repo/build/smid-0768-debian12-amd64.tar.gz"
tar -czf "$archive" -C "$stage" smid-0768
sha256sum "$archive"
printf 'Bundle: %s\nBuild tree: %s\n' "$archive" "$dest"
