// Bugslayer deck — RP2350B pin map.
// Authority is crazyflie-exp-electronics/bugslayer; see ../../docs/hardware.md.
// Regenerated 2026-09-20 from the rev-A schematic netlist (pre-order state).
#pragma once

// --- control / housekeeping (GP0-15, never sniffed) ---
#define PIN_EXT_VCOM_EN      0   // SiP32431 U8, high = deck sources VCOM
#define PIN_EXT_VCC_EN       1   // SiP32431 U13, high = deck sources port VCC
#define PIN_EXT_I2C_PULL_EN  2   // high = 2.2k pull-ups on (standalone only), 8mA drive
#define PIN_USB_UART_MUX_NEN 3   // TS3USB221A OE, active low. Strap R13 = low (enabled)
#define PIN_FX_SDA           4   // i2c0 — emulated FX2 boot EEPROM at 0xA2
#define PIN_FX_SCL           5   // i2c0
#define PIN_FX_RESET_N       6   // 10k pull-down holds the FX2 in reset
#define PIN_USB_UART_MUX_SEL 7   // TS3USB221A S. Strap R32 = high (UART leg)
#define PIN_LED_2            8
#define PIN_LED_3            9
#define PIN_LED_4           10
#define PIN_LED_5           11
#define PIN_EXT_VCC_SENSE   12   // port VCC through R48 1k. INPUT ONLY — driving it
                                 // low shorts the rail through U13. Enable the pad
                                 // pull-down: the rail floats when U13 is off.
#define PIN_LED_1           13
#define PIN_TRIG_2          14   // TP3 wire loop (scope trigger / spare)
#define PIN_TRIG_1          15   // TP2 wire loop (scope trigger / spare)
                                 // GP14/15 are reachable only by a PIO block at
                                 // gpio_base 0 — that block cannot also drive the FX2.

// --- CF expansion signals (GP16-31, reachable from either PIO window) ---
// Two contiguous capture bytes:
//   fast byte, in pins,8 from GP16 = IO_1..IO_4, MISO, OW, SCK, MOSI
//   slow byte, in pins,8 from GP24 = WKUP, N_IO_1, TX2, RX2, TX1, RX1, SDA, SCL
// in pins,16 from GP16 captures every CF signal in one state machine.
#define PIN_CAP_BASE        16
#define PIN_FAST_BASE       16   // SPI + all four chip-select candidates
#define PIN_IO_1            16   //   bit0   also RP2040 GP10 (SWD on EXP)
#define PIN_IO_2            17   //   bit1   also RP2040 GP11 (SWD on EXP)
#define PIN_IO_3            18   //   bit2   BOOT_SELECT, SW6 via R29 + R34 1k
#define PIN_IO_4            19   //   bit3   SW5 via R30 1k; RP2040 GP12 via R3 0R
#define PIN_MISO            20   //   bit4   spi0_rx
#define PIN_OW              21   //   bit5   (spi0_ss_n) 1-Wire deck ID, nRF51 domain
#define PIN_SCK             22   //   bit6   spi0_sclk
#define PIN_MOSI            23   //   bit7   spi0_tx
#define PIN_SLOW_BASE       24
#define PIN_WKUP            24   //   bit0   nRF51 domain, no series R
#define PIN_N_IO_1          25   //   bit1   nRF51 domain, no series R
#define PIN_TX2             26   //   bit2   uart1_tx (F11 AUX), via mux -> P2.1 TX2
#define PIN_RX2             27   //   bit3   uart1_rx (F11 AUX), via mux <- P2.2 RX2
#define PIN_TX1             28   //   bit4   uart0_tx -> P3.3 TX1
#define PIN_RX1             29   //   bit5   uart0_rx <- P3.2 RX1
#define PIN_SDA             30   //   bit6   i2c1_sda
#define PIN_SCL             31   //   bit7   i2c1_scl

// Both UART ports are wired straight through: TX drives the Crazyflie's TX pin,
// because the deck impersonates the Crazyflie for the decks stacked on it.
// Consequence when sitting ON a Crazyflie: the CF's STM32 drives TX1 and TX2, so
// never give GP26/GP28 their UART function there — sniff only, and use a PIO UART
// TX on GP27/GP29 if the deck has to talk to the Crazyflie.

// --- FX2 slave FIFO (GP32-43, requires PIO gpio_base 16) ---
#define PIN_FX_FD0          32   // FD0..FD7 contiguous
#define PIN_FX_IFCLK        40   // side-set; RP2350 is the clock master
#define PIN_FX_SLWR_N       41
#define PIN_FX_PKTEND_N     42
#define PIN_FX_FLAGB        43   // EP6 full flag, active low (jmp pin)

// --- GP44-47: unconnected on rev A ---
// The ADC power telemetry (VCOM/VUSB/VCC dividers) was dropped; CF presence is
// sensed digitally on PIN_EXT_VCC_SENSE. These are the only ADC-capable pins left
// and the only spare pins inside the base-16 PIO window.
#define PIN_SPARE_ADC4      44
#define PIN_SPARE_ADC5      45
#define PIN_SPARE_ADC6      46
#define PIN_SPARE_ADC7      47
