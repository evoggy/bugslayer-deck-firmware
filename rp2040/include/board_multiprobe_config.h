/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2025 Picolemon
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#ifndef BOARD_MULTIPROBE_H_
#define BOARD_MULTIPROBE_H_

#define PROBE_IO_RAW
#define PROBE_CDC_UART

// Debug probe config
#define UART_CONFIG0 { .index=0, .name="IFACE0", .txPin=4, .rxPin=5, .hwUartId=1, .baud=115200, .activityLedPin=19 }        // [debug 1]  
#define UART_CONFIG1 { .index=1, .name="IFACE1", .txPin=16, .rxPin=17, .hwUartId=0, .baud=115200, .activityLedPin=27 }      // [debug 2]  
#define UART_CONFIG2 { .index=2, .name="IFACE2", .txPin=8, .rxPin=9, .hwUartId=1, .baud=115200, .activityLedPin=21 }        // [debug 3]  
#define UART_CONFIG3 { .index=3, .name="IFACE3", .txPin=12, .rxPin=13, .hwUartId=0, .baud=115200, .activityLedPin=26 }      // [debug 4]  

// Debug probe config
#define PROBE_CONFIG0 {.name="IFACE0", .pio=pio0, .sm=0, .clkPin=2, .dioPin=3, .activityLedPin=18 }                         // [debug 1]
#define PROBE_CONFIG1 {.name="IFACE1", .pio=pio0, .sm=1, .clkPin=14, .dioPin=15, .activityLedPin=28 }                       // [debug 2]
#define PROBE_CONFIG2 {.name="IFACE2", .pio=pio1, .sm=1, .clkPin=6, .dioPin=7, .activityLedPin=20}                          // [debug 3]
#define PROBE_CONFIG3 {.name="IFACE3", .pio=pio1, .sm=0, .clkPin=10, .dioPin=11, .activityLedPin=22 }                       // [debug 4]

#define PROBE_USB_CONNECTED_LED 29

#define PROBE_PRODUCT_STRING "Multiprobe on Pico (CMSIS-DAP)"

#endif
