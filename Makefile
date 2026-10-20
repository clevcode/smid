CC ?= gcc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O3 -Wall -Wextra -Werror -std=c11 -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L
DEPFLAGS := -MMD -MP
NPROC ?= $(shell nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)

TURBOJPEG_REPO ?= https://github.com/libjpeg-turbo/libjpeg-turbo.git
TURBOJPEG_REF ?= afad69dafa6193d838ed075dc34652e646bf745e
TURBOJPEG_SRC ?= vendor/libjpeg-turbo
TURBOJPEG_BUILD ?= $(TURBOJPEG_SRC)/build
TURBOJPEG_STATIC ?= $(TURBOJPEG_BUILD)/libturbojpeg.a
TURBOJPEG_SOURCE_STAMP ?= $(TURBOJPEG_SRC)/.smid-source-$(TURBOJPEG_REF)
TURBOJPEG_BUILD_LABEL ?= 0
TURBOJPEG_SCRATCH ?= $(TURBOJPEG_BUILD)/scratch
TURBOJPEG_CFLAGS ?= -I$(TURBOJPEG_SRC)/src
TURBOJPEG_LIBS ?= $(TURBOJPEG_STATIC) -lm
NASM ?= $(shell command -v nasm 2>/dev/null || command -v yasm 2>/dev/null)

PKGS := libusb-1.0 libva libva-drm
USE_BUNDLED_EVDI ?= 0
EVDI_SRC ?= vendor/evdi
EVDI_REF := 9905b9bfed8b7e49befa0530bfb6e464380d938f
EVDI_STAMP := $(EVDI_SRC)/.smid-source-$(EVDI_REF)
ifeq ($(USE_BUNDLED_EVDI),1)
EVDI_CFLAGS := -I$(EVDI_SRC)/library
EVDI_LIBS := -L$(EVDI_SRC)/library -Wl,-rpath,'$$ORIGIN/vendor/evdi/library' -levdi
EVDI_DEPS := $(EVDI_SRC)/library/libevdi.so
EVDI_CONNECT2_FLAG := -DSMID_EVDI_CONNECT2
else
EVDI_CFLAGS :=
EVDI_LIBS := -levdi
EVDI_DEPS :=
# Debian 12-era libevdi calls the five-argument API evdi_connect; newer
# releases renamed it to evdi_connect2. Inspect the installed header.
EVDI_CONNECT2_FLAG := $(shell printf '\043include <stdint.h>\n\043include <evdi_lib.h>\nvoid f(void) { (void)evdi_connect2; }\n' | $(CC) -x c -fsyntax-only - >/dev/null 2>&1 && printf '%s' '-DSMID_EVDI_CONNECT2')
endif
USE_SYSTEM_TURBOJPEG ?= 0
ifeq ($(USE_SYSTEM_TURBOJPEG),1)
PKGS += libturbojpeg
TURBOJPEG_CFLAGS :=
TURBOJPEG_LIBS :=
TURBOJPEG_DEPS :=
else
TURBOJPEG_DEPS := $(TURBOJPEG_STATIC)
endif
PKG_CFLAGS := $(TURBOJPEG_CFLAGS) $(shell $(PKG_CONFIG) --cflags $(PKGS))
PKG_CFLAGS += $(EVDI_CFLAGS) $(EVDI_CONNECT2_FLAG)
PKG_LIBS := $(shell $(PKG_CONFIG) --libs $(PKGS)) $(TURBOJPEG_LIBS) $(EVDI_LIBS)

OBJS := cnm.o encoder.o evdi_source.o frame_packet.o heartbeat.o main.o log.o protocol.o time.o transport.o usb.o
DEPS := $(OBJS:.o=.d)

.PHONY: all clean distclean libjpeg-turbo

all: smid

smid: $(OBJS) $(TURBOJPEG_DEPS) $(EVDI_DEPS)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(PKG_LIBS) -lpthread

libjpeg-turbo: $(TURBOJPEG_STATIC)

$(TURBOJPEG_SOURCE_STAMP):
	mkdir -p $(dir $(TURBOJPEG_SRC))
	@if [ ! -d "$(TURBOJPEG_SRC)/.git" ]; then \
		rm -rf "$(TURBOJPEG_SRC)"; \
		git init "$(TURBOJPEG_SRC)"; \
		git -C "$(TURBOJPEG_SRC)" remote add origin "$(TURBOJPEG_REPO)"; \
	fi
	git -C "$(TURBOJPEG_SRC)" fetch --depth 1 origin "$(TURBOJPEG_REF)"
	git -C "$(TURBOJPEG_SRC)" checkout --detach FETCH_HEAD
	@actual=$$(git -C "$(TURBOJPEG_SRC)" rev-parse HEAD); \
	if [ "$$actual" != "$(TURBOJPEG_REF)" ]; then \
		echo "error: libjpeg-turbo checkout is $$actual, expected $(TURBOJPEG_REF)" >&2; \
		exit 1; \
	fi
	rm -f "$(TURBOJPEG_SRC)"/.smid-source-*
	touch "$@"

$(TURBOJPEG_STATIC): $(TURBOJPEG_SOURCE_STAMP)
	@if [ -z "$(NASM)" ]; then \
		echo "error: nasm or yasm is required for a SIMD libjpeg-turbo static build" >&2; \
		exit 1; \
	fi
	rm -rf $(TURBOJPEG_BUILD)
	mkdir -p $(TURBOJPEG_SCRATCH)
	TMPDIR=$(abspath $(TURBOJPEG_SCRATCH)) cmake -S $(TURBOJPEG_SRC) -B $(TURBOJPEG_BUILD) -G Ninja \
		-DCMAKE_BUILD_TYPE=Release \
		-DBUILD=$(TURBOJPEG_BUILD_LABEL) \
		-DENABLE_SHARED=OFF -DENABLE_STATIC=ON \
		-DWITH_TOOLS=OFF -DWITH_TESTS=OFF \
		-DWITH_TURBOJPEG=ON -DWITH_SIMD=ON \
		-DWITH_SYSTEM_SPNG=OFF -DWITH_SYSTEM_ZLIB=OFF \
		-DCMAKE_POSITION_INDEPENDENT_CODE=ON \
		-DCMAKE_ASM_NASM_COMPILER=$(NASM)
	TMPDIR=$(abspath $(TURBOJPEG_SCRATCH)) cmake --build $(TURBOJPEG_BUILD) --target turbojpeg-static -j$(NPROC)
	rm -rf $(TURBOJPEG_SCRATCH) $(TURBOJPEG_BUILD)/pkgscripts
	rm -f $(TURBOJPEG_BUILD)/CMakeFiles/CMakeConfigureLog.yaml

encoder.o: $(TURBOJPEG_DEPS)
evdi_source.o: $(EVDI_DEPS)

$(EVDI_STAMP):
	@if [ ! -d "$(EVDI_SRC)" ]; then \
		git clone https://github.com/DisplayLink/evdi.git "$(EVDI_SRC)"; \
		git -C "$(EVDI_SRC)" checkout --detach "$(EVDI_REF)"; \
	fi
	@test "$$(git -C "$(EVDI_SRC)" rev-parse HEAD)" = "$(EVDI_REF)" || \
		{ echo "unexpected EVDI checkout; expected $(EVDI_REF)" >&2; exit 1; }
	touch "$@"

$(EVDI_SRC)/library/libevdi.so: $(EVDI_STAMP)
	$(MAKE) -C $(EVDI_SRC)/library

%.o: %.c Makefile
	$(CC) $(CFLAGS) $(DEPFLAGS) $(PKG_CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(DEPS) smid

distclean: clean
	rm -rf vendor

-include $(DEPS)
