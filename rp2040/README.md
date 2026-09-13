# RP2040 firmware — three-port CMSIS-DAP probe

Three independent SWD debug ports on 6-pin JST-SH connectors, one RP2040, behind the on-board
hub. Per-port CONNECTED/RUNNING LEDs plus a USB LED.

| Port | Signals | LEDs |
|---|---|---|
| SWD1 | CLK, IO, NRST, **SWO** | `LED1_CONNECTED`, `LED1_RUNNING` |
| SWD2 | CLK, IO, NRST, **SWO** | `LED2_CONNECTED`, `LED2_RUNNING` |
| SWD3 | CLK, IO, NRST (no SWO) | `LED3_CONNECTED`, `LED3_RUNNING` |

USB identity: **`35F0:DB11`**. Serial is this RP2040's own flash unique ID — it does not share
the RP2350's, since it is a debug probe rather than half of the capture path.

## Open question: how do three DAPs appear to the host?

The LEDs imply three *simultaneously usable* ports, which does not map cleanly onto CMSIS-DAP.
A CMSIS-DAP probe is conventionally one debug port per USB device. Candidates:

1. **Composite device, three CMSIS-DAP v2 bulk interface pairs**, each with its own interface
   string. probe-rs and pyOCD discover CMSIS-DAP v2 by interface, so this *may* work — but
   whether they cope with three on one device, and how the user selects between them, needs
   checking before committing. ⚠️ Unverified.
2. **Three separate USB devices**, if the RP2040's USB can be made to look like that (it
   cannot, natively — this would mean three MCUs, which the board does not have).
3. **One active port at a time**, selected by a vendor request or over the RP2350 control
   channel. Least elegant, certainly works, and the per-port LEDs still make sense.

Prior art to check first: `YAPicoprobe`, `free-dap`, and Raspberry Pi's `debugprobe`.

Resolve this before writing much code — it determines the USB descriptor shape and therefore
almost everything else.

## Toolchain

Pico SDK + CMake + `arm-none-eabi-gcc`.

## Status

Not started, and lowest priority: testable against an existing CMSIS-DAP build with probe-rs.
Bring-up only needs "the hub sees it" — see [../docs/bringup-plan.md](../docs/bringup-plan.md)
stage 0b.
