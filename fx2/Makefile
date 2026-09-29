TARGET    = bugslayer-fx2
SOURCES   = src/main
LIBRARIES = fx2 fx2usb fx2isrs
MODEL     = small

# VID/PID here are only used by the `load` target, which RAM-loads over USB
# during bring-up -- at which point the FX2 is still the ROM's 04B4:8613.
VID       = 04B4
PID       = 8613

LIBFX2   ?= libfx2/firmware/library
include $(LIBFX2)/fx2rules.mk

# C2 EEPROM image, and the C array the RP2350 serves it from. The header is
# committed, so the RP2350 build never needs SDCC: rerun `make c2` and commit
# ../rp2350/src/fx2_image.h after every FX2 firmware change.
c2: $(TARGET).ihex
	python3 tools/mkc2.py --vid 0x35F0 --pid 0xDB13 \
		--header ../rp2350/src/fx2_image.h $(TARGET).ihex build/$(TARGET).c2

.PHONY: c2

# fx2rules.mk lists the libraries as prerequisites but only knows how to build
# them via .stamp, so a fresh checkout fails with "No rule to make target".
$(LIBFX2)/lib/$(MODEL)/%.lib: $(LIBFX2)/.stamp ;
