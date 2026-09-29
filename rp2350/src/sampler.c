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
#include "sck8.pio.h"

static uint16_t __aligned(4) s_raw16_ring[SAMPLER_RAW16_RING_SAMPLES];
static uint8_t  __aligned(4) s_sck8_ring[SAMPLER_SCK8_RING_SAMPLES];

typedef struct {
    uint8_t *ring;
    uint32_t ring_samples;
    uint32_t sample_bytes;
    uint32_t xfer_bytes;                         // DMA transfer size
    uint32_t half_xfers;                         // transfers per ring half
    uint     sm;
    int      dma[2];
    volatile uint32_t halves_done;               // completed halves since start
    volatile bool     running;
    uint64_t final;                              // produced count after stop
} chan_t;

static PIO    s_pio;
static uint   s_raw16_offset, s_sck8_offset, s_cswatch_offset;
static uint   s_cswatch_sm;                      // marks CS changes in the SCK8 stream
static chan_t s_chan[SAMPLER_COUNT];

static void __isr dma_irq_handler(void) {
    for (uint c = 0; c < SAMPLER_COUNT; c++) {
        chan_t *ch = &s_chan[c];
        for (uint i = 0; i < 2; i++) {
            if (dma_channel_get_irq1_status(ch->dma[i])) {
                dma_channel_acknowledge_irq1(ch->dma[i]);
                ch->halves_done++;
                // Re-arm for its next turn; its partner is filling the other half now.
                dma_channel_set_write_addr(ch->dma[i],
                                           ch->ring + i * (ch->ring_samples / 2) * ch->sample_bytes,
                                           false);
            }
        }
    }
}

void sampler_init(void) {
    s_pio = pio1;
    hard_assert(pio_set_gpio_base(s_pio, 16) == PICO_OK);
    s_raw16_offset = pio_add_program(s_pio, &sampler_program);
    s_sck8_offset  = pio_add_program(s_pio, &sck8_program);
    s_cswatch_offset = pio_add_program(s_pio, &cswatch_program);
    s_cswatch_sm   = pio_claim_unused_sm(s_pio, true);

    // RAW16: autopush packs two samples per 32-bit word.
    s_chan[SAMPLER_RAW16] = (chan_t){
        .ring = (uint8_t *)s_raw16_ring, .ring_samples = SAMPLER_RAW16_RING_SAMPLES,
        .sample_bytes = 2, .xfer_bytes = 4,
    };
    // SCK8: one byte per FIFO word, read as a byte.
    s_chan[SAMPLER_SCK8] = (chan_t){
        .ring = s_sck8_ring, .ring_samples = SAMPLER_SCK8_RING_SAMPLES,
        .sample_bytes = 1, .xfer_bytes = 1,
    };
    for (uint c = 0; c < SAMPLER_COUNT; c++) {
        chan_t *ch = &s_chan[c];
        ch->half_xfers = ch->ring_samples / 2 * ch->sample_bytes / ch->xfer_bytes;
        ch->sm = pio_claim_unused_sm(s_pio, true);
        for (uint i = 0; i < 2; i++) ch->dma[i] = dma_claim_unused_channel(true);
    }
    irq_add_shared_handler(DMA_IRQ_1, dma_irq_handler,
                           PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
    irq_set_enabled(DMA_IRQ_1, true);
}

static void chan_start(chan_t *ch) {
    pio_sm_clear_fifos(s_pio, ch->sm);
    for (uint i = 0; i < 2; i++) {
        dma_channel_config c = dma_channel_get_default_config(ch->dma[i]);
        channel_config_set_transfer_data_size(&c, ch->xfer_bytes == 4 ? DMA_SIZE_32 : DMA_SIZE_8);
        channel_config_set_read_increment(&c, false);
        channel_config_set_write_increment(&c, true);
        channel_config_set_dreq(&c, pio_get_dreq(s_pio, ch->sm, false));
        channel_config_set_chain_to(&c, ch->dma[i ^ 1]);
        dma_channel_configure(ch->dma[i], &c,
                              ch->ring + i * (ch->ring_samples / 2) * ch->sample_bytes,
                              &s_pio->rxf[ch->sm], ch->half_xfers, false);
        dma_channel_acknowledge_irq1(ch->dma[i]);
        dma_channel_set_irq1_enabled(ch->dma[i], true);
    }
    ch->halves_done = 0;
    ch->running = true;
    dma_channel_start(ch->dma[0]);
}

void sampler_start(uint32_t clkdiv, bool sck8, uint32_t *rate_num, uint32_t *rate_den) {
    sampler_program_init(s_pio, s_chan[SAMPLER_RAW16].sm, s_raw16_offset, PIN_CAP_BASE, clkdiv);
    chan_start(&s_chan[SAMPLER_RAW16]);
    uint mask = 1u << s_chan[SAMPLER_RAW16].sm;
    if (sck8) {
        sck8_program_init(s_pio, s_chan[SAMPLER_SCK8].sm, s_sck8_offset, PIN_CAP_BASE);
        cswatch_program_init(s_pio, s_cswatch_sm, s_cswatch_offset, PIN_CAP_BASE);
        pio_interrupt_clear(s_pio, CSWATCH_IRQ);
        chan_start(&s_chan[SAMPLER_SCK8]);
        mask |= (1u << s_chan[SAMPLER_SCK8].sm) | (1u << s_cswatch_sm);
    }
    // Both from the same clk_sys cycle, so SCK8 byte k and the RAW16 samples
    // around it describe the same moment of the same bus.
    pio_enable_sm_mask_in_sync(s_pio, mask);

    *rate_num = clock_get_hz(clk_sys);
    *rate_den = clkdiv;
}

// Samples in the ring right now. The active channel's remaining count and the
// halves counter can change between reads; retry until both are stable.
static uint64_t produced_now(const chan_t *ch) {
    for (;;) {
        uint32_t halves = ch->halves_done;
        int active = halves & 1;             // halves alternate 0, 1, 0, ...
        uint32_t remaining = dma_channel_hw_addr(ch->dma[active])->transfer_count;
        if (halves == ch->halves_done) {
            uint64_t xfers = (uint64_t)halves * ch->half_xfers + (ch->half_xfers - remaining);
            return xfers * ch->xfer_bytes / ch->sample_bytes;
        }
    }
}

static void chan_stop(chan_t *ch) {
    if (!ch->running) return;
    pio_sm_set_enabled(s_pio, ch->sm, false);
    // Let the DMA take the last words out of the RX FIFO.
    while (!pio_sm_is_rx_fifo_empty(s_pio, ch->sm)) tight_loop_contents();
    busy_wait_us(2);
    ch->final = produced_now(ch);
    for (uint i = 0; i < 2; i++) {
        dma_channel_set_irq1_enabled(ch->dma[i], false);
        dma_channel_abort(ch->dma[i]);
        dma_channel_acknowledge_irq1(ch->dma[i]);
    }
    ch->running = false;
}

void sampler_stop(void) {
    // Both in the same cycle, for the same reason they start together.
    pio_set_sm_mask_enabled(s_pio, (1u << s_chan[SAMPLER_RAW16].sm) |
                                   (1u << s_chan[SAMPLER_SCK8].sm) | (1u << s_cswatch_sm), false);
    for (uint c = 0; c < SAMPLER_COUNT; c++) chan_stop(&s_chan[c]);
}

uint32_t sampler_ring_samples(sampler_id_t id) { return s_chan[id].ring_samples; }

uint64_t sampler_produced(sampler_id_t id) {
    const chan_t *ch = &s_chan[id];
    return ch->running ? produced_now(ch) : ch->final;
}

// Two memcpys at most: at the FX2 sink's rates the CPU has a handful of
// cycles per sample, and a per-sample modulo does not fit.
void sampler_copy(sampler_id_t id, void *dst, uint64_t index, uint32_t n) {
    const chan_t *ch = &s_chan[id];
    uint32_t sb    = ch->sample_bytes;
    uint32_t at    = (uint32_t)(index % ch->ring_samples);
    uint32_t first = ch->ring_samples - at;
    if (first > n) first = n;
    memcpy(dst, ch->ring + at * sb, first * sb);
    memcpy((uint8_t *)dst + first * sb, ch->ring, (n - first) * sb);
}
