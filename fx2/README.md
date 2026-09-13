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

Sync slave FIFO, **IN-only**, `AUTOIN`, **external IFCLK** driven by the RP2350 at 25 MHz.
EP6 bulk IN, 512 bytes, quad-buffered. FIFOADR is strapped to EP6 in hardware; SLRD and SLOE
are tied inactive. `FLAGB` = EP6 full flag, active low.

Gotchas that will cost an afternoon each:
`SYNCDELAY` between FIFO/endpoint register writes · `WORDWIDE` defaults **on** and must be
cleared · `PINFLAGSAB = 0xE0` for FLAGB = EP6 FF · bump `bcdDevice` on every descriptor change
during development, because Windows caches the MS-OS-descriptor answer per VID/PID/bcdDevice.

## Toolchain

SDCC (`sdcc`, `sdas8051`, `sdobjcopy`) + libfx2. Output is an Intel HEX plus a C2 EEPROM image;
`tools/` converts the C2 image to a C array for the RP2350 build, which is committed so the
RP2350 firmware builds without SDCC.

## Boot

The RP2350 emulates the EEPROM at 0xA2 on GP6/GP7. Byte 0 of the image selects the mode:

| Byte 0 | Behaviour |
|---|---|
| (silent I²C) | `04B4:8613`, ROM descriptors, host RAM-load — **bring-up mode** |
| `0xC0` | our VID/PID, still host RAM-loads — **development mode** |
| `0xC2` | full self-boot — **production** |

One image, one byte. No reprogramming to switch.

## Layout

```
src/      main.c, descriptors
tools/    C2 image → C array for the RP2350 build
```

## Status

Not started. See [../docs/bringup-plan.md](../docs/bringup-plan.md) stage 1.
