#include <stdio.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/structs/pads_bank0.h"
#include "pico/stdlib.h"

#include "board.h"
#include "fx2_link.h"
#include "fx2_write.pio.h"

#define PIO_CYCLES_PER_IFCLK 8

// Two 8 KiB buffers ping-pong through the PIO TX FIFO. At 18.75 MB/s each
// buffer lasts ~437 us, so the refill IRQ is comfortably infrequent.
#define BUF_WORDS 2048

static PIO      s_pio;
static uint     s_sm;
static uint     s_offset;
static bool     s_up;
static uint32_t s_ifclk_hz;

static uint32_t s_buf[2][BUF_WORDS];
static int      s_dma[2];
static bool     s_running;
static uint32_t s_next_word;
static uint64_t s_words_total;
static bool     s_flushed = true;

static void fill(uint which) {
    uint32_t v = s_next_word;
    for (uint i = 0; i < BUF_WORDS; i++) s_buf[which][i] = v++;
    s_next_word = v;
    s_words_total += BUF_WORDS;
}

// Refill whichever buffer just drained. Its partner is streaming right now.
static void __isr dma_irq_handler(void) {
    for (uint i = 0; i < 2; i++) {
        if (dma_channel_get_irq0_status(s_dma[i])) {
            dma_channel_acknowledge_irq0(s_dma[i]);
            if (!s_running) continue;
            fill(i);
            dma_channel_set_read_addr(s_dma[i], s_buf[i], false);
        }
    }
}

static void jump_to(uint target) {
    pio_sm_exec(s_pio, s_sm, pio_encode_jmp(s_offset + target));
}

void fx2_link_init(void) {
    gpio_init(PIN_FX_RESET_N);
    gpio_put(PIN_FX_RESET_N, 0);
    gpio_set_dir(PIN_FX_RESET_N, GPIO_OUT);

    s_pio = pio0;
    hard_assert(pio_set_gpio_base(s_pio, 16) == PICO_OK);
    s_sm     = pio_claim_unused_sm(s_pio, true);
    s_offset = pio_add_program(s_pio, &fx2_write_program);

    s_ifclk_hz = clock_get_hz(clk_sys) / PIO_CYCLES_PER_IFCLK;
    fx2_write_program_init(s_pio, s_sm, s_offset, PIN_FX_FD0, PIN_FX_IFCLK,
                           PIN_FX_SLWR_N, PIN_FX_FLAGB, 1.0f);

    for (uint i = 0; i < 2; i++) s_dma[i] = dma_claim_unused_channel(true);
    irq_add_shared_handler(DMA_IRQ_0, dma_irq_handler,
                           PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
    irq_set_enabled(DMA_IRQ_0, true);
}

uint32_t fx2_link_set_ifclk(uint32_t hz) {
    if (hz == 0) return s_ifclk_hz;
    float div = (float)clock_get_hz(clk_sys) / (float)(hz * PIO_CYCLES_PER_IFCLK);
    if (div < 1.0f)     div = 1.0f;
    if (div > 65535.0f) div = 65535.0f;
    pio_sm_set_clkdiv(s_pio, s_sm, div);
    s_ifclk_hz = (uint32_t)((float)clock_get_hz(clk_sys) / (div * PIO_CYCLES_PER_IFCLK));
    return s_ifclk_hz;
}

uint32_t fx2_link_get_ifclk(void) { return s_ifclk_hz; }

void fx2_link_up(void) {
    if (s_up) return;
    // IFCLK first: the FX2's boot firmware writes IFCONFIG and the FIFO
    // registers, and with IFCLKSRC=0 those need a running clock on GP40.
    jump_to(fx2_write_offset_clk_only);
    pio_sm_set_enabled(s_pio, s_sm, true);
    sleep_us(100);
    gpio_put(PIN_FX_RESET_N, 1);
    s_up = true;
}

void fx2_link_down(void) {
    fx2_counter_stop();
    gpio_put(PIN_FX_RESET_N, 0);
    pio_sm_set_enabled(s_pio, s_sm, false);
    s_up = false;
}

bool fx2_link_is_up(void) { return s_up; }

void fx2_counter_start(void) {
    if (s_running || !s_up) return;

    s_next_word   = 0;
    s_words_total = 0;
    fill(0);
    fill(1);

    // Each channel drains one buffer into the TX FIFO, then chains to the other.
    for (uint i = 0; i < 2; i++) {
        dma_channel_config c = dma_channel_get_default_config(s_dma[i]);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
        channel_config_set_read_increment(&c, true);
        channel_config_set_write_increment(&c, false);
        channel_config_set_dreq(&c, pio_get_dreq(s_pio, s_sm, true));
        channel_config_set_chain_to(&c, s_dma[i ^ 1]);
        dma_channel_configure(s_dma[i], &c, &s_pio->txf[s_sm], s_buf[i], BUF_WORDS, false);
        dma_channel_set_irq0_enabled(s_dma[i], true);
    }

    // Start from an empty TX FIFO and OSR. Anything left over from a previous
    // run would go out first, and a partial word shifts every word after it.
    // Stopping the SM pauses IFCLK for a few cycles, which is harmless once
    // the FX2 has configured itself.
    pio_sm_set_enabled(s_pio, s_sm, false);
    pio_sm_clear_fifos(s_pio, s_sm);
    pio_sm_restart(s_pio, s_sm);
    jump_to(fx2_write_offset_stall);   // always re-enter with SLWR# deasserted
    pio_sm_exec(s_pio, s_sm, pio_encode_set(pio_pins, 0b11));
    pio_sm_set_enabled(s_pio, s_sm, true);

    s_running = true;
    dma_channel_start(s_dma[0]);
}

void fx2_counter_stop(void) {
    if (!s_running) return;
    s_running = false;
    for (uint i = 0; i < 2; i++) {
        dma_channel_set_irq0_enabled(s_dma[i], false);
        dma_channel_abort(s_dma[i]);
        dma_channel_acknowledge_irq0(s_dma[i]);
    }
    // Let the SM finish every word already queued, so the FX2 only ever holds
    // whole words: TXSTALL means the FIFO and the OSR are both empty. If EP6 is
    // full and nobody is reading, that never happens; give up after 100 ms and
    // let the next arm discard the rest.
    s_pio->fdebug = 1u << (PIO_FDEBUG_TXSTALL_LSB + s_sm);
    absolute_time_t deadline = make_timeout_time_ms(100);
    while (!(s_pio->fdebug & (1u << (PIO_FDEBUG_TXSTALL_LSB + s_sm))) &&
           !time_reached(deadline))
        tight_loop_contents();
    s_flushed = (s_pio->fdebug & (1u << (PIO_FDEBUG_TXSTALL_LSB + s_sm))) != 0;
    jump_to(fx2_write_offset_clk_only);
    // TODO(stage 5): pulse PKTEND# here so a partial packet is committed
    // instead of being stranded in EP6. Not needed while the counter runs
    // continuously and every packet fills.
}

bool fx2_counter_running(void) { return s_running; }

bool fx2_counter_flushed(void) { return s_flushed; }

uint64_t fx2_counter_words(void) { return s_words_total; }

void fx2_link_debug(void) {
    uint pc = pio_sm_get_pc(s_pio, s_sm) - s_offset;
    printf("dbg flagb=%d iso=%d pc=%u (clk_only=%u stall=%u) txlevel=%u "
           "dma0=%lu dma1=%lu busy=%d/%d\n",
           gpio_get(PIN_FX_FLAGB),
           (int)!!(pads_bank0_hw->io[PIN_FX_FLAGB] & PADS_BANK0_GPIO0_ISO_BITS),
           pc, fx2_write_offset_clk_only, fx2_write_offset_stall,
           pio_sm_get_tx_fifo_level(s_pio, s_sm),
           (unsigned long)dma_channel_hw_addr(s_dma[0])->transfer_count,
           (unsigned long)dma_channel_hw_addr(s_dma[1])->transfer_count,
           dma_channel_is_busy(s_dma[0]), dma_channel_is_busy(s_dma[1]));
}
