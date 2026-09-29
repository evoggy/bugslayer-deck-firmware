// PIO + DMA sampler for the 16 CF expansion signals (stage 3).
//
// Samples land in a RAM ring; the capture engine copies them out into blocks.
// Sample indices are 64-bit and exact: the ring is two DMA halves chained to
// each other, and an IRQ counts completed halves, so a stalled main loop can
// lose data (reported as an overrun) but never miscount it.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SAMPLER_RING_SAMPLES (64 * 1024)   // 128 KB of u16

void sampler_init(void);

// Start sampling at clk_sys / clkdiv. Returns the exact rate as num/den.
void sampler_start(uint32_t clkdiv, uint32_t *rate_num, uint32_t *rate_den);
void sampler_stop(void);

// Samples written into the ring since start. Stops advancing after stop.
uint64_t sampler_produced(void);

// Copy samples [index, index+n) out of the ring. The caller must check with
// sampler_produced() before and after that they were not overwritten meanwhile.
void sampler_copy(uint16_t *dst, uint64_t index, uint32_t n);
