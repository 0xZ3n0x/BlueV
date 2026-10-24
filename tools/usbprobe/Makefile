# SPDX-License-Identifier: GPL-3.0-or-later
# usbprobe: asks libSceUsbd about the USB devices (ps5-payload-sdk)
PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

CFLAGS := -O2 -Wall -Wextra

usbprobe.elf: usbprobe.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f usbprobe.elf

.PHONY: clean
