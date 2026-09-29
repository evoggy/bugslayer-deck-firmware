#include "tusb.h"

#include "usb_stream.h"

bool usb_stream_ready(void) {
    return tud_vendor_mounted() && tud_vendor_write_available() >= BLOCK_SIZE;
}

void usb_stream_write(const block_t *b) {
    tud_vendor_write(b, BLOCK_SIZE);
    tud_vendor_write_flush();
}
