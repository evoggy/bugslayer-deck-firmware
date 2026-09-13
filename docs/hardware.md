# Hardware reference (firmware view)

Extracted from Obsidian `Projects/Bugslayer/Deck/Expansion port and RP2350 pinout.md`
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
reachable. With base 16, PIO pin indices are `GPn - 16`:

- capture group GP16–23 → PIO pins **0–7**
- FX2 bus GP32–43 → PIO pins **16–27**

## CF expansion signals — GP16–31

| GP | PIO idx (base 16) | CF pin | Signal | HW peripheral |
|---|---|---|---|---|
| GP16 | 0 | P2.4 | MISO | `spi0_rx` |
| GP17 | 1 | P1.7 | IO_1 | (`spi0_ss_n`) |
| GP18 | 2 | P2.5 | SCK | `spi0_sclk` |
| GP19 | 3 | P2.3 | MOSI | `spi0_tx` |
| GP20 | 4 | P1.8 | IO_2 | — |
| GP21 | 5 | P1.9 | IO_3 | — |
| GP22 | 6 | P1.10 | IO_4 | — |
| GP23 | 7 | P2.10 | OW | — (1-Wire deck ID) |
| GP24 | 8 | P1.5 | SDA | `i2c0_sda` |
| GP25 | 9 | P1.6 | SCL | `i2c0_scl` |
| GP26 | 10 | mux 2D+ | EXT_TX2 | `uart1_tx` (AUX) |
| GP27 | 11 | mux 2D− | EXT_RX2 | `uart1_rx` (AUX) |
| GP28 | 12 | P1.4 | TX1 | `uart0_tx` |
| GP29 | 13 | P1.3 | RX1 | `uart0_rx` |
| GP30 | 14 | P2.8 | N_IO_1 | — |
| GP31 | 15 | P2.9 | N_IO_2 | — |

**GP16–23 is the money group.** One `in pins, 8` from PIO pin 0 captures MISO, SCK, MOSI,
*all four* CS candidates and OW as one byte per sample. CS is not fixed to IO_1 — it varies
by deck — so capturing all four and masking in software is the only option that works
across decks, and transaction framing needs CS anyway.

Peripheral *inputs* are non-exclusive on RP2040/RP2350: the pad input reaches every consumer
regardless of `fsel`. So a hardware UART can drive TX while a PIO SM simultaneously sniffs
the same pin.

## FX2 slave FIFO — GP32–43

| GP | PIO idx | FX2 signal | Direction | Notes |
|---|---|---|---|---|
| GP32–39 | 16–23 | FD0–FD7 | RP2350 → FX2 | contiguous, one `out pins, 8` |
| GP40 | 24 | IFCLK | RP2350 → FX2 | **RP2350 is clock master**, side-set |
| GP41 | 25 | SLWR | RP2350 → FX2 | write strobe, active low |
| GP42 | 26 | PKTEND | RP2350 → FX2 | commit a short packet, active low |
| GP43 | 27 | FLAGB | FX2 → RP2350 | EP6 full flag, **active low** (`jmp pin`) |

Strapped in hardware, not firmware: FIFOADR = EP6 (`FIFOADR1=1, FIFOADR0=0`), SLRD and SLOE
tied inactive. The link is IN-only.

⚠️ **FLAGB low means full.** `jmp pin` branches when the pin is *high*, i.e. when there is
room. Easy to get backwards.

⚠️ **IFCLK must be free-running before `FX_RESET#` is released and must never stop while the
FX2 is out of reset.** With `IFCLKSRC=0` the FX2's FIFO register block is clocked from that
pin; if it stops, the 8051 hangs on the next register write. The PIO program idles the clock
and deasserts SLWR — it does not gate the clock.

## Control / housekeeping — GP0–15

| GP | Signal | Notes |
|---|---|---|
| GP0 | `EXT_VCOM_EN` | SiP32431 high-side switch |
| GP1 | `EXT_VCC_EN` | SiP32431 high-side switch |
| GP2 | `EXT_I2C_PULL_EN` | drives 2× 2.2 kΩ pull-ups: **high = on** (standalone), low/Hi-Z = off (on a CF) |
| GP3 | `USB_UART_MUX_SEL` | TS3USB221A S |
| GP4 | `USB_UART_MUX_nEN` | TS3USB221A OE |
| GP5 | `FX_RESET#` | 10 kΩ pull-down: FX2 stays in reset until RP2350 firmware releases it |
| GP6 | `FX_SDA` | `i2c1_sda` — RP2350 emulates the FX2 boot EEPROM at 0xA2 |
| GP7 | `FX_SCL` | `i2c1_scl` — 2.2 kΩ pull-ups on the FX2 sheet |
| GP8–15 | spare | route a few to test points |

The FX2 PIO block runs at base 16, which makes GP0–7 invisible to it. None of them need PIO.

## ADC — power telemetry

| ADC | Pin | Tap | Purpose |
|---|---|---|---|
| ADC4 | GP44 | VCOM ÷2 | who is supplying VCOM, and at what level |
| ADC5 | GP45 | VUSB ÷2 | USB 5 V presence |
| ADC6 | GP46 | VCC (3.0 V) | **Crazyflie presence detect** |
| ADC7 | GP47 | aux test point | one flexible analog channel |

## RP2040 — three debug ports

Three 6-pin JST-SH (SM06B-SRSS-TB) connectors, one RP2040, its own QSPI flash and 12 MHz
crystal:

| Port | Signals | LEDs |
|---|---|---|
| SWD1 | CLK, IO, NRST, **SWO** | `LED1_CONNECTED`, `LED1_RUNNING` |
| SWD2 | CLK, IO, NRST, **SWO** | `LED2_CONNECTED`, `LED2_RUNNING` |
| SWD3 | CLK, IO, NRST (no SWO) | `LED3_CONNECTED`, `LED3_RUNNING` |

Plus `LED_USB`. The RP2040's own SWD (`SWD_CLK`/`SWD_IO`/`T_NRST`) and USB leave the sheet
as hierarchical labels.

## Board-level

- **CH334P** USB 2.0 HS hub, on the top sheet. Everything on the board is behind it, plus
  a downstream port to the deck's EXP USB connector.
- **TS3USB221A** mux — routes the RP2350's Full-Speed USB *or* UART2 to the EXP connector.
  See Obsidian `Deck/USB-UART switch.md`.
- **TPS62A01** ×2 buck, **SiP32431** ×2 high-side switches for VCC/VCOM.
- ⚠️ The FX2 runs off **+3V0** with no separate 3.3 V LDO. That is exactly the FX2's 3.0 V
  minimum, accepted knowingly. If the part misbehaves at temperature or on load steps,
  suspect this first.

## Not yet in the schematic

As of 2026-09-13 the KiCad project still contains `ft232h.kicad_sch` (superseded by
`cbm9002a.kicad_sch`) and orphaned `dbg-1/dbg-2/stm32-dbg/nrf51-dbg` sheets that are not
instantiated from any parent. Several symbols are unannotated. Do not infer pin numbers from
those files.
