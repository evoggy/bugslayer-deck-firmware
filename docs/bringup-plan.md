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
| 1.3 → 25 MB/s | 2026-09-29 | ✅ **4-cycle engine + FX2 programmable flag.** 25.05 MB/s at 25 MHz. At clkdiv 1 (IFCLK 37.5 MHz): **60 s, 2.25 GB, 37.52 MB/s, zero gaps**, stall test clean. The engine never waits on USB, so the RP2350's clock is now the limit |
| 2 · block stream over RP2350 USB | 2026-09-29 | ✅ `bsly.py capture`: SESSION first, session matches `arm`, zero seq gaps, every synthetic sample = its index, END totals match, `.sr` written. Clean up to 350 ksps raw16 (the Full-Speed sink carries 768 kB/s); above that, loss arrives as inline OVERRUN blocks that the verifier accounts for exactly |
| 4 · SPI, SCK-clocked | 2026-09-29 | ✅ `arm ... spi` / `bsly.py capture --spi`: stream 1 = GP16–23 at every rising SCK edge, with CS-change markers. Flow deck v2 (PMW3901, CS = IO_3, 1.31 MHz mode 3), CF rebooted via an nRF51 reset: **product ID `0x00` → `0x49`, inverse `0x5F` → `0xB6`, and all 77 register writes (power-up reset + `InitRegisters`) identical to `pmw3901.c`**. Same result over the USB sink (300 ksps raw16, SPI still exact, transactions timed from raw16 CS windows) and the FX2 (16.67 Msps: 1998 transactions in 20 s, every one byte-identical to an independent decode from raw16, zero loss). sigrok's own SPI decoder agrees on the raw16 `.sr`. **Not yet tested at 21 MHz**: no uSD or LPS deck on hand |
| 5 · FX2 block sink | 2026-09-29 | ✅ `arm <rate> pins fx2`, `bsly.py capture --sink fx2`. **16.67 Msps raw16, 10 s, 167 M samples, 35.0 MB/s, zero loss**; 18.75 Msps overruns and accounts for it exactly; 3 ksps delivers every block plus END via idle ZLPs. Stage 1 still 37.5 MB/s on the reworked engine. **Real ground truth:** an nRF51 reset (`probe-rs reset --probe 35f0:db11-1`) re-enumerates a Flow deck v2: Search ROM and Match ROM agree on `0D D0 7B 8E 00 00 00 2D` (DS28E05, Dallas CRC OK), and Read Memory returns `bcFlow2` rev `A`, VID `BC` PID `0F`, with both CRC-32 bytes in the deck header matching. That meets stage 3's pass criterion through the FX2 |
| 5 · FX2 self-boot | 2026-09-29 | ✅ RP2350 emulates the EEPROM at 0xA2 and serves the 3388-byte C2 image with its serial patched in. 20/20 reboots enumerate as `35F0:DB13` with the RP2350's serial, 0.18–0.22 s after reset release. Self-booted FX2 streams 37.55 MB/s with zero gaps. C0 and rom modes work |

Two RP2350 bugs found and fixed on the way to 1.2, both now in `rp2350/README.md`:
FLAGB's pad was still isolated (ISO) so the engine never left `stall`, and the PIO input
synchronizer made FLAGB late enough to drop one byte per EP6-full event at clkdiv 1. The second
is invisible in a plain streaming run where the host keeps up. `fx2_counter_test.py --stall`
catches it.

Stage 1.3 replaced the flag-timing problem instead of tuning it. FLAGB is now the FX2's EP6
*programmable-level* flag, asserting 16 bytes before full (active high, inverted at the RP2350
pad). A late sample now writes into slack instead of into a full FIFO, and the engine dropped
from 8 to 4 `clk_sys` cycles per byte with SLWR# held asserted while streaming.

An apparent ~32 MB/s ceiling turned out to be the **test tool**. The per-word Python checker
resubmitted USB transfers late, and the FX2 flow-controlled the RP2350. That was diagnosed with
`prof` (23% in `stall`, 0% starved) and `fx2_counter_test.py --no-check` (37.6 MB/s), and it
didn't change between a dock and a direct root port. The checker now has a numpy fast path
(~3 GB/s) and measures the link, not itself. Beyond 37.5 MB/s needs a faster `clk_sys`: the
FX2 takes IFCLK up to 48 MHz, which is 192 MHz at 4 cycles/byte.

## Stage 0 — signs of life, no firmware to speak of

**0a. RP2350 blink + CDC.** Stock Pico SDK hello-world. Proves the +3V3 rail, XOSC, QSPI
flash, USB routing through the CH334 and BOOTSEL recovery.
→ *Pass: a CDC port enumerates and prints; BOOTSEL re-flashes over USB.*

**0b. RP2040 probe.** Flash the probe firmware (repository `bugslayer-probe-firmware`, formerly
`rp2040/` here) over the RP2040's BOOTSEL (SW2). It enumerates as `35F0:DB11` with four
CMSIS-DAP interfaces, one per SWD port.
→ *Pass: `probe-rs list` shows four probes, and `probe-rs info --probe 35f0:db11-0` sees the
RP2350. From then on the RP2350 can be flashed over SWD0 instead of BOOTSEL.* (Until
2026-09-30 the RP2350 was the last port, SWD4 = `db11-3`; the results table below uses the old
numbering.)

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
link is proven; do not optimise before stage 1.3 passes. *(Superseded by stage 1.3: 4 cycles
per byte, FLAGB = programmable flag. See Results and `rp2350/pio/fx2_write.pio`.)*

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

> **Done differently (2026-09-29).** Not time-boxed: SPI is captured *clocked by SCK*, one byte
> per rising edge, as its own stream (`ENC_SCK8`, docs/protocol.md). That is exact at 21 MHz,
> needs no RAM window, and at the Crazyflie's SPI duty cycle fits through either sink. The plan
> as first written is kept below.

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

- ✅ *(done 2026-09-29, ahead of the rest of stage 5)* Move FX2 boot from `fx2tool` RAM-loading
  to the **RP2350 emulating the EEPROM at 0xA2**, with the C2 image bundled in the RP2350's
  UF2. `fx2 boot c2|c0|rom` switches mode without reprogramming.
- ✅ *(2026-09-29)* The FX2 block sink: a 64-slot ring of 512-byte blocks, fed to the write
  engine by DMA. Loss when the host stops reading shows up as inline `OVERRUN` blocks, because
  the sampler ring overflows behind a full sink. That is the same path as the USB sink.
- ✅ *(2026-09-29)* `PKTEND`: a flush at sink start, and a zero-length packet whenever the
  sink goes quiet, so the host's transfers complete (see docs/protocol.md). Blocks are whole
  packets, so a disarm never strands a tail.
- **Found on the way:** the write engine must never freeze IFCLK. It used to stall on an
  empty TX FIFO with the clock stopped. The FX2 commits packets on IFCLK, and the block sink
  runs dry at every packet boundary, so it wedged after a few dozen packets with EP6 full. It
  now checks for data and room once per word and waits with the clock running
  (`rp2350/README.md`).
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
