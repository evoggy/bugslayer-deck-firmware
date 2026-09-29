/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2021 Raspberry Pi (Trading) Ltd.
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

#include <pico/stdlib.h>
#include <stdio.h>
#include <string.h>

#include <hardware/clocks.h>
#include <hardware/gpio.h>

#include "probe_config.h"
#include "probe.h"
#include "tusb.h"

#define DIV_ROUND_UP(m, n)	(((m) + (n) - 1) / (n))

// Only want to set / clear one gpio per event so go up in powers of 2
enum _dbg_pins {
    DBG_PIN_WRITE = 1,
    DBG_PIN_WRITE_WAIT = 2,
    DBG_PIN_READ = 4,
    DBG_PIN_PKT = 8,
};

CU_REGISTER_DEBUG_PINS(probe_timing)

// Uncomment to enable debug
//CU_SELECT_DEBUG_PINS(probe_timing)

#define PROBE_BUF_SIZE 8192
struct _probe {
    const char* name;
    
    // PIO offset
    uint offset;
    uint initted;

    // swd port
    int clkPin;
    int dioPin;
    PIO pio;
    int sm;        

    int activityLedPin;
};


static struct _probe probes[MAX_DAP_PROBES] = {
    PROBE_CONFIG0,       // Default
    PROBE_CONFIG1,
    PROBE_CONFIG2,
    PROBE_CONFIG3
};


void DAP_SETUP (uint32_t probeId) 
{
    assert(probeId < MAX_DAP_PROBES);    
    probe_gpio_init(probes[probeId].pio, probes[probeId].clkPin, probes[probeId].dioPin);
}


void probe_set_swclk_freq(uint32_t probeId, uint freq_khz) {
    assert(probeId < MAX_DAP_PROBES);
    uint clk_sys_freq_khz = clock_get_hz(clk_sys) / 1000;
    probe_info("Set swclk freq %dKHz sysclk %dkHz\n", freq_khz, clk_sys_freq_khz);
    uint32_t divider = clk_sys_freq_khz / freq_khz / 4;
    if (divider == 0)
        divider = 1;
    pio_sm_set_clkdiv_int_frac(probes[probeId].pio, probes[probeId].sm, divider, 0);
}

void probe_assert_reset(uint32_t probeId, bool state)
{
    assert(probeId < MAX_DAP_PROBES);
#if defined(PROBE_PIN_RESET)
    /* Change the direction to out to drive pin to 0 or to in to emulate open drain */
    gpio_set_dir(PROBE_PIN_RESET, state == 0 ? GPIO_OUT : GPIO_IN);
#endif
}

int probe_reset_level(uint32_t probeId)
{
    assert(probeId < MAX_DAP_PROBES);
#if defined(PROBE_PIN_RESET)
    return gpio_get(PROBE_PIN_RESET);
#else
    return 0;
#endif
}

typedef enum probe_pio_command {
    CMD_WRITE = 0,
    CMD_SKIP,
    CMD_TURNAROUND,
    CMD_READ
} probe_pio_command_t;

static inline uint32_t fmt_probe_command(uint32_t probeId, uint bit_count, bool out_en, probe_pio_command_t cmd) {
    assert(probeId < MAX_DAP_PROBES);
    uint cmd_addr =
        cmd == CMD_WRITE      ? probes[probeId].offset + probe_offset_write_cmd :
        cmd == CMD_SKIP       ? probes[probeId].offset + probe_offset_get_next_cmd :
        cmd == CMD_TURNAROUND ? probes[probeId].offset + probe_offset_turnaround_cmd :
                                probes[probeId].offset + probe_offset_read_cmd;
    return ((bit_count - 1) & 0xff) | ((uint)out_en << 8) | (cmd_addr << 9);
}

void probe_write_bits(uint32_t probeId, uint bit_count, uint32_t data_byte) {
    assert(probeId < MAX_DAP_PROBES);
    DEBUG_PINS_SET(probe_timing, DBG_PIN_WRITE);
    pio_sm_put_blocking(probes[probeId].pio, probes[probeId].sm, fmt_probe_command(probeId, bit_count, true, CMD_WRITE));
    pio_sm_put_blocking(probes[probeId].pio, probes[probeId].sm, data_byte);
    probe_dump("Write %d bits 0x%x\n", bit_count, data_byte);
    // Return immediately so we can cue up the next command whilst this one runs
    DEBUG_PINS_CLR(probe_timing, DBG_PIN_WRITE);
}

void probe_hiz_clocks(uint32_t probeId, uint bit_count) {
    assert(probeId < MAX_DAP_PROBES);
    pio_sm_put_blocking(probes[probeId].pio, probes[probeId].sm, fmt_probe_command(probeId, bit_count, false, CMD_TURNAROUND));
    pio_sm_put_blocking(probes[probeId].pio, probes[probeId].sm, 0);
}

uint32_t probe_read_bits(uint32_t probeId, uint bit_count) {
    assert(probeId < MAX_DAP_PROBES);
    DEBUG_PINS_SET(probe_timing, DBG_PIN_READ);
    pio_sm_put_blocking(probes[probeId].pio, probes[probeId].sm, fmt_probe_command(probeId, bit_count, false, CMD_READ));
    uint32_t data = pio_sm_get_blocking(probes[probeId].pio, probes[probeId].sm);
    uint32_t data_shifted = data;
    if (bit_count < 32) {
        data_shifted = data >> (32 - bit_count);
    }

    probe_dump("Read %d bits 0x%x (shifted 0x%x)\n", bit_count, data, data_shifted);
    DEBUG_PINS_CLR(probe_timing, DBG_PIN_READ);
    return data_shifted;
}

static void probe_wait_idle(uint32_t probeId) {
    assert(probeId < MAX_DAP_PROBES);
    probes[probeId].pio->fdebug = 1u << (PIO_FDEBUG_TXSTALL_LSB + probes[probeId].sm);
    while (!(probes[probeId].pio->fdebug & (1u << (PIO_FDEBUG_TXSTALL_LSB + probes[probeId].sm))))
        ;
}

void probe_read_mode(uint32_t probeId) {
    assert(probeId < MAX_DAP_PROBES);
    pio_sm_put_blocking(probes[probeId].pio, probes[probeId].sm, fmt_probe_command(probeId, 0, false, CMD_SKIP));
    probe_wait_idle(probeId);
}

void probe_write_mode(uint32_t probeId) {
    assert(probeId < MAX_DAP_PROBES);
    pio_sm_put_blocking(probes[probeId].pio, probes[probeId].sm, fmt_probe_command(probeId, 0, true, CMD_SKIP));
    probe_wait_idle(probeId);
}


void probe_init(uint32_t probeId) {
    assert(probeId < MAX_DAP_PROBES);

    if (!probes[probeId].initted) {
        uint offset = pio_add_program(probes[probeId].pio, &probe_program);
        probes[probeId].offset = offset;

        pio_sm_config sm_config = probe_program_get_default_config(offset);
        probe_sm_init(&sm_config, probes[probeId].pio, probes[probeId].sm, probes[probeId].clkPin, probes[probeId].dioPin);
        pio_sm_init(probes[probeId].pio, probes[probeId].sm, offset, &sm_config);

        // Set up divisor
        probe_set_swclk_freq(probeId, 1000);

        // Jump SM to command dispatch routine, and enable it
        pio_sm_exec(probes[probeId].pio, probes[probeId].sm, offset + probe_offset_get_next_cmd);
        pio_sm_set_enabled(probes[probeId].pio, probes[probeId].sm, 1);
        probes[probeId].initted = 1;
    }
}

void probe_deinit(uint32_t probeId)
{
  assert(probeId < MAX_DAP_PROBES);
  if (probes[probeId].initted) {
    probe_read_mode(probeId);
    pio_sm_set_enabled(probes[probeId].pio, probes[probeId].sm, 0);
    pio_remove_program(probes[probeId].pio, &probe_program, probes[probeId].offset);

    probe_assert_reset(probeId, 1);	// de-assert nRESET

    probes[probeId].initted = 0;  
  }
}

void probe_led_init()
{
    for(int i=0;i<MAX_DAP_PROBES;i++)
    {
        struct _probe* probe = &probes[i];
        if(probe->activityLedPin != -1)
        {
            // Led off
            gpio_init(probe->activityLedPin);
            gpio_set_dir(probe->activityLedPin, GPIO_OUT);    
            gpio_put(probe->activityLedPin, 0);    
        }
    }
}

void probe_led_update()
{
    for(int i=0;i<MAX_DAP_PROBES;i++)
    {
        struct _probe* probe = &probes[i];
        if(probe->activityLedPin != -1)
        {
            if(probes[i].initted)
            {                
                // Use pullup to dim led ( full bright activity not used as swd traffic is constant )
                gpio_init(probe->activityLedPin);
                gpio_set_dir(probe->activityLedPin, GPIO_IN);    
                gpio_pull_up(probe->activityLedPin);    
            }
            else
            {
                // Led off
                gpio_init(probe->activityLedPin);
                gpio_set_dir(probe->activityLedPin, GPIO_OUT);    
                gpio_put(probe->activityLedPin, 0);    
            }
        }
    }
}