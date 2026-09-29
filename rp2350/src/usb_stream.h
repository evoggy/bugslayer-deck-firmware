// Capture stream sink: the vendor bulk IN interface of the RP2350's own USB.
#pragma once
#include <stdbool.h>

#include "block.h"

#define USB_STREAM_ITF    2      // interface number, claimed by host tools
#define USB_STREAM_EP_IN  0x83

// True if a whole block fits in the TX FIFO right now.
bool usb_stream_ready(void);

// Queue one block. Call only after usb_stream_ready(). Never blocks.
void usb_stream_write(const block_t *b);
