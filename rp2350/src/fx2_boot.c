#include <string.h>

#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "pico/i2c_slave.h"

#include "board.h"
#include "fx2_boot.h"
#include "fx2_image.h"

#define EEPROM_ADDR   0x51      // 0xA2 >> 1: the FX2 ROM's 16-bit-addressed EEPROM
#define EEPROM_I2C    i2c0      // GP4 SDA / GP5 SCL, 2.2k pull-ups on the FX2 sheet
#define EEPROM_BAUD   400000    // the C2 config byte selects 400 kHz

static uint8_t          s_image[FX2_BOOT_IMAGE_LEN];
static fx2_boot_mode_t  s_mode = FX2_BOOT_C2;
static bool             s_slave_on;

// EEPROM state, touched only from the I2C IRQ.
static uint16_t s_addr;          // current read/write pointer
static uint8_t  s_addr_bytes;    // address bytes received in this write
static volatile uint32_t s_served;

// A 24LC-style EEPROM: a write sets the 16-bit address (data bytes after it are
// ignored -- this EEPROM is read-only); a read streams from the address and
// auto-increments. Past the image it reads 0xFF, like erased EEPROM.
static void i2c_handler(i2c_inst_t *i2c, i2c_slave_event_t event) {
    switch (event) {
    case I2C_SLAVE_RECEIVE: {
        uint8_t b = i2c_read_byte_raw(i2c);
        if (s_addr_bytes == 0)      s_addr = (uint16_t)b << 8;
        else if (s_addr_bytes == 1) s_addr |= b;
        if (s_addr_bytes < 2) s_addr_bytes++;
        break;
    }
    case I2C_SLAVE_REQUEST: {
        uint8_t b = s_addr < FX2_BOOT_IMAGE_LEN ? s_image[s_addr] : 0xFF;
        if (s_mode == FX2_BOOT_C0) {
            // Header only. And clear DISCON (config byte bit 6): in C2 our
            // firmware reconnects in usb_init(), but in C0 there is no firmware
            // to do it and the FX2 would never appear on the bus.
            if (s_addr == 0) b = 0xC0;
            if (s_addr == 7) b &= (uint8_t)~0x40;
        }
        i2c_write_byte_raw(i2c, b);
        s_addr++;
        s_served++;
        break;
    }
    case I2C_SLAVE_FINISH:
        s_addr_bytes = 0;
        break;
    }
}

static void slave_on(void) {
    if (s_slave_on) return;
    gpio_set_function(PIN_FX_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_FX_SCL, GPIO_FUNC_I2C);
    gpio_disable_pulls(PIN_FX_SDA);   // external 2.2k to +3V0
    gpio_disable_pulls(PIN_FX_SCL);
    i2c_init(EEPROM_I2C, EEPROM_BAUD);
    i2c_slave_init(EEPROM_I2C, EEPROM_ADDR, i2c_handler);
    s_slave_on = true;
}

static void slave_off(void) {
    if (!s_slave_on) return;
    i2c_slave_deinit(EEPROM_I2C);
    i2c_deinit(EEPROM_I2C);
    // Back to plain inputs: the boot ROM then finds no EEPROM.
    gpio_init(PIN_FX_SDA);
    gpio_init(PIN_FX_SCL);
    gpio_disable_pulls(PIN_FX_SDA);
    gpio_disable_pulls(PIN_FX_SCL);
    s_slave_on = false;
}

void fx2_boot_init(const char *serial) {
    memcpy(s_image, fx2_boot_image, sizeof(s_image));
#if FX2_BOOT_IMAGE_SERIAL_OFFSET >= 0
    size_t n = strlen(serial);
    if (n > FX2_BOOT_IMAGE_SERIAL_LEN) n = FX2_BOOT_IMAGE_SERIAL_LEN;
    memcpy(&s_image[FX2_BOOT_IMAGE_SERIAL_OFFSET], serial, n);
#else
#warning "FX2 image has no serial placeholder; the host cannot pair the FX2 with the RP2350"
    (void)serial;
#endif
    fx2_boot_set_mode(s_mode);
}

void fx2_boot_set_mode(fx2_boot_mode_t mode) {
    s_mode = mode;
    if (mode == FX2_BOOT_ROM) slave_off();
    else slave_on();
}

fx2_boot_mode_t fx2_boot_get_mode(void) { return s_mode; }

const char *fx2_boot_mode_name(fx2_boot_mode_t mode) {
    switch (mode) {
    case FX2_BOOT_ROM: return "rom";
    case FX2_BOOT_C0:  return "c0";
    case FX2_BOOT_C2:  return "c2";
    }
    return "?";
}

uint32_t fx2_boot_image_len(void) { return FX2_BOOT_IMAGE_LEN; }

uint32_t fx2_boot_bytes_served(void) { return s_served; }

void fx2_boot_reset_stats(void) { s_served = 0; }
