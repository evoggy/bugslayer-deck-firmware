# Bring-up plan — test firmware

Goal: **stack decks on a Crazyflie, plug in the Bugslayer deck, power the Crazyflie, and
capture the whole startup sequence on the PC.**

There are two independent unknowns in that sentence:

1. **Does the sniffing work?** PIO capture, DMA, framing, decoding.
2. **Does the FX2 pipe work?** Slave FIFO with external IFCLK on a clone nobody has run that
   way, plus a firmware we have to write. See Obsidian `Deck/FX2 firmware.md`.

The plan below tests them **separately and in that order**, then combines them. If the FX2
turns out to be a problem, stages 0–4 still deliver a working (slower) instrument, and the
host tooling written along the way needs no changes.

Each stage has an explicit pass criterion. Do not move on without it.

---

## Results — rev-A prototype

| Stage | Date | Result |
|---|---|---|
| 0b RP2040 probe | 2026-09-29 | ✅ `35F0:DB11`, four CMSIS-DAP interfaces. SWD4 sees the RP2350's two M33s and flashes it |
| 0a RP2350 | 2026-09-29 | ✅ Flashed over SWD4, `35F0:DB12` CDC answers `ping`/`ver`/`id`/`stat`. Blank flash booted to `2e8a:000f` first, as expected |
| 0c FX2, no firmware | 2026-09-29 | ✅ `fx2 up` → `04B4:8613` at **480 Mbit/s** on CH334 port 2 |
| 1.1 6 MHz | 2026-09-29 | ✅ 6.00 MB/s, 61 MB, zero gaps. **The clone's sync external-IFCLK path works** |
| 1.2 18.75 MHz | 2026-09-29 | ✅ **60 s, 1.13 GB, 18.76 MB/s, zero gaps**. Stall test (10 fill/stall rounds) clean |
| 1.3 → 25 MB/s | — | not started |

Two RP2350 bugs found and fixed on the way to 1.2, both now in `rp2350/README.md`:
FLAGB's pad was still isolated (ISO) so the engine never left `stall`, and the PIO input
synchronizer made FLAGB late enough to drop one byte per EP6-full event at clkdiv 1. The second
is invisible in a plain streaming run where the host keeps up. `fx2_counter_test.py --stall`
catches it.

## Stage 0 — signs of life, no firmware to speak of

**0a. RP2350 blink + CDC.** Stock Pico SDK hello-world. Proves the +3V3 rail, XOSC, QSPI
flash, USB routing through the CH334 and BOOTSEL recovery.
→ *Pass: a CDC port enumerates and prints; BOOTSEL re-flashes over USB.*

**0b. RP2040 probe.** Flash `rp2040/` over the RP2040's BOOTSEL (SW2). It enumerates as
`35F0:DB11` with four CMSIS-DAP interfaces, one per SWD port. See
[../rp2040/README.md](../rp2040/README.md).
→ *Pass: `probe-rs list` shows four probes, and `probe-rs info --probe 35f0:db11-3` sees the
RP2350. From then on the RP2350 can be flashed over SWD4 instead of BOOTSEL.*

**0c. FX2 with no firmware at all.** Leave `FX_RESET#` released and the RP2350 silent on I²C.
The FX2's boot ROM finds no EEPROM at 0xA2 and enumerates as **`04B4:8613`**.
→ *Pass: `lsusb` shows `04B4:8613` behind the hub.*

This is a genuinely valuable milestone: it proves the 24 MHz crystal, the +3V0 rail (⚠️ exactly
the FX2's 3.0 V minimum), the USB routing and the hub — with **zero FX2 firmware written**. Do
it before anyone writes 8051 code.

---

## Stage 1 — the pipe, with synthetic data

No Crazyflie, no sniffing. Just: can the RP2350 push bytes through the FX2 to the PC without
losing any?

**RP2350 side.** ✅ Implemented: `rp2350/pio/fx2_write.pio` + `rp2350/src/fx2_link.c`.
A PIO write engine on GP32–43 plus two DMA channels ping-ponging 8 KiB buffers filled with a
**32-bit little-endian incrementing counter**. A 32-bit counter rather than a byte pattern is
the point — it makes loss *quantifiable*, not merely detectable.

The engine is eight `clk_sys` cycles per IFCLK period (4 low, 4 high), so **18.75 MB/s at the
stock 150 MHz**, with these margins against the CBM9002A datasheet p.17 numbers:

| | Required | At 150 MHz |
|---|---|---|
| tSFD — FD setup | ≥ 3.2 ns | 26.7 ns |
| tSWR — SLWR# setup | ≥ 12.1 ns | 20.0 ns |
| tFDH / tWRH — hold | ≥ 4.5 / 3.6 ns | 26.7 ns |
| tXFLG — FLAGB valid | ≤ 13.5 ns | sampled at 20.0 ns |
| tIFCLK — period | 20.83–200 ns | 53.3 ns |

Three entry points share one program: `clk_only` (a clock that cannot stall, used from `fx2 up`
until the first `arm`), `stall` (EP6 full — keep clocking, SLWR# deasserted) and `write`.
Switching between them is a `pio_sm_exec` of a `jmp`. A stalled `out` freezes the clock, which
is safe *after* the FX2 has configured itself — see docs/hardware.md.

⚠️ 18.75 MB/s is a deliberately conservative first cut. A tighter loop is possible once the
link is proven; do not optimise before stage 1.3 passes.

**FX2 side.** ✅ Implemented: `fx2/src/main.c`. ~30 lines of register init plus a descriptor
set with the MS OS 1.0 `WINUSB` compat ID, built with SDCC against libfx2 (submodule). Needs
`sdcc` installed, which is the real cost of this firmware — the code is trivial.

⚠️ It is **sync-only**. The async fallback mentioned below is a one-line `IFCONFIG` change
(`|_ASYNC`) plus a different PIO program, not something the current build can switch to at
runtime.

**Host side.** ✅ Implemented: `host/fx2_counter_test.py`. Queued async libusb transfers (16 ×
64 KiB in flight — a synchronous read loop tops out below the link rate and would report false
failures), verifying the counter and reporting exactly how many bytes went missing where.

**Walk the ladder — this is the FX2 risk test:**

| Step | Mode | Expected |
|---|---|---|
| 1.1 | RAM-load with `fx2tool`, `clk 6000000` | ~6 MB/s, no gaps — **go/no-go for the clone's external-IFCLK path** |
| 1.2 | `clk 18750000` (clkdiv 1, the engine's native rate) | ~18.7 MB/s, no gaps |
| 1.3 | tighten the PIO loop and/or raise `clk_sys` | toward 25 MB/s |

→ *Pass: 60 s with **zero sequence gaps** at 18.75 MB/s. Throughput is reported, not asserted —
correctness first, then optimise.*

RAM-loading with `fx2tool` (or `fx2pipe`) means the RP2350 does not need EEPROM emulation yet
and the edit-test loop is seconds, not a reflash. Keep the RP2350 silent on I²C so the FX2
falls back to `04B4:8613`.

**If 1.2 fails**, async mode on the same pins gives 8 MB/s with no board change — still above
the ~6 MB/s worst case. Note it, carry on, revisit later.

---

## Stage 2 — control plane and stream framing, over the RP2350's USB only

Still no Crazyflie. Build the *format*, not the speed.

- CDC command loop: `ping`, `ver`, `id`, `stat`, `arm`, `disarm` (see
  [protocol.md](protocol.md)).
- Emit the 512-byte block format over the RP2350's own USB, filled with the same synthetic
  counter.
- `host/bsly.py`: `arm`, `disarm`, `capture` — parses blocks, checks `seq`, writes a file.

→ *Pass: `arm` then `disarm` produces a file whose session ID matches the `arm` reply, with no
sequence gaps and a `SESSION` block first.*

Now the host tool exists and the format is settled, with the FX2 entirely out of the picture.

---

## Stage 3 — first real capture: 1-Wire deck enumeration, over the RP2350 USB

The best possible first real target. It happens on **every** Crazyflie boot, it is slow enough
that the FX2 is irrelevant, and the answer is independently checkable.

- Sample the GP16–23 group free-running at ~2 Msps (2 MB/s — comfortable over the RP2350's USB
  in bursts, and trivially so if capture is time-boxed to the boot window).
- Stack a deck on a Crazyflie, arm the capture, power the Crazyflie.
- The STM32 reads each deck's DS28E05 deck-ID memory over the OW line (GP23) at startup.

→ *Pass: the reset pulse, presence pulse and a 64-bit ROM ID are visible in the trace, and the
ID matches the deck's known ID (readable via the Crazyflie's own deck-info output).*

This proves the sniffing chain end to end — pins, PIO, DMA, framing, transport, decoding —
against ground truth, with zero dependence on stage 1.

**Also worth doing here:** I²C at 400 kHz (GP24/25) during the same boot, for any I²C deck in
the stack. Same sample group, same code path.

---

## Stage 4 — SPI at speed, still over the RP2350 USB where possible

- Same 8-bit group, higher rate. A Flow deck v2 (PMW3901 + VL53L1x) or an LPS deck (DWM1000)
  produces a burst of register initialisation at startup.
- At full rate this exceeds the RP2350's USB, so **time-box it**: capture into RAM for a fixed
  window around boot, then drain. ~400 KB of RAM at 24 Msps is ~16 ms — enough for a burst,
  not enough for a boot.
- Decode: a PMW3901 product-ID read (register 0x00 → 0x49) is an easy first ground truth.

→ *Pass: decoded SPI transactions match the expected register traffic for the deck, with the
correct CS line identified out of IO_1–IO_4.*

The RAM limit here is exactly what motivates stage 5.

---

## Stage 5 — combine: the real thing

Point the stage 3/4 capture at the FX2 sink instead (`arm ... fx2`) and capture a **full
Crazyflie startup** — every deck, every bus, no time-boxing.

- Move FX2 boot from `fx2tool` RAM-loading to the **RP2350 emulating the EEPROM at 0xA2**,
  with the C2 image bundled in the RP2350's UF2. Start in **C0 mode** (our VID/PID, host
  RAM-load) and flip to **C2** (self-boot) only once the image is stable — one byte, no
  reprogramming, see `Deck/FX2 firmware.md`.
- Add inline `OVERRUN` blocks when FLAGB stalls the write SM.
- Assert `PKTEND` on disarm so the tail of the capture is not stranded in the FIFO.
- Verify the shared `iSerialNumber` pairing with **two decks plugged into one PC**. This is the
  only way to find out that it is wrong.
- Windows check on a clean machine: WinUSB binds to the FX2 with no Zadig.

→ *Pass: a complete Crazyflie boot captured with zero overrun blocks, decoding to the same
1-Wire and SPI ground truth as stages 3 and 4.*

---

## Sequencing notes

- **Stage 1 and stages 2–4 are independent.** If two people are working, split there.
- **Do stage 0c before writing any 8051 code.** It is free and it de-risks the rail and crystal.
- **Do not skip stage 2.** Settling the block format before there is data worth keeping is
  much cheaper than migrating capture files later.
- Everything through stage 4 works if the FX2 never works at all. That is the point of the
  ordering.

## What is deliberately not in this plan

- Active driving (standalone mode: SPI/I²C/UART master, DeckCtrl) — needs the sniffing side
  proven first.
- The USB/UART mux (`USB_UART_MUX_SEL/nEN`) and power switching, beyond `pwr`/`pull` commands.
- Compression. v0 is raw samples; RLE comes after the pipe is proven.
- The RP2040's SWD1–3 against real targets, past "probe-rs sees it".
