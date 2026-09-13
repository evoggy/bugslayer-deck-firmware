// Bugslayer deck — RP2350B pin map.
// Authority is crazyflie-exp-electronics/bugslayer; see ../../docs/hardware.md.
#pragma once

// --- control / housekeeping (GP0-15, never sniffed) ---
#define PIN_EXT_VCOM_EN      0
#define PIN_EXT_VCC_EN       1
#define PIN_EXT_I2C_PULL_EN  2   // high = 2.2k pull-ups on (standalone only)
#define PIN_USB_UART_MUX_SEL 3
#define PIN_USB_UART_MUX_NEN 4   // active low output enable
#define PIN_FX_RESET_N       5   // 10k pull-down holds the FX2 in reset
#define PIN_FX_SDA           6   // i2c1 — emulated FX2 boot EEPROM at 0xA2
#define PIN_FX_SCL           7

// --- CF expansion signals (GP16-31, the PIO overlap window) ---
#define PIN_CAP_BASE        16   // 8-pin capture group, one byte per sample:
#define PIN_MISO            16   //   bit0
#define PIN_IO_1            17   //   bit1
#define PIN_SCK             18   //   bit2
#define PIN_MOSI            19   //   bit3
#define PIN_IO_2            20   //   bit4
#define PIN_IO_3            21   //   bit5
#define PIN_IO_4            22   //   bit6
#define PIN_OW              23   //   bit7
#define PIN_SDA             24
#define PIN_SCL             25
#define PIN_EXT_TX2         26
#define PIN_EXT_RX2         27
#define PIN_TX1             28
#define PIN_RX1             29
#define PIN_N_IO_1          30
#define PIN_N_IO_2          31

// --- FX2 slave FIFO (GP32-43, requires PIO gpio_base 16) ---
#define PIN_FX_FD0          32   // FD0..FD7 contiguous
#define PIN_FX_IFCLK        40
#define PIN_FX_SLWR_N       41
#define PIN_FX_PKTEND_N     42
#define PIN_FX_FLAGB        43   // EP6 full flag, active low

// --- ADC ---
#define PIN_ADC_VCOM        44
#define PIN_ADC_VUSB        45
#define PIN_ADC_VCC         46   // Crazyflie presence detect
#define PIN_ADC_AUX         47
