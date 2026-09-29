// Capture sessions: arm/disarm, and the block stream they produce.
//
// One raw16 stream from either the 16 CF signals (PIO + DMA ring, stage 3) or
// a synthetic source whose sample n has the value (uint16_t)n (stage 2), on
// the same sample clock model, into either sink: the RP2350's own USB or the
// FX2. Loss is real: if the sink falls behind the ring, samples are dropped
// and an OVERRUN block says exactly which.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    CAPTURE_COUNTER,     // synthetic: sample n = n mod 2^16 (stage 2)
    CAPTURE_PINS,        // the 16 CF signals, PIO + DMA (stage 3)
} capture_source_t;

typedef enum {
    SINK_USB,            // the RP2350's vendor bulk IN, Full Speed: ~380 ksps
    SINK_FX2,            // the FX2's EP6, High Speed: ~17 Msps
} capture_sink_t;

typedef enum {
    ARM_OK,
    ARM_BUSY,            // a session is armed or still draining
    ARM_RATE,            // rate out of range for this sink
    ARM_NO_FX2,          // FX2 link down, or its stage 1 test is running
} capture_arm_result_t;

void capture_init(void);

// Highest rate each sink accepts (the lowest is 2300 Hz for both).
uint32_t capture_rate_max(capture_sink_t sink);

// Start a session. The pin source rounds to clk_sys / integer divider; the
// exact rate is in the SESSION block.
capture_arm_result_t capture_arm(uint32_t rate_hz, capture_source_t source,
                                 capture_sink_t sink, uint32_t *session);

// Stop sampling. Samples already due are still sent, then an END block. If
// the sink makes no progress for a second (nobody reading the FX2), the
// session is abandoned without END and `aborted` is set in the status.
void capture_disarm(void);

// Drive the stream. Call from the main loop as often as possible.
void capture_poll(void);

typedef struct {
    bool     armed;         // sampling
    bool     busy;          // armed, or still draining to END
    uint32_t session;
    uint32_t rate_hz;
    capture_source_t source;
    capture_sink_t sink;
    bool     aborted;       // last session was abandoned without END
    uint64_t samples;       // sampled so far (at disarm: total)
    uint32_t blocks;        // emitted so far
    uint32_t overruns;
    uint64_t lost;
} capture_status_t;

void capture_status(capture_status_t *st);
