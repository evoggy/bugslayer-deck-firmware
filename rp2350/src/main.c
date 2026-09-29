// Bugslayer deck — RP2350B test firmware, bring-up stages 0 and 1.
//
//   stage 0: enumerate, blink, prove the rails and the hub  (docs/bringup-plan.md)
//   stage 1: stream a 32-bit counter through the FX2 and verify it on the PC
//
// The control protocol here is the ASCII line protocol from docs/protocol.md,
// minus everything that needs capture to exist. Type `help`.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include "pico/unique_id.h"

#include "board.h"
#include "fx2_link.h"

#define FW_VERSION "0.1.0"

static char s_serial[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1];

// LED roles during bring-up. LED_1 red, LED_2 green, LED_3 yellow, LED_4 blue,
// LED_5 orange (rev-A BOM).
#define LED_HEARTBEAT PIN_LED_2
#define LED_FX2_UP    PIN_LED_3
#define LED_STREAMING PIN_LED_4

// Everything that must not float or glitch a Crazyflie at power-on.
static void board_init(void) {
    const uint outputs[] = {
        PIN_EXT_VCOM_EN,      // both high-side switches off
        PIN_EXT_VCC_EN,
        PIN_EXT_I2C_PULL_EN,  // no pull-ups: assume we are stacked on a CF
        PIN_LED_1, PIN_LED_2, PIN_LED_3, PIN_LED_4, PIN_LED_5,
    };
    for (uint i = 0; i < count_of(outputs); i++) {
        gpio_init(outputs[i]);
        gpio_put(outputs[i], 0);
        gpio_set_dir(outputs[i], GPIO_OUT);
    }

    // The USB/UART mux pins stay inputs: the straps (R32 up, R13 down) select
    // the UART leg with the mux enabled, which is the safe state.

    // CF presence. Input only - driving it low would short the port VCC rail
    // through U13. The rail floats when U13 is off, hence the pull-down.
    gpio_init(PIN_EXT_VCC_SENSE);
    gpio_set_dir(PIN_EXT_VCC_SENSE, GPIO_IN);
    gpio_pull_down(PIN_EXT_VCC_SENSE);

    // FX_SDA/FX_SCL (GP4/5) stay untouched: with the RP2350 silent on I2C the
    // FX2 boot ROM finds no EEPROM and enumerates as 04B4:8613.

    pico_get_unique_board_id_string(s_serial, sizeof(s_serial));
}

static void update_leds(void) {
    gpio_put(LED_FX2_UP, fx2_link_is_up());
    gpio_put(LED_STREAMING, fx2_counter_running());
}

static void cmd_help(void) {
    puts("commands:");
    puts("  ping                  -> pong");
    puts("  ver                   firmware version");
    puts("  id                    board serial (shared with the FX2 later)");
    puts("  stat                  link and counter state");
    puts("  clk <hz>              set IFCLK (5000000..37500000 = clkdiv 1)");
    puts("  fx2 up|down           release/assert FX_RESET# with IFCLK running");
    puts("  arm                   start the stage 1 counter stream");
    puts("  disarm                stop it");
    puts("  pwr vcc|vcom on|off   high-side switches");
    puts("  pull on|off           I2C pull-ups (standalone only!)");
    puts("  dbg                   FX2 link internals (EP6 room, PIO PC, DMA)");
    puts("  prof                  write engine: streaming / flow-controlled / starved");
}

static void cmd_stat(void) {
    printf("stat fx2=%s ifclk=%lu counter=%s words=%llu cf_vcc=%d\n",
           fx2_link_is_up() ? "up" : "down",
           (unsigned long)fx2_link_get_ifclk(),
           fx2_counter_running() ? "running" : "stopped",
           (unsigned long long)fx2_counter_words(),
           gpio_get(PIN_EXT_VCC_SENSE));
}

static void handle(char *line) {
    char *cmd = strtok(line, " ");
    char *a1  = strtok(NULL, " ");
    char *a2  = strtok(NULL, " ");
    if (!cmd) return;

    if (!strcmp(cmd, "ping")) {
        puts("pong");
    } else if (!strcmp(cmd, "ver")) {
        printf("ver rp2350=%s fx2=none hw=v1\n", FW_VERSION);
    } else if (!strcmp(cmd, "id")) {
        printf("id serial=%s\n", s_serial);
    } else if (!strcmp(cmd, "stat")) {
        cmd_stat();
    } else if (!strcmp(cmd, "dbg")) {
        fx2_link_debug();
    } else if (!strcmp(cmd, "prof")) {
        fx2_link_profile(100000);
    } else if (!strcmp(cmd, "help")) {
        cmd_help();
    } else if (!strcmp(cmd, "clk") && a1) {
        printf("clk ok ifclk=%lu\n", (unsigned long)fx2_link_set_ifclk((uint32_t)atoi(a1)));
    } else if (!strcmp(cmd, "fx2") && a1) {
        if (!strcmp(a1, "up")) {
            fx2_link_up();
            printf("fx2 ok up ifclk=%lu\n", (unsigned long)fx2_link_get_ifclk());
        } else if (!strcmp(a1, "down")) {
            fx2_link_down();
            puts("fx2 ok down");
        } else {
            puts("err usage: fx2 up|down");
        }
    } else if (!strcmp(cmd, "arm")) {
        if (!fx2_link_is_up()) {
            puts("err fx2 is down; run `fx2 up` first");
        } else {
            fx2_counter_start();
            puts("arm ok source=counter sink=fx2");
        }
    } else if (!strcmp(cmd, "disarm")) {
        fx2_counter_stop();
        printf("disarm ok words=%llu flushed=%s\n", (unsigned long long)fx2_counter_words(),
               fx2_counter_flushed() ? "yes" : "no");
    } else if (!strcmp(cmd, "pwr") && a1 && a2) {
        uint pin = !strcmp(a1, "vcc") ? PIN_EXT_VCC_EN
                 : !strcmp(a1, "vcom") ? PIN_EXT_VCOM_EN : 0xff;
        if (pin == 0xff) {
            puts("err usage: pwr vcc|vcom on|off");
        } else {
            gpio_put(pin, !strcmp(a2, "on"));
            printf("pwr ok %s=%s\n", a1, a2);
        }
    } else if (!strcmp(cmd, "pull") && a1) {
        gpio_put(PIN_EXT_I2C_PULL_EN, !strcmp(a1, "on"));
        printf("pull ok %s\n", a1);
    } else {
        printf("err unknown command '%s' (try `help`)\n", cmd);
    }
}

int main(void) {
    stdio_init_all();
    board_init();
    fx2_link_init();

    char line[64];
    uint len = 0;
    absolute_time_t next_blink = get_absolute_time();
    bool led = false;

    while (true) {
        // Stage 0 sign of life.
        if (absolute_time_diff_us(get_absolute_time(), next_blink) <= 0) {
            led = !led;
            gpio_put(LED_HEARTBEAT, led);
            next_blink = delayed_by_ms(get_absolute_time(), 500);
        }
        update_leds();

        int ch = getchar_timeout_us(1000);
        if (ch == PICO_ERROR_TIMEOUT) continue;
        if (ch == '\r' || ch == '\n') {
            if (len) {
                line[len] = '\0';
                handle(line);
                len = 0;
            }
        } else if (len < sizeof(line) - 1) {
            line[len++] = (char)ch;
        }
    }
}
