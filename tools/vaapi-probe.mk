# Run from the repository (or source-bundle) root:
#   make -f tools/vaapi-probe.mk
# Only the encoder and logger are linked; no USB/EVDI dependencies.
CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -g -Wall -Wextra -Werror -std=c11 -D_GNU_SOURCE
PROBE_PKGS := libva libva-drm libturbojpeg
PROBE := build/vaapi-probe/smid-vaapi-probe
BUNDLE_SOURCES := encoder.c encoder.h log.c log.h protocol.h common.h LICENSE \
	tools/vaapi-probe.c tools/vaapi-probe.mk tools/run-vaapi-probe.sh tools/VAAPI-PROBE.md

.PHONY: all bundle
all: $(PROBE)

$(PROBE): tools/vaapi-probe.c encoder.c encoder.h log.c log.h protocol.h common.h tools/vaapi-probe.mk
	$(PKG_CONFIG) --print-errors --exists $(PROBE_PKGS)
	mkdir -p $(@D)
	$(CC) $(CFLAGS) -iquote . $$( $(PKG_CONFIG) --cflags $(PROBE_PKGS) ) \
		tools/vaapi-probe.c encoder.c log.c -o $@ \
		$$( $(PKG_CONFIG) --libs $(PROBE_PKGS) ) -lpthread

bundle: $(BUNDLE_SOURCES)
	mkdir -p build/vaapi-probe
	tar -czf build/vaapi-probe/smid-vaapi-probe.tar.gz \
		--transform='s,^,smid-vaapi-probe/,' $(BUNDLE_SOURCES)
	sha256sum build/vaapi-probe/smid-vaapi-probe.tar.gz
