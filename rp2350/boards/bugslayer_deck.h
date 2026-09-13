// Bugslayer deck — RP2350B, W25Q32 (4 MB) QSPI flash, 12 MHz crystal.
// -----------------------------------------------------
// NOTE: THIS HEADER IS ALSO INCLUDED BY ASSEMBLER SO
//       SHOULD ONLY CONSIST OF PREPROCESSOR DIRECTIVES
// -----------------------------------------------------
#ifndef _BOARDS_BUGSLAYER_DECK_H
#define _BOARDS_BUGSLAYER_DECK_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)

#define BUGSLAYER_DECK

// --- RP2350 VARIANT ---
// 0 means RP2350B: 48 GPIOs, 8 ADC channels, and PIO GPIO base support.
#define PICO_RP2350A 0

// --- FLASH --- W25Q32RVXHJQ
#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1

#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (4 * 1024 * 1024))
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (4 * 1024 * 1024)
#endif

pico_board_cmake_set_default(PICO_RP2350_A2_SUPPORTED, 1)
#ifndef PICO_RP2350_A2_SUPPORTED
#define PICO_RP2350_A2_SUPPORTED 1
#endif

// No default UART, LED, I2C or SPI: every pin is spoken for by the deck's own
// pin map (src/board.h) and the SDK defaults would collide with it.

#endif
