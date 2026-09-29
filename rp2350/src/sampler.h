// PIO + DMA samplers of the CF expansion signals.
//
//   RAW16  the 16 CF signals at a fixed rate, clk_sys / integer (stage 3)
//   SCK8   GP16-23 (CS candidates IO_1-4, MISO, OW, SCK, MOSI) once per
//          rising SCK edge: exact SPI bits at any SCK speed (stage 4)
//
// Each lands in its own RAM ring; the capture engine copies them out into
// blocks. Sample indices are 64-bit and exact: a ring is two DMA halves
// chained to each other, and an IRQ counts completed halves, so a stalled main
// loop can lose data (reported as an overrun) but never miscount it.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    SAMPLER_RAW16,
    SAMPLER_SCK8,
    SAMPLER_COUNT,
} sampler_id_t;

#define SAMPLER_RAW16_RING_SAMPLES (64 * 1024)    // 128 KB of u16
#define SAMPLER_SCK8_RING_SAMPLES  (128 * 1024)   // 128 KB of u8: ~6 ms at 21 MHz SCK

void sampler_init(void);

// Start the fixed-rate sampler at clk_sys / clkdiv, and optionally the SCK
// sampler. Returns the exact fixed rate as num/den.
void sampler_start(uint32_t clkdiv, bool sck8, uint32_t *rate_num, uint32_t *rate_den);

// Stop both.
void sampler_stop(void);

uint32_t sampler_ring_samples(sampler_id_t id);

// Samples written into that ring since start. Stops advancing after stop.
uint64_t sampler_produced(sampler_id_t id);

// Copy samples [index, index+n) out of a ring. The caller must check with
// sampler_produced() afterwards that they were not overwritten meanwhile.
void sampler_copy(sampler_id_t id, void *dst, uint64_t index, uint32_t n);
