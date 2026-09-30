// I2C master on the Crazyflie deck bus: I2C1 on GP30 (SDA) / GP31 (SCL).
//
// Off by default: the pins are plain inputs, invisible to a Crazyflie in the
// stack. Protocols (DeckCtrl, sensors) live on the host; this only moves bytes.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define I2CM_MAX_XFER 512

typedef enum {
    I2CM_OK = 0,
    I2CM_OFF,        // `i2c on` first
    I2CM_NAK,        // address or data not acknowledged
    I2CM_TIMEOUT,    // held low, or stretched past the timeout
} i2cm_result_t;

uint32_t i2cm_enable(uint32_t hz);   // returns the actual SCL rate
void     i2cm_disable(void);
bool     i2cm_enabled(void);
uint32_t i2cm_rate(void);

// Write `wn` bytes, then (repeated START) read `rn`; either may be 0, not both.
i2cm_result_t i2cm_xfer(uint8_t addr, const uint8_t *w, size_t wn, uint8_t *r, size_t rn);

// Clock SCL until a stuck slave lets go of SDA, then STOP. Works whether or
// not the master is on; leaves it as it was. Returns SDA's level afterwards.
bool i2cm_recover(void);
