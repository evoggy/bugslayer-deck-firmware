# Bugslayer deck — firmware

Firmware for the **Bugslayer deck**, a Crazyflie 2.x expansion deck that works either
standalone (drive decks with no Crazyflie present) or as a shim in the stack (passively
sniff every bus while the Crazyflie runs).

This repository holds **all three firmwares that ship on the board**, plus minimal host-side
tools used to bring the hardware up. The real capture/display application lives in a separate
repository — anything in `host/` here is a test harness, not a product.

Hardware: `crazyflie-exp-electronics/bugslayer` (KiCad).
Design notes: Obsidian `Projects/Bugslayer/Deck/`.

## The board, in one picture

```
                 USB-C
                   │
              ┌────┴─────┐
              │  CH334P  │  USB 2.0 HS hub
              └─┬───┬──┬─┘
       FS       │   │  │        HS
  ┌─────────────┘   │  └──────────────────┐
  │                 │                     │
┌─┴────────┐   ┌────┴─────┐        ┌──────┴──────┐
│ RP2040   │   │ RP2350B  │◄──────►│  CBM9002A   │
│ 3× DAP   │   │ sniff +  │ 12-pin │  (FX2LP)    │
│ CMSIS-DAP│   │ control  │ slave  │  bulk IN    │
└─┬─┬─┬────┘   └────┬─────┘  FIFO  └─────────────┘
  │ │ │             │
 3× SWD          CF expansion port (16 signals)
 (JST-SH 6p)
```

- **RP2040** — three independent SWD debug ports on 6-pin JST-SH connectors, each with
  its own CONNECTED/RUNNING LED. Enumerates on the hub as a CMSIS-DAP probe.
- **RP2350B** — the actual instrument. PIO sniffs the 16 expansion-port signals, and the
  same pins can drive them actively when the deck is used standalone. Owns the **control
  plane** on its own Full-Speed USB, and boots the FX2 over an emulated I²C EEPROM.
- **CBM9002A (FX2LP clone)** — dumb one-way High-Speed pipe. The RP2350 clocks bytes into
  its slave FIFO; they come out of a bulk IN endpoint on the PC at up to ~25 MB/s. The 8051
  runs ~30 lines of setup at boot and is then out of the data path entirely.

## Layout

| Directory | What | Toolchain |
|---|---|---|
| `rp2350/` | Sniffing, control plane, FX2 boot + clocking | Pico SDK / CMake / arm-none-eabi |
| `rp2040/` | 3-port CMSIS-DAP probe | Pico SDK / CMake / arm-none-eabi |
| `fx2/` | 8051 slave-FIFO firmware for the CBM9002A | SDCC + libfx2 |
| `host/` | Minimal bring-up + verification tools | Python / libusb |
| `docs/` | Protocol, pin map, bring-up plan | — |

Start with **[docs/bringup-plan.md](docs/bringup-plan.md)**. It is ordered so that each stage
proves one thing and the two big unknowns — "does the sniffing work" and "does the FX2 pipe
work" — are tested independently before being combined.

## The two-channel design

**Commands go over the RP2350's USB. Data comes out of the FX2.** The RP2350 and the FX2
enumerate as two separate USB devices behind the on-board hub.

This is deliberate: the FX2 is a one-way pipe *on purpose*. Giving it an OUT endpoint would
cost pins that are currently strapped (SLRD/SLOE/FIFOADR), put the 8051 back in the data path,
and buy nothing — the RP2350 has to expose a USB interface anyway for CMSIS-DAP, UF2 recovery
and control.

The cost is that the host now talks to two devices and must not assume anything about ordering
between them. Two rules make that a non-issue, and **both must be honoured from the first
firmware commit because they are baked into descriptors and framing**:

1. **Both devices report the same `iSerialNumber`**, derived from the RP2350's flash unique ID.
   The RP2350 patches it into the FX2's descriptors when it serves the boot image. This is how
   the host pairs them when two decks are plugged into one PC.
2. **The data stream is self-describing.** Session ID, block sequence numbers, timestamps and
   inline overrun records live *in* the stream, so the host never needs to correlate the two
   channels in time. See [docs/protocol.md](docs/protocol.md).

One stream format, two transports: the same framed blocks can be sent over the RP2350's own
USB at ~1 MB/s. That is not a fallback bolted on later — it is how the host tooling and the
decoders get developed before the FX2 works at all, and it is enough for 1-Wire, I²C and UART
captures on its own.

## Status

The board has not been fabricated, so **none of this has run on hardware**. Everything below
compiles and is ready for stage 0/1 of the bring-up plan.

| Firmware | Status |
|---|---|
| `rp2350/` | stage 0 + 1 — CDC command loop, FX2 link, PIO write engine, counter DMA. **Builds clean** |
| `rp2040/` | not started; USB topology question open |
| `fx2/` | stage 1 — slave FIFO + WinUSB descriptors. **Not compiled** (no SDCC installed here) |
| `host/` | `fx2_counter_test.py` done; needs `pip install libusb1` |
