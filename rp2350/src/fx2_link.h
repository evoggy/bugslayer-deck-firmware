// FX2 slave FIFO link: IFCLK generation, reset sequencing, the capture block
// sink, and a synthetic counter source for the stage 1 pipe test
// (docs/bringup-plan.md).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "block.h"

// Loads the PIO program and parks the FX2 in reset. Call once at boot.
void fx2_link_init(void);

// Start IFCLK and release FX_RESET#. IFCLK must already be running when the
// FX2's firmware touches its FIFO registers, so the order matters.
void fx2_link_up(void);

// Assert FX_RESET# and stop the clock.
void fx2_link_down(void);
bool fx2_link_is_up(void);

// Requested IFCLK in Hz; returns what was actually achieved after clkdiv
// rounding, clamped to the FX2's 5..48 MHz. The PIO program takes 4 clk_sys
// cycles per IFCLK period, so clkdiv 1 at 150 MHz is 37.5 MHz.
uint32_t fx2_link_set_ifclk(uint32_t hz);
uint32_t fx2_link_get_ifclk(void);

// Block sink: capture blocks through EP6, whole 512-byte blocks only, so with
// AUTOIN every block is exactly one USB packet. Blocks are built in place in a
// ring of slots and a DMA channel feeds them to the write engine. The sink and
// the counter test share the engine; each refuses to start while the other runs.

// Flush any partial packet out of EP6 (PKTEND) and start the engine. False if
// the link is down or the counter test is running.
bool fx2_sink_start(void);

// Stop at once, dropping whatever is still queued. A partial block may be left
// in EP6; the next start flushes it as a short packet the host discards.
void fx2_sink_stop(void);

// A free slot to build the next block in, or NULL if all are queued. Calling
// it again before fx2_sink_put() returns the same slot.
block_t *fx2_sink_get(void);

// Queue the slot from fx2_sink_get().
void fx2_sink_put(void);

// Every queued block has left the RP2350: DMA done, PIO FIFO and OSR empty.
// (The last packets can still be in EP6, waiting for the host.)
bool fx2_sink_drained(void);

// If drained, pulse PKTEND: EP6 is on a packet boundary, so this commits a
// zero-length packet, which completes the host's pending bulk transfer instead
// of leaving the last blocks waiting in it for more data. Returns false (and
// does nothing) if not drained.
bool fx2_sink_flush(void);

bool fx2_sink_running(void);
uint32_t fx2_sink_blocks(void);        // blocks put since start

// Stream a 32-bit little-endian incrementing counter into the FX2 until
// stopped. Loss is then quantifiable on the host, not merely detectable.
void fx2_counter_start(void);
void fx2_counter_stop(void);
bool fx2_counter_running(void);

// False if the last stop timed out with words still queued (EP6 full and the
// host not reading). The next start discards them.
bool fx2_counter_flushed(void);

// Words handed to the DMA since the last start.
uint64_t fx2_counter_words(void);

// One line of link internals for bring-up: FLAGB, PIO PC, FIFO level, DMA.
void fx2_link_debug(void);

// Sample the write engine's state: streaming vs flow-controlled vs starved.
void fx2_link_profile(uint32_t samples);
