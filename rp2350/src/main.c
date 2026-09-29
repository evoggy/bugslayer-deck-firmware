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
#include "tusb.h"

#include "capture.h"
#include "fx2_boot.h"
#include "fx2_link.h"

#define FW_VERSION "0.2.0"

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
        PIN_LED_1, PIN_LED_2, PIN_LED_3, PIN_LED_4, PIN_LED_5,
    };
    for (uint i = 0; i < count_of(outputs); i++) {
        gpio_init(outputs[i]);
        gpio_put(outputs[i], 0);
        gpio_set_dir(outputs[i], GPIO_OUT);
    }

    // I2C pull-ups off. R9/R10 (2.2k) run from SDA/SCL to this pin: it is
    // their supply. Driving it LOW would make them 2.2k pull-DOWNS on the
    // Crazyflie's I2C bus -- which stopped the CF's STM32 from booting. Off is
    // Hi-Z; `pull on` drives it high.
    gpio_init(PIN_EXT_I2C_PULL_EN);
    gpio_disable_pulls(PIN_EXT_I2C_PULL_EN);

    // Every CF signal (GP16-31) a plain input: no pulls, input buffer on,
    // pad isolation released. Out of reset the pads have a ~50k pull-down,
    // which a weak pull-up on the CF side cannot beat. Sniffing must be
    // invisible to the Crazyflie.
    for (uint pin = PIN_CAP_BASE; pin < PIN_CAP_BASE + 16; pin++) {
        gpio_init(pin);
        gpio_disable_pulls(pin);
    }

    // The USB/UART mux pins stay inputs: the straps (R32 up, R13 down) select
    // the UART leg with the mux enabled, which is the safe state.

    // CF presence. Input only - driving it low would short the port VCC rail
    // through U13. The rail floats when U13 is off, hence the pull-down.
    gpio_init(PIN_EXT_VCC_SENSE);
    gpio_set_dir(PIN_EXT_VCC_SENSE, GPIO_IN);
    gpio_pull_down(PIN_EXT_VCC_SENSE);

    // FX_SDA/FX_SCL (GP4/5) belong to fx2_boot.c, which emulates the FX2's
    // boot EEPROM there.

    pico_get_unique_board_id_string(s_serial, sizeof(s_serial));
}

static void update_leds(void) {
    gpio_put(LED_FX2_UP, fx2_link_is_up());
    capture_status_t cs;
    capture_status(&cs);
    gpio_put(LED_STREAMING, fx2_counter_running() || cs.busy);
}

static void cmd_help(void) {
    puts("commands:");
    puts("  ping                  -> pong");
    puts("  ver                   firmware version");
    puts("  id                    board serial (shared with the FX2 later)");
    puts("  stat                  capture, FX2 link and board state");
    puts("  arm <rate_hz> [pins|counter]");
    puts("                        start a capture session into the vendor bulk IN:");
    puts("                        the 16 CF signals (default) or a synthetic counter.");
    puts("                        2300..2000000 Hz; ~380 ksps is what USB FS carries");
    puts("  disarm                stop sampling; the stream ends with an END block");
    puts("  clk <hz>              set IFCLK (5000000..37500000 = clkdiv 1)");
    puts("  fx2 up|down           release/assert FX_RESET# with IFCLK running");
    puts("  fx2 reboot            down, then up: the FX2 boots again");
    puts("  fx2 boot [rom|c0|c2]  what the emulated EEPROM serves at the next up:");
    puts("                        c2 = our firmware (default), c0 = our VID/PID for");
    puts("                        fx2tool RAM loads, rom = silent, 04b4:8613");
    puts("  fx2 test start|stop   stage 1 pipe test: raw 32-bit counter into the FX2");
    puts("  pwr vcc|vcom on|off   high-side switches");
    puts("  pull on|off           I2C pull-ups (standalone only!); off = Hi-Z, never low");
    puts("  pins                  live level of every CF signal");
    puts("  dbg                   FX2 link internals (EP6 room, PIO PC, DMA)");
    puts("  prof                  write engine: streaming / flow-controlled / starved");
}

static void cmd_stat(void) {
    capture_status_t cs;
    capture_status(&cs);
    printf("stat armed=%d busy=%d session=%lu rate=%lu samples=%llu blocks=%lu overruns=%lu lost=%llu\n",
           cs.armed, cs.busy, (unsigned long)cs.session, (unsigned long)cs.rate_hz,
           (unsigned long long)cs.samples, (unsigned long)cs.blocks,
           (unsigned long)cs.overruns, (unsigned long long)cs.lost);
    printf("stat fx2=%s boot=%s eeprom_read=%lu ifclk=%lu counter=%s words=%llu cf_vcc=%d\n",
           fx2_link_is_up() ? "up" : "down",
           fx2_boot_mode_name(fx2_boot_get_mode()),
           (unsigned long)fx2_boot_bytes_served(),
           (unsigned long)fx2_link_get_ifclk(),
           fx2_counter_running() ? "running" : "stopped",
           (unsigned long long)fx2_counter_words(),
           gpio_get(PIN_EXT_VCC_SENSE));
}

// Serve the boot image from a fresh count, then start IFCLK and release reset.
static void fx2_up(void) {
    fx2_boot_reset_stats();
    fx2_link_up();
}

static void handle(char *line) {
    char *cmd = strtok(line, " ");
    char *a1  = strtok(NULL, " ");
    char *a2  = strtok(NULL, " ");
    if (!cmd) return;

    if (!strcmp(cmd, "ping")) {
        puts("pong");
    } else if (!strcmp(cmd, "ver")) {
        printf("ver rp2350=%s fx2_image=%lu hw=v1\n", FW_VERSION,
               (unsigned long)fx2_boot_image_len());
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
            fx2_up();
            printf("fx2 ok up boot=%s ifclk=%lu\n", fx2_boot_mode_name(fx2_boot_get_mode()),
                   (unsigned long)fx2_link_get_ifclk());
        } else if (!strcmp(a1, "down")) {
            fx2_link_down();
            puts("fx2 ok down");
        } else if (!strcmp(a1, "reboot")) {
            fx2_link_down();
            sleep_ms(10);
            fx2_up();
            printf("fx2 ok reboot boot=%s\n", fx2_boot_mode_name(fx2_boot_get_mode()));
        } else if (!strcmp(a1, "boot")) {
            if (a2) {
                fx2_boot_mode_t m = !strcmp(a2, "rom") ? FX2_BOOT_ROM
                                  : !strcmp(a2, "c0")  ? FX2_BOOT_C0
                                  : !strcmp(a2, "c2")  ? FX2_BOOT_C2 : (fx2_boot_mode_t)-1;
                if ((int)m < 0) {
                    puts("err usage: fx2 boot rom|c0|c2");
                    return;
                }
                fx2_boot_set_mode(m);
            }
            printf("fx2 ok boot=%s (applies at the next fx2 up/reboot)\n",
                   fx2_boot_mode_name(fx2_boot_get_mode()));
        } else if (!strcmp(a1, "test") && a2 && !strcmp(a2, "start")) {
            if (!fx2_link_is_up()) {
                puts("err fx2 is down; run `fx2 up` first");
            } else {
                fx2_counter_start();
                puts("fx2 ok test running source=counter");
            }
        } else if (!strcmp(a1, "test") && a2 && !strcmp(a2, "stop")) {
            fx2_counter_stop();
            printf("fx2 ok test stopped words=%llu flushed=%s\n",
                   (unsigned long long)fx2_counter_words(),
                   fx2_counter_flushed() ? "yes" : "no");
        } else {
            puts("err usage: fx2 up|down|reboot|boot|test");
        }
    } else if (!strcmp(cmd, "arm")) {
        uint32_t session;
        uint32_t rate = a1 ? (uint32_t)strtoul(a1, NULL, 0) : 0;
        capture_source_t src = (a2 && !strcmp(a2, "counter")) ? CAPTURE_COUNTER : CAPTURE_PINS;
        if (!a1 || (a2 && strcmp(a2, "counter") && strcmp(a2, "pins"))) {
            puts("err usage: arm <rate_hz> [pins|counter]");
        } else if (!capture_arm(rate, src, &session)) {
            puts("err already armed, or rate out of range (2300..2000000)");
        } else {
            printf("arm ok session=%lu rate=%lu source=%s sink=usb\n", (unsigned long)session,
                   (unsigned long)rate, src == CAPTURE_PINS ? "pins" : "counter");
        }
    } else if (!strcmp(cmd, "disarm")) {
        capture_status_t cs;
        capture_disarm();
        capture_status(&cs);
        printf("disarm ok session=%lu samples=%llu overruns=%lu lost=%llu\n",
               (unsigned long)cs.session, (unsigned long long)cs.samples,
               (unsigned long)cs.overruns, (unsigned long long)cs.lost);
    } else if (!strcmp(cmd, "pwr") && a1 && a2) {
        uint pin = !strcmp(a1, "vcc") ? PIN_EXT_VCC_EN
                 : !strcmp(a1, "vcom") ? PIN_EXT_VCOM_EN : 0xff;
        if (pin == 0xff) {
            puts("err usage: pwr vcc|vcom on|off");
        } else {
            gpio_put(pin, !strcmp(a2, "on"));
            printf("pwr ok %s=%s\n", a1, a2);
        }
    } else if (!strcmp(cmd, "pins")) {
        static const char *names[16] = {"IO_1", "IO_2", "IO_3", "IO_4", "MISO", "OW", "SCK",
                                        "MOSI", "WKUP", "N_IO_1", "TX2", "RX2", "TX1", "RX1",
                                        "SDA", "SCL"};
        printf("pins");
        for (uint i = 0; i < 16; i++) printf(" %s=%d", names[i], gpio_get(PIN_CAP_BASE + i));
        printf(" cf_vcc=%d\n", gpio_get(PIN_EXT_VCC_SENSE));
    } else if (!strcmp(cmd, "pull") && a1) {
        // Pull-up supply: high = 2.2k pull-ups on (standalone only), Hi-Z = off.
        if (!strcmp(a1, "on")) {
            gpio_put(PIN_EXT_I2C_PULL_EN, 1);
            gpio_set_dir(PIN_EXT_I2C_PULL_EN, GPIO_OUT);
        } else {
            gpio_set_dir(PIN_EXT_I2C_PULL_EN, GPIO_IN);
        }
        printf("pull ok %s\n", a1);
    } else {
        printf("err unknown command '%s' (try `help`)\n", cmd);
    }
}

int main(void) {
    // We own TinyUSB (composite CDC + vendor, usb_descriptors.c); stdio rides
    // on CDC 0 and expects the stack to be up before it starts.
    tusb_init();
    stdio_init_all();
    board_init();
    fx2_boot_init(s_serial);
    fx2_link_init();
    capture_init();

    // The deck is one device to the user: the FX2 boots with the RP2350.
    fx2_up();

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

        tud_task();
        capture_poll();

        int ch = getchar_timeout_us(0);
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
