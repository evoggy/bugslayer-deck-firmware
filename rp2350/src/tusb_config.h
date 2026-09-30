// TinyUSB configuration for the RP2350 control plane.
//
// One composite device, 35F0:DB12:
//   interfaces 0+1  CDC  - ASCII control protocol (pico_stdio_usb uses CDC 0)
//   interface  2    vendor, bulk IN - the capture stream, same 512-byte blocks
//                   as the FX2 sends. MS OS 2.0 descriptors bind WinUSB to it.
//   interfaces 3+4  CDC  - Crazyflie UART1 bridge (uart_bridge.c)
//   interfaces 5+6  CDC  - Crazyflie UART2 bridge
// See usb_descriptors.c and ../../docs/protocol.md.
#pragma once

#define CFG_TUSB_RHPORT0_MODE       (OPT_MODE_DEVICE)
#define CFG_TUD_ENDPOINT0_SIZE      64

#define CFG_TUD_CDC                 3
#define CFG_TUD_CDC_RX_BUFSIZE      256
#define CFG_TUD_CDC_TX_BUFSIZE      1024
#define CFG_TUD_CDC_EP_BUFSIZE      64

#define CFG_TUD_VENDOR              1
#define CFG_TUD_VENDOR_RX_BUFSIZE   64
// Room for several whole blocks, so the stream producer can queue one while
// the previous ones drain at Full Speed.
#define CFG_TUD_VENDOR_TX_BUFSIZE   4096
#define CFG_TUD_VENDOR_EPSIZE       64
