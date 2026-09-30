// I2C master on the Crazyflie deck bus. See i2c_master.h.

#include "i2c_master.h"

#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "pico/stdlib.h"

#include "board.h"

static bool s_on;
static uint32_t s_rate;

// Back to a plain input, exactly as board_init() leaves the CF signals.
static void release(uint pin) {
    gpio_init(pin);
    gpio_disable_pulls(pin);
}

uint32_t i2cm_enable(uint32_t hz) {
    s_rate = s_on ? i2c_set_baudrate(i2c1, hz) : i2c_init(i2c1, hz);
    if (!s_on) {
        // No pad pulls: the bus is pulled up by the deck's 2.2k (`pull on`)
        // or by the Crazyflie.
        gpio_set_function(PIN_SDA, GPIO_FUNC_I2C);
        gpio_set_function(PIN_SCL, GPIO_FUNC_I2C);
        s_on = true;
    }
    return s_rate;
}

void i2cm_disable(void) {
    if (!s_on) return;
    i2c_deinit(i2c1);
    release(PIN_SDA);
    release(PIN_SCL);
    s_on = false;
}

bool i2cm_enabled(void) { return s_on; }
uint32_t i2cm_rate(void) { return s_on ? s_rate : 0; }

// Worst case for n bytes at the current rate, doubled, plus room for clock
// stretching (a DeckCtrl stretches through its SPI bridge transfers).
static uint32_t timeout_us(size_t n) {
    return (uint32_t)((uint64_t)(n + 1) * 9 * 2000000 / s_rate) + 20000;
}

static i2cm_result_t result(int r) {
    return r == PICO_ERROR_TIMEOUT ? I2CM_TIMEOUT : r < 0 ? I2CM_NAK : I2CM_OK;
}

i2cm_result_t i2cm_xfer(uint8_t addr, const uint8_t *w, size_t wn, uint8_t *r, size_t rn) {
    if (!s_on) return I2CM_OFF;
    if (wn) {
        int n = i2c_write_timeout_us(i2c1, addr, w, wn, rn > 0, timeout_us(wn));
        if (n != (int)wn) return result(n < 0 ? n : PICO_ERROR_GENERIC);
    }
    if (rn) {
        int n = i2c_read_timeout_us(i2c1, addr, r, rn, false, timeout_us(rn));
        if (n != (int)rn) return result(n < 0 ? n : PICO_ERROR_GENERIC);
    }
    return I2CM_OK;
}

bool i2cm_recover(void) {
    bool was_on = s_on;
    uint32_t rate = s_rate;
    i2cm_disable();
    // Open drain by hand: drive low, or release to input for high.
    gpio_put(PIN_SCL, 0);
    gpio_put(PIN_SDA, 0);
    for (int i = 0; i < 9 && !gpio_get(PIN_SDA); i++) {
        gpio_set_dir(PIN_SCL, GPIO_OUT);
        sleep_us(5);
        gpio_set_dir(PIN_SCL, GPIO_IN);
        sleep_us(5);
    }
    // STOP: SDA low -> high while SCL is high.
    gpio_set_dir(PIN_SDA, GPIO_OUT);
    sleep_us(5);
    gpio_set_dir(PIN_SDA, GPIO_IN);
    sleep_us(5);
    bool sda = gpio_get(PIN_SDA);
    if (was_on) i2cm_enable(rate);
    return sda;
}
