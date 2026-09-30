/*
 * Bugslayer deck, rev A — on-board RP2040 probe (U2).
 *
 * Pin map from the rev-A schematic netlist (see docs/hardware.md). All four
 * ports use the same pin order: CLK, IO = CLK + 1, nRST. probe_sm_init()
 * relies on IO being CLK + 1.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef BOARD_BUGSLAYER_H_
#define BOARD_BUGSLAYER_H_

#define PROBE_IO_RAW
#define PROBE_CDC_UART

// SWD ports. Port SWDn is USB interface n: probe-rs `35f0:db11-n`, OpenOCD
// "cmsis-dap usb interface n". Each entry carries its own pins, PIO SM and LEDs,
// so the order here is only the numbering.
//   SWD0: the on-board RP2350 (U5 SWCLK / SWDIO / RUN)
//   SWD1: P1 (JST-SH), SWO on GP5
//   SWD2: P5 (JST-SH), SWO on GP9
//   SWD3: deck port IO_1 / IO_2 / IO_4 (P3.6 / P3.7 / P3.9) - shared with RP2350 GP16/17/19
#define PROBE_CONFIG0 { .name="SWD0", .pio=pio1, .sm=1, .clkPin=13, .dioPin=14, .rstPin=15, .activityLedPin=23, .runningLedPin=24 }
#define PROBE_CONFIG1 { .name="SWD1", .pio=pio0, .sm=0, .clkPin=2,  .dioPin=3,  .rstPin=4,  .activityLedPin=17, .runningLedPin=18 }
#define PROBE_CONFIG2 { .name="SWD2", .pio=pio0, .sm=1, .clkPin=6,  .dioPin=7,  .rstPin=8,  .activityLedPin=19, .runningLedPin=20 }
#define PROBE_CONFIG3 { .name="SWD3", .pio=pio1, .sm=0, .clkPin=10, .dioPin=11, .rstPin=12, .activityLedPin=21, .runningLedPin=22 }

// The only UART-capable target signals are the two SWO lines, both UART1 RX.
// Every UART1 TX pin (GP4, GP8, GP20, GP24) is an nRST or an LED, so the CDC
// ports are receive-only and the two SWO profiles cannot be active at once.
#define UART_CONFIG0 { .index=0, .name="SWO1", .txPin=-1, .rxPin=5,  .hwUartId=1, .baud=115200, .activityLedPin=-1 }
#define UART_CONFIG1 { .index=1, .name="SWO2", .txPin=-1, .rxPin=9,  .hwUartId=1, .baud=115200, .activityLedPin=-1 }
#define UART_CONFIG2 { .index=2, .name="NONE", .txPin=-1, .rxPin=-1, .hwUartId=0, .baud=115200, .activityLedPin=-1 }
#define UART_CONFIG3 { .index=3, .name="NONE", .txPin=-1, .rxPin=-1, .hwUartId=0, .baud=115200, .activityLedPin=-1 }

#define PROBE_USB_CONNECTED_LED 0

#define PROBE_USB_VID 0x35F0
#define PROBE_USB_PID 0xDB11
#define PROBE_MANUFACTURER_STRING "Bitcraze AB"
#define PROBE_PRODUCT_STRING "Bugslayer probe (CMSIS-DAP)"

// Per-interface strings. Each must contain "CMSIS-DAP" or probe-rs/OpenOCD skip it.
#define PROBE_ITF0_STRING "CMSIS-DAP SWD0 (RP2350)"
#define PROBE_ITF1_STRING "CMSIS-DAP SWD1 (P1)"
#define PROBE_ITF2_STRING "CMSIS-DAP SWD2 (P5)"
#define PROBE_ITF3_STRING "CMSIS-DAP SWD3 (deck port)"

#endif
