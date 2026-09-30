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

// Stream 0 is always the fixed-rate raw16 stream; stream 1, with `spi`, is the
// SCK-clocked byte stream.
#define N_STREAMS_MAX      2
#define STREAM_RAW16       0
#define STREAM_SCK8        1

// The synthetic source models the capture ring the pin source really has:
// sampled data waits there until the sink takes it, and when the sink falls
// further behind than the ring, the oldest samples are lost.
#define COUNTER_RING_SAMPLES (64 * 1024 / 2)

// The real rings: leave headroom for what the DMA writes while a block is
// being copied out. At 16.67 Msps raw16, 4096 samples is 245 us; at 21 MHz
// SCK, 8192 edges is 390 us.
#define RAW16_RING_SAMPLES (SAMPLER_RAW16_RING_SAMPLES - 4096)
#define SCK8_RING_SAMPLES  (SAMPLER_SCK8_RING_SAMPLES - 8192)

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

// One stream of a session: what it carries, and how far the sink has got.
typedef struct {
    uint8_t  encoding;
    uint8_t  n_pins;
    uint8_t  sample_bytes;
    uint32_t per_block;       // samples in a full SAMPLES block
    uint32_t ring;            // samples the source can hold before losing any
    uint32_t rate_num;        // exact: rate = num / den; 0/0 for event streams
    uint32_t rate_den;
    const char *source;
    uint64_t stop_at;         // total samples, fixed at disarm
    uint64_t next;            // next sample index to put in a block
    uint64_t pending_first;   // loss not yet reported in an OVERRUN block
    uint64_t pending_lost;
} stream_t;

static enum state s_state;
static capture_source_t s_source;
static capture_sink_t s_sink;
static bool       s_aborted;
static uint64_t   s_progress_us;      // last time the sink took a block
static uint32_t   s_flush_mark;       // FX2: blocks put as of the last PKTEND
static uint32_t   s_session;
static uint32_t   s_rate_hz;          // as requested
static uint64_t   s_t0_us;
static uint32_t   s_seq;
static uint32_t   s_overruns;
static uint64_t   s_lost;
static bool       s_session_sent;
static uint8_t    s_n_streams;
static stream_t   s_streams[N_STREAMS_MAX];

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

static sampler_id_t sampler_of(uint8_t id) {
    return id == STREAM_RAW16 ? SAMPLER_RAW16 : SAMPLER_SCK8;
}

static uint64_t produced(uint8_t id) {
    if (s_source == CAPTURE_COUNTER)
        return (time_us_64() - s_t0_us) * s_rate_hz / 1000000u;
    return sampler_produced(sampler_of(id));
}

static uint64_t samples_due(uint8_t id) {
    return s_state == ARMED ? produced(id) : s_streams[id].stop_at;
}

// Fill in the header of a block whose payload is already in place, and send it.
static void emit(block_t *b, uint8_t type, uint8_t stream, uint64_t sample,
                 uint16_t payload_len) {
    b->magic       = BLOCK_MAGIC;
    b->version     = BLOCK_VERSION;
    b->type        = type;
    b->stream      = stream;
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
    p->n_streams = s_n_streams;
    for (uint8_t i = 0; i < s_n_streams; i++) {
        stream_desc_t *d = &p->streams[i];
        d->id        = i;
        d->encoding  = s_streams[i].encoding;
        d->first_pin = 16;           // GP16..: every CF expansion signal
        d->n_pins    = s_streams[i].n_pins;
        d->rate_num  = s_streams[i].rate_num;
        d->rate_den  = s_streams[i].rate_den;
        strncpy(d->source, s_streams[i].source, sizeof(d->source));
    }
    emit(b, BLOCK_SESSION, 0, 0,
         (uint16_t)(sizeof(*p) + s_n_streams * sizeof(stream_desc_t)));
    return true;
}

static void record_loss(stream_t *st, uint64_t first, uint64_t n) {
    if (!st->pending_lost) st->pending_first = first;
    st->pending_lost += n;
    s_lost += n;
}

static bool emit_samples(uint8_t id, uint32_t n) {
    stream_t *st = &s_streams[id];
    block_t *b = sink_get();
    if (!b) return false;
    if (s_source == CAPTURE_PINS) {
        sampler_id_t sid = sampler_of(id);
        sampler_copy(sid, b->payload, st->next, n);
        // Did the DMA lap these samples while we copied them? Then they are
        // not what was sampled: report them lost instead of sending them. The
        // block is not put, so the sink hands out the same one next time.
        if (sampler_produced(sid) - st->next > sampler_ring_samples(sid)) {
            record_loss(st, st->next, n);
            st->next += n;
            return true;
        }
    } else {
        uint16_t *out = (uint16_t *)b->payload;
        for (uint32_t i = 0; i < n; i++) out[i] = (uint16_t)(st->next + i);   // value = index
    }
    emit(b, BLOCK_SAMPLES, id, st->next, (uint16_t)(n * st->sample_bytes));
    st->next += n;
    return true;
}

static bool emit_overrun(uint8_t id) {
    stream_t *st = &s_streams[id];
    block_t *b = sink_get();
    if (!b) return false;
    overrun_payload_t *p = (overrun_payload_t *)b->payload;
    p->lost = st->pending_lost;
    emit(b, BLOCK_OVERRUN, id, st->pending_first, sizeof(*p));
    s_overruns++;
    st->pending_lost = 0;
    return true;
}

static bool emit_end(void) {
    block_t *b = sink_get();
    if (!b) return false;
    end_payload_t *p = (end_payload_t *)b->payload;
    memset(p, 0, sizeof(*p));
    p->blocks    = s_seq + 1;     // this END included
    p->overruns  = s_overruns;
    p->lost      = s_lost;
    p->n_streams = s_n_streams;
    for (uint8_t i = 0; i < s_n_streams; i++) p->samples[i] = s_streams[i].stop_at;
    emit(b, BLOCK_END, 0, s_streams[STREAM_RAW16].stop_at,
         (uint16_t)(sizeof(*p) + s_n_streams * sizeof(uint64_t)));
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
                                 capture_sink_t sink, bool spi, uint32_t *session) {
    if (s_state != IDLE) return ARM_BUSY;
    if (spi && source != CAPTURE_PINS) return ARM_USAGE;
    if (rate_hz < RATE_MIN_HZ || rate_hz > capture_rate_max(sink)) return ARM_RATE;
    if (sink == SINK_FX2 && !fx2_sink_start()) return ARM_NO_FX2;

    // Random, so a stale block from before a reboot can never match.
    do s_session = get_rand_32(); while (s_session == 0);
    s_rate_hz       = rate_hz;
    s_seq           = 0;
    s_overruns      = 0;
    s_lost          = 0;
    s_session_sent  = false;
    s_source        = source;
    s_sink          = sink;
    s_aborted       = false;
    s_flush_mark    = 0;
    s_n_streams     = spi ? 2 : 1;
    memset(s_streams, 0, sizeof(s_streams));

    stream_t *raw = &s_streams[STREAM_RAW16];
    raw->encoding     = ENC_RAW16;
    raw->n_pins       = 16;          // GP16..31
    raw->sample_bytes = 2;
    raw->per_block    = BLOCK_PAYLOAD_MAX / 2;     // 242
    raw->source       = source == CAPTURE_PINS ? "pins" : "counter";
    raw->ring         = source == CAPTURE_PINS ? RAW16_RING_SAMPLES : COUNTER_RING_SAMPLES;
    if (spi) {
        stream_t *sck = &s_streams[STREAM_SCK8];
        sck->encoding     = ENC_SCK8;
        sck->n_pins       = 8;       // GP16..23: IO_1-4, MISO, OW, SCK, MOSI
        sck->sample_bytes = 1;
        sck->per_block    = BLOCK_PAYLOAD_MAX;     // 484
        sck->source       = "sck";
        sck->ring         = SCK8_RING_SAMPLES;
    }

    s_t0_us       = time_us_64();
    s_progress_us = s_t0_us;
    if (source == CAPTURE_PINS) {
        // Integer divider only, so samples are evenly spaced; the rate is
        // then exactly clk_sys / clkdiv, which is what SESSION reports.
        uint32_t clkdiv = (clock_get_hz(clk_sys) + rate_hz / 2) / rate_hz;
        sampler_start(clkdiv, spi, &raw->rate_num, &raw->rate_den);
    } else {
        raw->rate_num = rate_hz;
        raw->rate_den = 1;
    }
    s_state  = ARMED;
    *session = s_session;
    return ARM_OK;
}

void capture_disarm(void) {
    if (s_state != ARMED) return;
    if (s_source == CAPTURE_PINS) sampler_stop();
    for (uint8_t i = 0; i < s_n_streams; i++) s_streams[i].stop_at = produced(i);
    s_state = DRAINING;
}

// The stream to send a SAMPLES block from next: of those with a full block
// waiting (or anything, once draining), the one whose ring is fullest.
static int pick_stream(const uint64_t *due) {
    int best = -1;
    uint64_t best_fill = 0;          // backlog / ring, in 1/65536ths
    for (uint8_t i = 0; i < s_n_streams; i++) {
        const stream_t *st = &s_streams[i];
        uint64_t avail = due[i] - st->next;
        if (!avail || (avail < st->per_block && s_state != DRAINING)) continue;
        uint64_t fill = (avail << 16) / st->ring;
        if (best < 0 || fill > best_fill) {
            best = i;
            best_fill = fill;
        }
    }
    return best;
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

        uint64_t due[N_STREAMS_MAX];
        for (uint8_t i = 0; i < s_n_streams; i++) {
            stream_t *st = &s_streams[i];
            due[i] = samples_due(i);
            // When the sink is more than a ring behind, the ring has
            // overflowed. Drop down to half full rather than just to full:
            // loss then comes in a few large, well-separated chunks, and what
            // survives is long contiguous runs instead of a sample-by-sample
            // trickle with an OVERRUN block eating the bandwidth after every
            // block.
            if (due[i] - st->next > st->ring) {
                uint64_t lost = due[i] - st->next - st->ring / 2;
                record_loss(st, st->next, lost);
                st->next += lost;
            }
        }

        // Loss first, so an OVERRUN always sits where the samples went missing.
        int lossy = -1;
        for (uint8_t i = 0; i < s_n_streams; i++)
            if (s_streams[i].pending_lost) lossy = i;
        if (lossy >= 0) {
            if (!emit_overrun((uint8_t)lossy)) return;
            continue;
        }

        int id = pick_stream(due);
        if (id >= 0) {
            stream_t *st = &s_streams[id];
            uint64_t avail = due[id] - st->next;
            if (!emit_samples((uint8_t)id,
                              avail < st->per_block ? (uint32_t)avail : st->per_block))
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
    st->armed     = s_state == ARMED;
    st->busy      = s_state != IDLE;
    st->session   = s_session;
    st->rate_hz   = s_rate_hz;
    st->source    = s_source;
    st->sink      = s_sink;
    st->spi       = s_n_streams > 1;
    st->aborted   = s_aborted;
    st->samples   = s_n_streams ? samples_due(STREAM_RAW16) : 0;
    st->spi_bytes = st->spi ? samples_due(STREAM_SCK8) : 0;
    st->blocks    = s_seq;
    st->overruns  = s_overruns;
    st->lost      = s_lost;
}
