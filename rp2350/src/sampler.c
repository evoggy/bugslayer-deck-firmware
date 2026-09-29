#include <string.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/sync.h"
#include "pico/time.h"

#include "board.h"
#include "sampler.h"
#include "sampler.pio.h"

#define HALF_SAMPLES (SAMPLER_RING_SAMPLES / 2)
#define HALF_WORDS   (HALF_SAMPLES / 2)          // two samples per 32-bit word

static uint16_t __aligned(4) s_ring[SAMPLER_RING_SAMPLES];

static PIO  s_pio;
static uint s_sm;
static uint s_offset;
static int  s_dma[2];
static volatile uint32_t s_halves_done;          // completed halves since start
static volatile bool     s_running;
static uint64_t          s_final;                // produced count after stop

static void __isr dma_irq_handler(void) {
    for (uint i = 0; i < 2; i++) {
        if (dma_channel_get_irq1_status(s_dma[i])) {
            dma_channel_acknowledge_irq1(s_dma[i]);
            s_halves_done++;
            // Re-arm for its next turn; its partner is filling the other half now.
            dma_channel_set_write_addr(s_dma[i], &s_ring[i * HALF_SAMPLES], false);
        }
    }
}

void sampler_init(void) {
    s_pio = pio1;
    hard_assert(pio_set_gpio_base(s_pio, 16) == PICO_OK);
    s_sm     = pio_claim_unused_sm(s_pio, true);
    s_offset = pio_add_program(s_pio, &sampler_program);

    for (uint i = 0; i < 2; i++) s_dma[i] = dma_claim_unused_channel(true);
    irq_add_shared_handler(DMA_IRQ_1, dma_irq_handler,
                           PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
    irq_set_enabled(DMA_IRQ_1, true);
}

void sampler_start(uint32_t clkdiv, uint32_t *rate_num, uint32_t *rate_den) {
    sampler_program_init(s_pio, s_sm, s_offset, PIN_CAP_BASE, clkdiv);
    pio_sm_clear_fifos(s_pio, s_sm);

    for (uint i = 0; i < 2; i++) {
        dma_channel_config c = dma_channel_get_default_config(s_dma[i]);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
        channel_config_set_read_increment(&c, false);
        channel_config_set_write_increment(&c, true);
        channel_config_set_dreq(&c, pio_get_dreq(s_pio, s_sm, false));
        channel_config_set_chain_to(&c, s_dma[i ^ 1]);
        dma_channel_configure(s_dma[i], &c, &s_ring[i * HALF_SAMPLES],
                              &s_pio->rxf[s_sm], HALF_WORDS, false);
        dma_channel_acknowledge_irq1(s_dma[i]);
        dma_channel_set_irq1_enabled(s_dma[i], true);
    }

    s_halves_done = 0;
    s_running = true;
    dma_channel_start(s_dma[0]);
    pio_sm_set_enabled(s_pio, s_sm, true);

    *rate_num = clock_get_hz(clk_sys);
    *rate_den = clkdiv;
}

// Samples in the ring right now. The active channel's remaining count and the
// halves counter can change between reads; retry until both are stable.
static uint64_t produced_now(void) {
    for (;;) {
        uint32_t halves = s_halves_done;
        int active = halves & 1;             // halves alternate 0, 1, 0, ...
        uint32_t remaining = dma_channel_hw_addr(s_dma[active])->transfer_count;
        if (halves == s_halves_done) {
            uint64_t words = (uint64_t)halves * HALF_WORDS + (HALF_WORDS - remaining);
            return words * 2;
        }
    }
}

void sampler_stop(void) {
    if (!s_running) return;
    pio_sm_set_enabled(s_pio, s_sm, false);
    // Let the DMA take the last words out of the RX FIFO.
    while (!pio_sm_is_rx_fifo_empty(s_pio, s_sm)) tight_loop_contents();
    busy_wait_us(2);
    s_final = produced_now();
    for (uint i = 0; i < 2; i++) {
        dma_channel_set_irq1_enabled(s_dma[i], false);
        dma_channel_abort(s_dma[i]);
        dma_channel_acknowledge_irq1(s_dma[i]);
    }
    s_running = false;
}

uint64_t sampler_produced(void) {
    return s_running ? produced_now() : s_final;
}

// Two memcpys at most: at the FX2 sink's rates the CPU has a handful of
// cycles per sample, and a per-sample modulo does not fit.
void sampler_copy(uint16_t *dst, uint64_t index, uint32_t n) {
    uint32_t at    = (uint32_t)(index % SAMPLER_RING_SAMPLES);
    uint32_t first = SAMPLER_RING_SAMPLES - at;
    if (first > n) first = n;
    memcpy(dst, &s_ring[at], first * sizeof(uint16_t));
    memcpy(dst + first, s_ring, (n - first) * sizeof(uint16_t));
}
