# Hardware reference (firmware view)

Regenerated 2026-09-20 from the rev-A schematic netlist (pre-order state)
and the KiCad project `crazyflie-exp-electronics/bugslayer`. **The schematic is the
authority** — this file exists so firmware work does not need KiCad open, and it will
drift. Re-check before trusting anything here.

## RP2350B — the organizing constraint

Each PIO block sees a **32-pin window**, either GP0–31 or GP16–47, selected with
`pio_set_gpio_base()`. The pin map is built around that:

| Region | Use | PIO base |
|---|---|---|
| GP16–31 | all 16 CF expansion signals | reachable from **either** base |
| GP32–43 | FX2 slave FIFO | requires base 16 |
| GP44–47 | ADC (power telemetry) | — |
| GP0–15 | control / housekeeping | never sniffed |

Put the capture SM and the FX2 write SM on the **same block at base 16** and both are
reachable.

**You do not do the base arithmetic yourself.** On RP2350B the SDK defaults
`PICO_PIO_USE_GPIO_BASE` to 1, and then every `sm_config_set_*_pins()` and
`pio_sm_set_consecutive_pindirs()` call takes a **real GP number in 0–47** and translates
internally. `pio_sm_init()` returns `PICO_ERROR_BAD_ALIGNMENT` if the resulting configuration
cannot fit one 32-pin window. So the base-16 constraint is real, but it is a validity check,
not pin maths in your code.

## CF expansion signals — GP16–31

As built, rev A (from the schematic netlist, 2026-09-20). **This differs from the
original planning note** — every CF signal moved.

| GP | CF pin | Signal | HW peripheral | Notes |
|---|---|---|---|---|
| GP16 | P3.6 | IO_1 | `spi0_rx`* | also RP2040 GP10 (SWD on EXP) |
| GP17 | P3.7 | IO_2 | `spi0_ss_n`* | also RP2040 GP11 (SWD on EXP) |
| GP18 | P3.8 | IO_3 / BOOT_SELECT | `spi0_sclk`* | SW6 via R29 + R34 1 k |
| GP19 | P3.9 | IO_4 | `spi0_tx`* | SW5 via R30 1 k; RP2040 GP12 via R3 0 R |
| GP20 | P2.4 | MISO | `spi0_rx` | |
| GP21 | P2.8 | OW | (`spi0_ss_n`) | 1-Wire deck ID, nRF51 domain |
| GP22 | P2.3 | SCK | `spi0_sclk` | |
| GP23 | P2.5 | MOSI | `spi0_tx` | |
| GP24 | P2.7 | WKUP | — | nRF51 domain, no series R |
| GP25 | P2.6 | N_IO_1 | — | nRF51 domain, no series R |
| GP26 | P2.1 (via mux) | TX2 | `uart1_tx` (F11 AUX) | deck drives, 33 R R40 |
| GP27 | P2.2 (via mux) | RX2 | `uart1_rx` (F11 AUX) | deck listens, 33 R R39 |
| GP28 | P3.3 | TX1 | `uart0_tx` | deck drives |
| GP29 | P3.2 | RX1 | `uart0_rx` | deck listens |
| GP30 | P3.4 | SDA | `i2c1_sda` | switchable 2.2 k pull-up via GP2 |
| GP31 | P3.5 | SCL | `i2c1_scl` | " |

\* GP16–19 carry SPI0 functions in the pin table, but the bus itself is on
GP20–23. Treat IO_1–IO_4 as GPIO / chip-select candidates.

**Two capture bytes, both contiguous.** `in pins, 8` from **GP16** gives
IO_1..IO_4, MISO, OW, SCK, MOSI — the SPI bus plus every CS candidate. `in pins, 8`
from **GP24** gives WKUP, N_IO_1, TX2, RX2, TX1, RX1, SDA, SCL — every UART line
plus I²C. `in pins, 16` from GP16 captures all 16 CF signals in one state machine.

**Four different peripheral instances** (SPI0, UART0, UART1-AUX, I2C1), so all four
buses can be driven at once. I2C0 is reserved for the FX2 boot EEPROM on GP4/GP5.

**Both UARTs are wired straight through** — TX drives the Crazyflie's TX pin,
because the deck impersonates the Crazyflie for the decks above it. Sitting *on* a
Crazyflie the STM32 drives TX1 and TX2, so never select the UART function on
GP26/GP28 there: sniff only, and use a PIO UART TX on GP27/GP29 to talk to the CF.

Peripheral *inputs* are non-exclusive on RP2040/RP2350: the pad input reaches every
consumer regardless of `fsel` (RP2350 DS §1.2: *"the input is always connected, so
the PIOs can always see the state of all pins"*). So a hardware peripheral can drive
a bus while a PIO SM logs it.

## FX2 slave FIFO — GP32–43

| GP | FX2 signal | Direction | Notes |
|---|---|---|---|
| GP32–39 | FD0–FD7 | RP2350 → FX2 | contiguous, one `out pins, 8` |
| GP40 | IFCLK | RP2350 → FX2 | **RP2350 is clock master**, side-set |
| GP41 | SLWR | RP2350 → FX2 | write strobe, active low |
| GP42 | PKTEND | RP2350 → FX2 | commit a short packet, active low |
| GP43 | FLAGB | FX2 → RP2350 | EP6 **programmable-level** flag, high = nearly full; pad input inverted (`jmp pin` = room) |

Strapped in hardware, not firmware: FIFOADR = EP6 (`FIFOADR1=1, FIFOADR0=0`), SLRD and SLOE
tied inactive. The link is IN-only.

⚠️ **FLAGB is the EP6 programmable flag, not the full flag.** The FX2 firmware sets it to
assert 16 bytes before EP6 is full (`PF_SLACK`), and the PF pin drives **high** when asserted.
`fx2_write_program_init()` inverts GP43 at the pad (`GPIO_OVERRIDE_INVERT`), so `jmp pin`
branches when there is room. The slack is what lets the write engine run one byte per IFCLK
and react a few clocks late: sampling the real full flag in time is not possible at clkdiv 1
through the input synchronizer.

⚠️ **IFCLK must be running before the FX2's firmware configures its FIFO registers**, i.e.
before `FX_RESET#` is released. With `IFCLKSRC=0` that register block is clocked from GP40, and
the 8051 hangs on a register write if the clock is dead. `fx2_write.pio` handles this with a
`clk_only` loop that runs from `fx2 up` onward.

Once the FX2 is configured it does no further register writes, so a **stalled** write engine
freezing IFCLK is benign: with no rising edge nothing is latched, and SLWR# simply holds. The
rule is about boot ordering, not about keeping the clock alive forever.

## Control / housekeeping — GP0–15

| GP | Signal | Notes |
|---|---|---|
| GP0 | `EXT_VCOM_EN` | SiP32431 U8 high-side switch |
| GP1 | `EXT_VCC_EN` | SiP32431 U13 high-side switch |
| GP2 | `EXT_I2C_PULL_EN` | 2 × 2.2 kΩ pull-ups: **high = on** (standalone). Set 8 mA drive |
| GP3 | `USB_UART_MUX_nEN` | TS3USB221A OE, active low. Strap R13 pulls it low = enabled |
| GP4 | `FX_SDA` | `i2c0_sda` — RP2350 emulates the FX2 boot EEPROM at 0xA2 |
| GP5 | `FX_SCL` | `i2c0_scl` — 2.2 kΩ pull-ups on the FX2 sheet |
| GP6 | `FX_RESET#` | 10 kΩ pull-down holds the FX2 in reset until firmware releases it |
| GP7 | `USB_UART_MUX_SEL` | TS3USB221A S. Strap R32 pulls it **high = UART leg** |
| GP8–11, GP13 | `LED_2`–`LED_5`, `LED_1` | GPIO-sourced; 100 R on the high-V_f colours |
| **GP12** | `EXT_VCC` sense, through **R48 1 kΩ** | CF-presence detect. **Input only** — driving it low shorts the port VCC rail through U13. The rail floats when U13 is off, so enable the pad pull-down |
| GP14, GP15 | `TP3`, `TP2` | RH-5015 wire loops — scope trigger / spare. Reachable only by a PIO block at `gpio_base 0`, which therefore cannot also drive the FX2 |

⚠️ **The mux straps are the safe state, so don't fight them.** Reset leaves both pins
as inputs: S high (UART leg) and OE low (enabled), which passes the RP2350's Hi-Z
UART pins through to the Crazyflie. Selecting the USB leg drives USB signalling into
the CF's PA2/PA3, so firmware must only do that deliberately.

## GP44–47 — unconnected on rev A

The ADC power telemetry (VCOM / VUSB / VCC dividers) was dropped before fab.
Crazyflie presence is sensed digitally on GP12 instead. GP44–47 are the only
ADC-capable pins left *and* the only spare pins inside the base-16 PIO window, so
they are the natural home for anything PIO has to drive alongside the FX2.

## RP2040 — the on-board debug probe

One RP2040 with its own QSPI flash and 12 MHz crystal, driving **two external 6-pin
JST-SH ports** plus two internal SWD targets. As built:

| GP | Signal | Goes to |
|---|---|---|
| GP0 | `LED_USB` | |
| GP1 | test pad | **TP1** (1 mm pad) |
| GP2–GP5 | SWD1 CLK / IO / NRST / **SWO** | **P1** (JST-SH, hand-soldered — C160405 was out of stock) |
| GP6–GP9 | SWD2 CLK / IO / NRST / **SWO** | **P5** (JST-SH, hand-soldered) |
| GP10, GP11 | `EXT_SWD_CLK` / `EXT_SWD_IO` | the deck port's **IO_1 / IO_2** (P3.6/P3.7) — shared with RP2350 GP16/GP17 |
| GP12 | `EXT_SWD_nRST` | the deck port's **IO_4** via R3 0 R — shared with RP2350 GP19 |
| GP13–GP15 | CLK / IO / nRST | the **RP2350's own SWD** (U5 pins 33/34/35) |
| GP17–GP24 | `LEDn_CONNECTED` / `LEDn_RUNNING` ×4 | |

⚠️ **GP10/GP11/GP12 are shared with the Crazyflie bus.** Keep them Hi-Z unless the
SWD-on-EXP mode is selected, or the probe will fight the RP2350 and whatever is
driving IO_1/IO_2/IO_4 — they are chip-select candidates on a real stack.

## Board-level

- **CH334P** USB 2.0 HS hub, on the top sheet. Everything on the board is behind it, plus
  a downstream port to the deck's EXP USB connector.
- **TS3USB221A** mux — routes the RP2350's Full-Speed USB *or* UART2 to the EXP connector.
  See Obsidian `Deck/USB-UART switch.md`.
- **TPS62A01** ×2 buck, **SiP32431** ×2 high-side switches for VCC/VCOM.
- The FX2 runs off **+3V0**, retargeted to 3.141 V typ (R22 43.2 k / R25 10.2 k) so it
  clears the part's 3.00 V minimum with margin. A separate **TLV70233** 3.3 V LDO (U9)
  feeds only RP2350 VREG_VIN/VREG_AVDD/USB_OTP_VDD and RP2040 USB_VDD; IOVDD stays at
  3.0 V for the Crazyflie's sake.

## Not yet in the schematic

The KiCad project still contains `ft232h.kicad_sch` (superseded by `cbm9002a.kicad_sch`)
and orphaned `dbg-1/dbg-2/stm32-dbg/nrf51-dbg` sheets that are not instantiated from any
parent. Do not infer pin numbers from those files.

**P1 and P5 are not assembled.** The JST SM06B-SRSS-TB (C160405) went out of stock during
ordering, so both connectors are hand-soldered after the boards arrive. They are still
normal fitted parts in the schematic and BOM.
