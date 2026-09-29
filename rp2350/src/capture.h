// Capture sessions: arm/disarm, and the block stream they produce.
//
// One raw16 stream into the RP2350's own USB, from either the 16 CF signals
// (PIO + DMA ring, stage 3) or a synthetic source whose sample n has the value
// (uint16_t)n (stage 2), on the same sample clock model. Loss is real: if the
// sink falls behind the ring, samples are dropped and an OVERRUN block says
// exactly which.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    CAPTURE_COUNTER,     // synthetic: sample n = n mod 2^16 (stage 2)
    CAPTURE_PINS,        // the 16 CF signals, PIO + DMA (stage 3)
} capture_source_t;

void capture_init(void);

// Start a session. Returns false if already armed or the rate is out of range
// (2300..2000000 Hz). The pin source rounds to clk_sys / integer divider; the
// exact rate is in the SESSION block.
bool capture_arm(uint32_t rate_hz, capture_source_t source, uint32_t *session);

// Stop sampling. Samples already due are still sent, then an END block.
void capture_disarm(void);

// Drive the stream. Call from the main loop as often as possible.
void capture_poll(void);

typedef struct {
    bool     armed;         // sampling
    bool     busy;          // armed, or still draining to END
    uint32_t session;
    uint32_t rate_hz;
    capture_source_t source;
    uint64_t samples;       // sampled so far (at disarm: total)
    uint32_t blocks;        // emitted so far
    uint32_t overruns;
    uint64_t lost;
} capture_status_t;

void capture_status(capture_status_t *st);
