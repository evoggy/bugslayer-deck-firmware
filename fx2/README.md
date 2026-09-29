# CBM9002A (FX2LP) firmware — slave FIFO bridge

~30 lines of register init plus a descriptor set. In `AUTOIN` mode the 8051 is out of the data
path entirely; after boot it does nothing.

**The design is already written up in Obsidian: `Projects/Bugslayer/Deck/FX2 firmware.md`.**
Register values, the C0/C2 boot trick, the Glasgow/libfx2 provenance and the evidence review
all live there. This README is the build entry point, not the design.

## Base and reference

- **[libfx2](https://github.com/whitequark/libfx2)** (0-BSD) — the base. USB core, standard
  requests, `usbmicrosoft.h`, EEPROM helpers, SDCC build rules, and `fx2tool` for RAM-loading
  during bring-up.
- **[Glasgow `firmware/fx2/`](https://github.com/GlasgowEmbedded/glasgow/tree/main/firmware/fx2)**
  (0-BSD) — the reference. Same chip, EEPROM boot at 0xA2, MS OS 1.0 `WINUSB` descriptors.
  Copy the descriptor set and the FIFO reset dance; discard the FPGA and management halves.
- ❌ **Not** sigrok `fx2lafw` — GPL-2+, host-loaded (so `04B4:8613` and Zadig on Windows), and
  it is a logic analyzer sampling its own port pins, not a FIFO bridge. Useful as a bring-up
  tool only, like `fx2pipe`.

## Configuration

Sync slave FIFO, **IN-only**, `AUTOIN`, **external IFCLK** driven by the RP2350 at up to 37.5 MHz.
EP6 bulk IN, 512 bytes, quad-buffered. FIFOADR is strapped to EP6 in hardware; SLRD and SLOE
are tied inactive. `FLAGB` = EP6 **programmable-level flag**, asserting `PF_SLACK` (16) bytes
before full; the PF pin is active **high** (measured, the TRM does not say).

Gotchas that will cost an afternoon each:
`SYNCDELAY` between FIFO/endpoint register writes · `WORDWIDE` defaults **on** and must be
cleared · `PINFLAGSAB = 0x60` for FLAGB = EP6 PF, with `EP6FIFOPFH/L = 0x99/0xF0`
(3 committed packets + 496 bytes, TRM §15.6.5) · bump `bcdDevice` on every descriptor change
during development, because Windows caches the MS-OS-descriptor answer per VID/PID/bcdDevice.

## Build

libfx2 is a submodule (`git submodule update --init`). Needs `sdcc` (`apt install sdcc`,
4.2.0 tested); libfx2's own library builds on first `make`:

```
make            # -> bugslayer-fx2.ihex
make c2         # -> build/bugslayer-fx2.c2 and ../rp2350/src/fx2_image.h
```

**After changing the FX2 firmware, run `make c2` and commit `rp2350/src/fx2_image.h`.** That
header is what the RP2350 serves at boot, and it's committed so the RP2350 build never needs
SDCC. To iterate without reflashing the RP2350, boot the FX2 in C0 mode and RAM-load:

```
host/bslyctl.py "fx2 boot c0" "fx2 reboot"
fx2tool -d 35f0:db13 load fx2/bugslayer-fx2.ihex
```

`tools/mkc2.py` is standalone — it parses the Intel HEX and emits the C2 image itself, so the
RP2350 build never needs SDCC or libfx2's Python. `--mode c0` emits the 8-byte header-only
image for development (our VID/PID, host RAM-loads the firmware).

## Boot

The RP2350 emulates a 16-bit-addressed EEPROM at 0xA2 (7-bit 0x51) on GP4/GP5 (i2c0,
400 kHz) and serves the C2 image (`rp2350/src/fx2_boot.c`). It patches its own serial into the
`BSLYSERIAL000000` placeholder first, so both devices report the same `iSerialNumber`. At
power-on the RP2350 starts IFCLK and releases `FX_RESET#`, and the FX2 enumerates about 0.2 s
later. `fx2 boot` on the control plane picks what it serves at the next `fx2 up`/`fx2 reboot`:

| `fx2 boot` | EEPROM serves | FX2 enumerates as |
|---|---|---|
| `c2` (default) | the full image | `35F0:DB13` running our firmware — **normal** |
| `c0` | byte 0 = `0xC0`, DISCON cleared | `35F0:DB13` boot ROM, waiting for `fx2tool` — **FX2 development** |
| `rom` | nothing (I²C silent) | `04B4:8613` — **bring-up fallback** |

C0 has to clear DISCON (config byte bit 6). The image sets it for C2 because our firmware
reconnects in `usb_init()`, but in C0 there is no firmware to reconnect and the FX2 never
appears on the bus. Found on hardware.

## Layout

```
src/      main.c, descriptors
tools/    C2 image → C array for the RP2350 build
```

## Status

**Works on the rev-A prototype (2026-09-29).** It builds warning-free with SDCC 4.2.0,
RAM-loads with `fx2tool`, re-enumerates as `35F0:DB13` at High Speed, and carried 1.1 GB of
the RP2350's counter at 37.5 MB/s (the RP2350's clkdiv-1 rate) with zero gaps. The CBM9002A clone behaves like the Cypress
part on the sync external-IFCLK path.

Implemented: sync slave FIFO init, EP6 bulk IN 512 quad-buffered, AUTOIN, vendor-class
descriptors, MS OS 1.0 `WINUSB`, and `tools/mkc2.py` (tested against libfx2's own
`boot-cypress.ihex`).

USB identity is **`35F0:DB13`**.

**Self-boot works (2026-09-29):** 20/20 reboots enumerated with the RP2350's serial, and the
self-booted FX2 streams 37.5 MB/s with zero gaps. A RAM-loaded `.ihex` (C0/rom modes) keeps the
unpatched `BSLYSERIAL000000`, because only the image the RP2350 serves is patched.

See [../docs/bringup-plan.md](../docs/bringup-plan.md) stage 1.
