# Run from the repository root: make -f tests/Makefile
CC ?= cc
FLAGS := -O2 -g -Wall -Wextra -Werror -std=c11 -D_GNU_SOURCE -iquote . \
    -ffunction-sections -fdata-sections $(shell pkg-config --cflags libusb-1.0)
WRAPS := get_device get_config_descriptor_by_value free_config_descriptor \
    set_auto_detach_kernel_driver get_configuration set_configuration \
    claim_interface set_interface_alt_setting release_interface close
.PHONY: all
all: build/tests/runtime build/tests/usb-selection
	./build/tests/runtime
	./build/tests/usb-selection
build/tests/runtime: tests/runtime.c main.c protocol.c protocol.h usb.h evdi_source.h
	mkdir -p $(@D)
	$(CC) $(FLAGS) tests/runtime.c protocol.c -Wl,--gc-sections -o $@
build/tests/usb-selection: tests/usb_selection.c usb.c usb.h protocol.c protocol.h
	mkdir -p $(@D)
	$(CC) $(FLAGS) tests/usb_selection.c protocol.c log.c -pthread -Wl,--gc-sections \
		$(foreach f,$(WRAPS),-Wl,--wrap=libusb_$(f)) $(shell pkg-config --libs libusb-1.0) -o $@
