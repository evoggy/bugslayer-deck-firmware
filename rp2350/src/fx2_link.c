#include <stdio.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/structs/pads_bank0.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

#include "board.h"
#include "fx2_link.h"
#include "fx2_write.pio.h"

#define PIO_CYCLES_PER_IFCLK 4

// FX2 IFCLK limits: tIFCLK 20.83..200 ns.
#define IFCLK_MAX_HZ 48000000u
#define IFCLK_MIN_HZ  5000000u

// Two 8 KiB buffers ping-pong through the PIO TX FIFO. At 37.5 MB/s each
// buffer lasts ~218 us; refilling one takes a small fraction of that.
#define BUF_WORDS 2048

static PIO      s_pio;
static uint     s_sm;
static uint     s_offset;
static bool     s_up;
static uint32_t s_ifclk_hz;

// Block sink: a ring of slots. The producer (capture_poll) fills slot
// s_head % SLOTS; the DMA sends runs of up to RUN_MAX contiguous queued slots
// and the IRQ frees each run as it completes. 32 KB is ~0.9 ms at 37.5 MB/s --
// the sampler's ring, not this one, is what absorbs the host's hiccups.
#define SLOTS    64
#define RUN_MAX  16
#define BLOCK_WORDS (BLOCK_SIZE / 4)

static block_t  s_slots[SLOTS];
static volatile uint32_t s_head;       // slots put
static volatile uint32_t s_tail;       // slots sent
static volatile uint32_t s_run;        // slots in the DMA run in flight; 0 = idle
static bool     s_sink;

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

// Start the next run of queued slots, if the DMA is idle and there are any.
// Called from the IRQ, or with it masked.
static void sink_kick(void) {
    if (s_run || !s_sink) return;
    uint32_t ready = s_head - s_tail;
    if (!ready) return;
    uint32_t idx = s_tail % SLOTS;
    uint32_t n = ready;
    if (n > SLOTS - idx) n = SLOTS - idx;   // contiguous up to the wrap
    if (n > RUN_MAX) n = RUN_MAX;          // free slots in smaller steps
    s_run = n;
    dma_channel_transfer_from_buffer_now(s_dma[0], &s_slots[idx], n * BLOCK_WORDS);
}

// Counter test: refill whichever buffer just drained; its partner is streaming
// right now. Block sink: free the finished run and start the next.
static void __isr dma_irq_handler(void) {
    if (s_sink) {
        if (dma_channel_get_irq0_status(s_dma[0])) {
            dma_channel_acknowledge_irq0(s_dma[0]);
            s_tail += s_run;
            s_run = 0;
            sink_kick();
        }
        return;
    }
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
    if (hz > IFCLK_MAX_HZ) hz = IFCLK_MAX_HZ;
    if (hz < IFCLK_MIN_HZ) hz = IFCLK_MIN_HZ;
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
    fx2_sink_stop();
    gpio_put(PIN_FX_RESET_N, 0);
    pio_sm_set_enabled(s_pio, s_sm, false);
    s_up = false;
}

bool fx2_link_is_up(void) { return s_up; }

// Empty TX FIFO and OSR, then run from `entry` with the strobes released.
// Anything left over from a previous run would go out first, and a partial
// word shifts every word after it. Stopping the SM pauses IFCLK for a few
// cycles, which is harmless once the FX2 has configured itself.
static void engine_restart(uint entry) {
    pio_sm_set_enabled(s_pio, s_sm, false);
    pio_sm_clear_fifos(s_pio, s_sm);
    pio_sm_restart(s_pio, s_sm);
    jump_to(entry);
    pio_sm_exec(s_pio, s_sm, pio_encode_set(pio_pins, 0b11));
    pio_sm_set_enabled(s_pio, s_sm, true);
}

// The engine has written out everything queued to it: it is waiting in the
// `stall` loop, which it only enters between words, and the TX FIFO is empty.
// With nothing refilling the FIFO that state is stable. (It is also where the
// engine waits for EP6 room, but then the FIFO still holds data.)
static bool in_stall_loop(void) {
    uint pc = pio_sm_get_pc(s_pio, s_sm) - s_offset;
    return pc >= fx2_write_offset_stall && pc <= fx2_write_offset_stall + 4;
}

static bool engine_empty_now(void) {
    return pio_sm_is_tx_fifo_empty(s_pio, s_sm) && in_stall_loop();
}

// False if the engine did not empty within timeout_ms (EP6 full and the host
// not reading).
static bool engine_wait_empty(uint32_t timeout_ms) {
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (!engine_empty_now())
        if (time_reached(deadline)) return false;
    return true;
}

bool fx2_sink_start(void) {
    if (s_sink) return true;
    if (s_running || !s_up) return false;

    dma_channel_config c = dma_channel_get_default_config(s_dma[0]);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(s_pio, s_sm, true));
    dma_channel_configure(s_dma[0], &c, &s_pio->txf[s_sm], s_slots, 0, false);
    dma_channel_acknowledge_irq0(s_dma[0]);
    dma_channel_set_irq0_enabled(s_dma[0], true);

    s_head = s_tail = s_run = 0;
    s_sink = true;
    // PKTEND first: commit any partial packet a previous run left in EP6, so
    // this session starts on a packet boundary. The engine then waits in
    // `stall` for the first block.
    engine_restart(fx2_write_offset_pktend);
    return true;
}

void fx2_sink_stop(void) {
    if (!s_sink) return;
    dma_channel_set_irq0_enabled(s_dma[0], false);
    s_sink = false;
    dma_channel_abort(s_dma[0]);
    dma_channel_acknowledge_irq0(s_dma[0]);
    s_head = s_tail = s_run = 0;
    engine_restart(fx2_write_offset_clk_only);
}

block_t *fx2_sink_get(void) {
    if (!s_sink || s_head - s_tail >= SLOTS) return NULL;
    return &s_slots[s_head % SLOTS];
}

void fx2_sink_put(void) {
    uint32_t irq = save_and_disable_interrupts();
    s_head++;
    sink_kick();
    restore_interrupts(irq);
}

bool fx2_sink_drained(void) {
    return !s_sink || (s_head == s_tail && !s_run && engine_empty_now());
}

bool fx2_sink_flush(void) {
    uint32_t irq = save_and_disable_interrupts();
    // With the DMA idle and IRQs masked nothing new can reach the engine, so
    // once it is empty it stays in `stall` with every byte latched, and
    // leaving for `pktend` cannot split a word. PKTEND needs a free EP6 buffer
    // to commit into, so only flush when FLAGB says there is room.
    bool ok = s_sink && s_head == s_tail && !s_run && engine_empty_now() &&
              gpio_get(PIN_FX_FLAGB);
    if (ok) jump_to(fx2_write_offset_pktend);
    restore_interrupts(irq);
    if (ok) busy_wait_us(1);            // let the strobe out before anyone restarts the SM
    return ok;
}

bool fx2_sink_running(void) { return s_sink; }

uint32_t fx2_sink_blocks(void) { return s_head; }

void fx2_counter_start(void) {
    if (s_running || s_sink || !s_up) return;

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

    engine_restart(fx2_write_offset_stall);   // always re-enter with SLWR# deasserted

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
    // whole words. If EP6 is full and nobody is reading, that never happens;
    // give up after 100 ms and let the next start discard the rest. A partial
    // packet left in EP6 is flushed by the block sink's PKTEND at its start.
    s_flushed = engine_wait_empty(100);
    jump_to(fx2_write_offset_clk_only);
}

bool fx2_counter_running(void) { return s_running; }

bool fx2_counter_flushed(void) { return s_flushed; }

uint64_t fx2_counter_words(void) { return s_words_total; }

void fx2_link_debug(void) {
    uint pc = pio_sm_get_pc(s_pio, s_sm) - s_offset;
    printf("dbg room=%d iso=%d pc=%u (clk_only=%u stall=%u) txlevel=%u "
           "dma0=%lu dma1=%lu busy=%d/%d\n",
           gpio_get(PIN_FX_FLAGB),
           (int)!!(pads_bank0_hw->io[PIN_FX_FLAGB] & PADS_BANK0_GPIO0_ISO_BITS),
           pc, fx2_write_offset_clk_only, fx2_write_offset_stall,
           pio_sm_get_tx_fifo_level(s_pio, s_sm),
           (unsigned long)dma_channel_hw_addr(s_dma[0])->transfer_count,
           (unsigned long)dma_channel_hw_addr(s_dma[1])->transfer_count,
           dma_channel_is_busy(s_dma[0]), dma_channel_is_busy(s_dma[1]));
}

// Where does the write engine spend its time? Samples the SM's PC and the TX
// FIFO. `stall` = waiting with data queued: FLAGB says EP6 has no room (the
// USB side is the bottleneck); `starved` = waiting with the FIFO empty (the
// RP2350 side is the bottleneck); `write` = streaming.
void fx2_link_profile(uint32_t samples) {
    uint32_t n_write = 0, n_stall = 0, n_other = 0, n_starved = 0;
    for (uint32_t i = 0; i < samples; i++) {
        busy_wait_at_least_cycles(40);   // ~2.5 IFCLKs at clkdiv 1
        bool empty = pio_sm_is_tx_fifo_empty(s_pio, s_sm);
        uint pc = pio_sm_get_pc(s_pio, s_sm) - s_offset;
        if (pc >= fx2_write_offset_stall && pc <= fx2_write_offset_stall + 4) {
            if (empty) n_starved++;
            else n_stall++;
        } else if (pc >= fx2_write_offset_word - 1 && pc < fx2_write_offset_stall) {
            n_write++;
        } else {
            n_other++;
        }
    }
    printf("prof samples=%lu write=%lu%% stall=%lu%% starved=%lu%% other=%lu%%\n",
           (unsigned long)samples,
           (unsigned long)(100ull * n_write / samples), (unsigned long)(100ull * n_stall / samples),
           (unsigned long)(100ull * n_starved / samples), (unsigned long)(100ull * n_other / samples));
}
