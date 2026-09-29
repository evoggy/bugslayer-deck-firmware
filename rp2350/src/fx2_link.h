// FX2 slave FIFO link: IFCLK generation, reset sequencing, and a synthetic
// counter source for the stage 1 pipe test (docs/bringup-plan.md).
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Loads the PIO program and parks the FX2 in reset. Call once at boot.
void fx2_link_init(void);

// Start IFCLK and release FX_RESET#. IFCLK must already be running when the
// FX2's firmware touches its FIFO registers, so the order matters.
void fx2_link_up(void);

// Assert FX_RESET# and stop the clock.
void fx2_link_down(void);
bool fx2_link_is_up(void);

// Requested IFCLK in Hz; returns what was actually achieved after clkdiv
// rounding. The PIO program takes 8 clk_sys cycles per IFCLK period.
uint32_t fx2_link_set_ifclk(uint32_t hz);
uint32_t fx2_link_get_ifclk(void);

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
