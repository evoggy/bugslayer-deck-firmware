#include <string.h>

#include "hardware/clocks.h"
#include "pico/rand.h"
#include "pico/stdlib.h"
#include "pico/unique_id.h"

#include "block.h"
#include "capture.h"
#include "fx2_link.h"
#include "sampler.h"
#include "usb_stream.h"

#define FW_VERSION "0.3.0"

#define STREAM_ID          0
#define SAMPLE_BYTES       2
#define SAMPLES_PER_BLOCK  (BLOCK_PAYLOAD_MAX / SAMPLE_BYTES)   // 242

// The synthetic source models the capture ring the pin source really has:
// sampled data waits there until the sink takes it, and when the sink falls
// further behind than the ring, the oldest samples are lost.
#define COUNTER_RING_SAMPLES (64 * 1024 / SAMPLE_BYTES)

// The pin source's ring is real; leave headroom for the samples the DMA writes
// while a block is being copied out.
#define PINS_RING_SAMPLES    (SAMPLER_RING_SAMPLES - 4096)

// Cap on blocks emitted per poll, so tud_task() and the console keep running.
// The FX2 sink needs ~70k blocks/s at full rate, so it gets a bigger share.
#define BLOCKS_PER_POLL_USB 8
#define BLOCKS_PER_POLL_FX2 32

#define RATE_MIN_HZ        2300        // clkdiv <= 65535 at 150 MHz
#define RATE_MAX_USB_HZ    2000000     // well past what Full Speed carries
#define SAMPLER_MIN_CLKDIV 8           // FX2: 150 MHz / 8 = 18.75 Msps, 37.5 MB/s
                                       // raw -- the most the link could carry

// A draining session whose sink takes nothing for this long is abandoned.
#define STUCK_US           1000000

// FX2: after this long with nothing new to send, end the host's bulk transfer
// with a zero-length packet so the blocks already sent are delivered now.
#define FX2_FLUSH_IDLE_US  1000

enum state {
    IDLE,
    ARMED,
    DRAINING,        // sampling stopped; sending what is left, then END
    FLUSHING,        // FX2: END queued; waiting for the engine to send it
};

static enum state s_state;
static capture_source_t s_source;
static capture_sink_t s_sink;
static bool       s_aborted;
static uint64_t   s_progress_us;      // last time the sink took a block
static uint32_t   s_flush_mark;       // FX2: blocks put as of the last PKTEND
static uint32_t   s_session;
static uint32_t   s_rate_hz;          // as requested
static uint32_t   s_rate_num;         // exact: rate = num / den
static uint32_t   s_rate_den;
static uint64_t   s_t0_us;
static uint64_t   s_stop_at;          // total samples, fixed at disarm
static uint64_t   s_next;             // next sample index to put in a block
static uint32_t   s_seq;
static uint32_t   s_overruns;
static uint64_t   s_lost;
static uint64_t   s_pending_first;    // loss not yet reported in an OVERRUN block
static uint64_t   s_pending_lost;
static bool       s_session_sent;

static block_t    s_usb_block;        // USB sink: built here, then queued to TinyUSB

// Where the next block is built, or NULL if the sink has no room. Calling it
// again before sink_put() returns the same block.
static block_t *sink_get(void) {
    if (s_sink == SINK_FX2) return fx2_sink_get();
    return usb_stream_ready() ? &s_usb_block : NULL;
}

static void sink_put(void) {
    if (s_sink == SINK_FX2) fx2_sink_put();
    else usb_stream_write(&s_usb_block);
    s_progress_us = time_us_64();
}

static uint64_t samples_due(void) {
    if (s_state != ARMED) return s_stop_at;
    if (s_source == CAPTURE_PINS) return sampler_produced();
    return (time_us_64() - s_t0_us) * s_rate_hz / 1000000u;
}

static uint64_t ring_samples(void) {
    return s_source == CAPTURE_PINS ? PINS_RING_SAMPLES : COUNTER_RING_SAMPLES;
}

// Fill in the header of a block whose payload is already in place, and send it.
static void emit(block_t *b, uint8_t type, uint64_t sample, uint16_t payload_len) {
    b->magic       = BLOCK_MAGIC;
    b->version     = BLOCK_VERSION;
    b->type        = type;
    b->stream      = (type == BLOCK_SAMPLES || type == BLOCK_OVERRUN) ? STREAM_ID : 0;
    b->flags       = 0;
    b->session     = s_session;
    b->seq         = s_seq++;
    b->sample      = sample;
    b->payload_len = payload_len;
    b->reserved    = 0;
    memset(b->payload + payload_len, 0, BLOCK_PAYLOAD_MAX - payload_len);
    sink_put();
}

// Each emit_* returns false, having changed nothing, if the sink has no room.

static bool emit_session(void) {
    block_t *b = sink_get();
    if (!b) return false;
    session_payload_t *p = (session_payload_t *)b->payload;
    memset(b->payload, 0, sizeof(b->payload));
    p->timebase_hz = clock_get_hz(clk_sys);
    p->arm_time_us = s_t0_us;
    strncpy(p->fw_version, FW_VERSION, sizeof(p->fw_version));
    char serial[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1];
    pico_get_unique_board_id_string(serial, sizeof(serial));
    memcpy(p->serial, serial, sizeof(p->serial));
    strncpy(p->hw, "rev-A", sizeof(p->hw));
    p->n_streams = 1;
    stream_desc_t *s = &p->streams[0];
    s->id        = STREAM_ID;
    s->encoding  = ENC_RAW16;
    s->first_pin = 16;           // GP16..31: every CF expansion signal
    s->n_pins    = 16;
    s->rate_num  = s_rate_num;
    s->rate_den  = s_rate_den;
    strncpy(s->source, s_source == CAPTURE_PINS ? "pins" : "counter", sizeof(s->source));
    emit(b, BLOCK_SESSION, 0, (uint16_t)(sizeof(*p) + sizeof(*s)));
    return true;
}

static void record_loss(uint64_t first, uint64_t n) {
    if (!s_pending_lost) s_pending_first = first;
    s_pending_lost += n;
    s_lost += n;
}

static bool emit_samples(uint32_t n) {
    block_t *b = sink_get();
    if (!b) return false;
    uint16_t *out = (uint16_t *)b->payload;
    if (s_source == CAPTURE_PINS) {
        sampler_copy(out, s_next, n);
        // Did the DMA lap these samples while we copied them? Then they are
        // not what was sampled: report them lost instead of sending them. The
        // block is not put, so the sink hands out the same one next time.
        if (sampler_produced() - s_next > SAMPLER_RING_SAMPLES) {
            record_loss(s_next, n);
            s_next += n;
            return true;
        }
    } else {
        for (uint32_t i = 0; i < n; i++) out[i] = (uint16_t)(s_next + i);   // value = index
    }
    emit(b, BLOCK_SAMPLES, s_next, (uint16_t)(n * SAMPLE_BYTES));
    s_next += n;
    return true;
}

static bool emit_overrun(void) {
    block_t *b = sink_get();
    if (!b) return false;
    overrun_payload_t *p = (overrun_payload_t *)b->payload;
    p->lost = s_pending_lost;
    emit(b, BLOCK_OVERRUN, s_pending_first, sizeof(*p));
    s_overruns++;
    s_pending_lost = 0;
    return true;
}

static bool emit_end(void) {
    block_t *b = sink_get();
    if (!b) return false;
    end_payload_t *p = (end_payload_t *)b->payload;
    p->blocks   = s_seq + 1;     // this END included
    p->overruns = s_overruns;
    p->lost     = s_lost;
    emit(b, BLOCK_END, s_stop_at, sizeof(*p));
    return true;
}

static void finish(bool aborted) {
    if (s_sink == SINK_FX2) fx2_sink_stop();
    s_aborted = aborted;
    s_state   = IDLE;
}

void capture_init(void) {
    sampler_init();
}

uint32_t capture_rate_max(capture_sink_t sink) {
    return sink == SINK_FX2 ? clock_get_hz(clk_sys) / SAMPLER_MIN_CLKDIV : RATE_MAX_USB_HZ;
}

capture_arm_result_t capture_arm(uint32_t rate_hz, capture_source_t source,
                                 capture_sink_t sink, uint32_t *session) {
    if (s_state != IDLE) return ARM_BUSY;
    if (rate_hz < RATE_MIN_HZ || rate_hz > capture_rate_max(sink)) return ARM_RATE;
    if (sink == SINK_FX2 && !fx2_sink_start()) return ARM_NO_FX2;
    // Random, so a stale block from before a reboot can never match.
    do s_session = get_rand_32(); while (s_session == 0);
    s_rate_hz       = rate_hz;
    s_seq           = 0;
    s_next          = 0;
    s_stop_at       = 0;
    s_overruns      = 0;
    s_lost          = 0;
    s_pending_lost  = 0;
    s_session_sent  = false;
    s_source        = source;
    s_sink          = sink;
    s_aborted       = false;
    s_t0_us         = time_us_64();
    s_progress_us   = s_t0_us;
    s_flush_mark    = 0;
    if (source == CAPTURE_PINS) {
        // Integer divider only, so samples are evenly spaced; the rate is
        // then exactly clk_sys / clkdiv, which is what SESSION reports.
        uint32_t clkdiv = (clock_get_hz(clk_sys) + rate_hz / 2) / rate_hz;
        sampler_start(clkdiv, &s_rate_num, &s_rate_den);
    } else {
        s_rate_num = rate_hz;
        s_rate_den = 1;
    }
    s_state         = ARMED;
    *session = s_session;
    return ARM_OK;
}

void capture_disarm(void) {
    if (s_state != ARMED) return;
    if (s_source == CAPTURE_PINS) sampler_stop();
    s_stop_at = samples_due();
    s_state   = DRAINING;
}

void capture_poll(void) {
    if (s_state == IDLE) return;

    // Nobody is reading: the FX2's host side is closed, so EP6 and our slots
    // stay full. While armed that is just loss, reported as OVERRUN when the
    // sink moves again; once sampling has stopped it would never finish.
    if (s_state != ARMED && time_us_64() - s_progress_us > STUCK_US) {
        finish(true);
        return;
    }

    // Quiet for a moment with blocks sent since the last flush: deliver them.
    if (s_sink == SINK_FX2 && fx2_sink_blocks() != s_flush_mark &&
        time_us_64() - s_progress_us > FX2_FLUSH_IDLE_US && fx2_sink_flush())
        s_flush_mark = fx2_sink_blocks();

    // END is out once the flush after it has happened.
    if (s_state == FLUSHING) {
        if (s_flush_mark == fx2_sink_blocks()) finish(false);
        return;
    }

    int budget = s_sink == SINK_FX2 ? BLOCKS_PER_POLL_FX2 : BLOCKS_PER_POLL_USB;
    for (int n = 0; n < budget; n++) {
        if (!s_session_sent) {
            if (!emit_session()) return;
            s_session_sent = true;
            continue;
        }

        uint64_t due = samples_due();

        // When the sink is more than a ring behind, the ring has overflowed.
        // Drop down to half full rather than just to full: loss then comes in
        // a few large, well-separated chunks, and what survives is long
        // contiguous runs instead of a sample-by-sample trickle with an
        // OVERRUN block eating the bandwidth after every block.
        if (due - s_next > ring_samples()) {
            uint64_t lost = due - s_next - ring_samples() / 2;
            record_loss(s_next, lost);
            s_next += lost;
        }

        if (s_pending_lost) {
            if (!emit_overrun()) return;
            continue;
        }

        uint64_t avail = due - s_next;
        if (avail >= SAMPLES_PER_BLOCK || (s_state == DRAINING && avail > 0)) {
            if (!emit_samples(avail < SAMPLES_PER_BLOCK ? (uint32_t)avail : SAMPLES_PER_BLOCK))
                return;
            continue;
        }

        if (s_state == DRAINING) {
            if (!emit_end()) return;
            // Blocks still queued to the FX2 need the engine until they are out.
            if (s_sink == SINK_FX2) s_state = FLUSHING;
            else finish(false);
        }
        return;
    }
}

void capture_status(capture_status_t *st) {
    st->armed    = s_state == ARMED;
    st->busy     = s_state != IDLE;
    st->session  = s_session;
    st->rate_hz  = s_rate_hz;
    st->source   = s_source;
    st->sink     = s_sink;
    st->aborted  = s_aborted;
    st->samples  = samples_due();
    st->blocks   = s_seq;
    st->overruns = s_overruns;
    st->lost     = s_lost;
}
