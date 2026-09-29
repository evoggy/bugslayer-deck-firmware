// Capture sessions: arm/disarm, and the block stream they produce.
//
// Stage 2 (docs/bringup-plan.md): one raw16 stream from a synthetic source
// whose sample n has the value (uint16_t)n, on a real sample clock, into the
// RP2350's own USB. The host can check every sample, and loss is real: if the
// sink falls behind a modelled capture buffer, samples are dropped and an
// OVERRUN block says exactly which.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Start a session. Returns false if already armed or the rate is out of range.
bool capture_arm(uint32_t rate_hz, uint32_t *session);

// Stop sampling. Samples already due are still sent, then an END block.
void capture_disarm(void);

// Drive the stream. Call from the main loop as often as possible.
void capture_poll(void);

typedef struct {
    bool     armed;         // sampling
    bool     busy;          // armed, or still draining to END
    uint32_t session;
    uint32_t rate_hz;
    uint64_t samples;       // sampled so far (at disarm: total)
    uint32_t blocks;        // emitted so far
    uint32_t overruns;
    uint64_t lost;
} capture_status_t;

void capture_status(capture_status_t *st);
