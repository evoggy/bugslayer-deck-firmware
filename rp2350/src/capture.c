#include <string.h>

#include "hardware/clocks.h"
#include "pico/rand.h"
#include "pico/stdlib.h"
#include "pico/unique_id.h"

#include "block.h"
#include "capture.h"
#include "sampler.h"
#include "usb_stream.h"

#define FW_VERSION "0.2.0"

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
#define BLOCKS_PER_POLL    8

#define RATE_MIN_HZ        2300        // clkdiv <= 65535 at 150 MHz
#define RATE_MAX_HZ        2000000

enum state { IDLE, ARMED, DRAINING };

static enum state s_state;
static capture_source_t s_source;
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

static block_t    s_block;

static uint64_t samples_due(void) {
    if (s_state != ARMED) return s_stop_at;
    if (s_source == CAPTURE_PINS) return sampler_produced();
    return (time_us_64() - s_t0_us) * s_rate_hz / 1000000u;
}

static uint64_t ring_samples(void) {
    return s_source == CAPTURE_PINS ? PINS_RING_SAMPLES : COUNTER_RING_SAMPLES;
}

static void emit(uint8_t type, uint64_t sample, uint16_t payload_len) {
    s_block.magic       = BLOCK_MAGIC;
    s_block.version     = BLOCK_VERSION;
    s_block.type        = type;
    s_block.stream      = (type == BLOCK_SAMPLES || type == BLOCK_OVERRUN) ? STREAM_ID : 0;
    s_block.flags       = 0;
    s_block.session     = s_session;
    s_block.seq         = s_seq++;
    s_block.sample      = sample;
    s_block.payload_len = payload_len;
    s_block.reserved    = 0;
    memset(s_block.payload + payload_len, 0, BLOCK_PAYLOAD_MAX - payload_len);
    usb_stream_write(&s_block);
}

static void emit_session(void) {
    session_payload_t *p = (session_payload_t *)s_block.payload;
    memset(s_block.payload, 0, sizeof(s_block.payload));
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
    emit(BLOCK_SESSION, 0, (uint16_t)(sizeof(*p) + sizeof(*s)));
}

static void record_loss(uint64_t first, uint64_t n) {
    if (!s_pending_lost) s_pending_first = first;
    s_pending_lost += n;
    s_lost += n;
}

static void emit_samples(uint32_t n) {
    uint16_t *out = (uint16_t *)s_block.payload;
    if (s_source == CAPTURE_PINS) {
        sampler_copy(out, s_next, n);
        // Did the DMA lap these samples while we copied them? Then they are
        // not what was sampled: report them lost instead of sending them.
        if (sampler_produced() - s_next > SAMPLER_RING_SAMPLES) {
            record_loss(s_next, n);
            s_next += n;
            return;
        }
    } else {
        for (uint32_t i = 0; i < n; i++) out[i] = (uint16_t)(s_next + i);   // value = index
    }
    emit(BLOCK_SAMPLES, s_next, (uint16_t)(n * SAMPLE_BYTES));
    s_next += n;
}

static void emit_overrun(void) {
    overrun_payload_t *p = (overrun_payload_t *)s_block.payload;
    p->lost = s_pending_lost;
    emit(BLOCK_OVERRUN, s_pending_first, sizeof(*p));
    s_overruns++;
    s_pending_lost = 0;
}

static void emit_end(void) {
    end_payload_t *p = (end_payload_t *)s_block.payload;
    p->blocks   = s_seq + 1;     // this END included
    p->overruns = s_overruns;
    p->lost     = s_lost;
    emit(BLOCK_END, s_stop_at, sizeof(*p));
}

void capture_init(void) {
    sampler_init();
}

bool capture_arm(uint32_t rate_hz, capture_source_t source, uint32_t *session) {
    if (s_state != IDLE || rate_hz < RATE_MIN_HZ || rate_hz > RATE_MAX_HZ) return false;
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
    s_t0_us         = time_us_64();
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
    return true;
}

void capture_disarm(void) {
    if (s_state != ARMED) return;
    if (s_source == CAPTURE_PINS) sampler_stop();
    s_stop_at = samples_due();
    s_state   = DRAINING;
}

void capture_poll(void) {
    if (s_state == IDLE) return;

    for (int n = 0; n < BLOCKS_PER_POLL; n++) {
        if (!s_session_sent) {
            if (!usb_stream_ready()) return;
            emit_session();
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
            if (!usb_stream_ready()) return;
            emit_overrun();
            continue;
        }

        uint64_t avail = due - s_next;
        if (avail >= SAMPLES_PER_BLOCK || (s_state == DRAINING && avail > 0)) {
            if (!usb_stream_ready()) return;
            emit_samples(avail < SAMPLES_PER_BLOCK ? (uint32_t)avail : SAMPLES_PER_BLOCK);
            continue;
        }

        if (s_state == DRAINING) {
            if (!usb_stream_ready()) return;
            emit_end();
            s_state = IDLE;
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
    st->samples  = samples_due();
    st->blocks   = s_seq;
    st->overruns = s_overruns;
    st->lost     = s_lost;
}
