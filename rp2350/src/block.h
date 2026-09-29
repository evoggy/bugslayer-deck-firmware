// Capture stream block format, v0. Identical on both transports (the FX2's
// bulk IN EP6 and the RP2350's own vendor bulk IN). See ../../docs/protocol.md.
//
// Every block is exactly 512 bytes -- one FX2 packet -- little-endian, with a
// naturally aligned 28-byte header. Nothing in a block depends on the channel
// that carried it or on host arrival time.
#pragma once
#include <stdint.h>

#define BLOCK_SIZE        512
#define BLOCK_MAGIC       0x594C5342u   // "BSLY"
#define BLOCK_VERSION     0
#define BLOCK_PAYLOAD_MAX 484

enum block_type {
    BLOCK_SESSION = 0,   // first block of every session: timebase, streams, versions
    BLOCK_SAMPLES = 1,   // samples of one stream, starting at hdr.sample
    BLOCK_OVERRUN = 2,   // samples that were lost, placed where they were lost
    BLOCK_IDLE    = 3,   // reserved: heartbeat for event-driven encodings
    BLOCK_EVENT   = 4,   // reserved: annotations
    BLOCK_END     = 5,   // last block of a session: totals
};

// How a stream's SAMPLES payload is encoded.
enum stream_encoding {
    ENC_RAW16 = 1,       // u16 per sample, bit n = pin first_pin + n, fixed rate
};

typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  type;          // enum block_type
    uint8_t  stream;        // SAMPLES/OVERRUN: which stream; else 0
    uint8_t  flags;
    uint32_t session;       // from the `arm` reply; blocks of other sessions are stale
    uint32_t seq;           // per session, every block, +1; a gap is loss
    uint64_t sample;        // SAMPLES/OVERRUN: stream sample index of the first
                            // sample this block covers; SESSION: 0; END: total
    uint16_t payload_len;   // valid bytes in payload
    uint16_t reserved;
    uint8_t  payload[BLOCK_PAYLOAD_MAX];
} block_t;
_Static_assert(sizeof(block_t) == BLOCK_SIZE, "a block is one FX2 packet");

// --- payloads ---

typedef struct {
    uint8_t  id;            // matches block_t.stream
    uint8_t  encoding;      // enum stream_encoding
    uint8_t  first_pin;     // GP number of bit 0
    uint8_t  n_pins;
    uint32_t rate_num;      // sample rate = rate_num / rate_den Hz, exactly
    uint32_t rate_den;
    char     source[8];     // "counter" (synthetic) or "pins"
} stream_desc_t;
_Static_assert(sizeof(stream_desc_t) == 20, "");

typedef struct {
    uint32_t timebase_hz;   // RP2350 clk_sys, from which every sample clock derives
    uint32_t reserved;
    uint64_t arm_time_us;   // RP2350 time_us_64() at sample 0 of every stream
    char     fw_version[16];
    char     serial[16];    // flash unique ID, not NUL-terminated if 16 chars
    char     hw[8];
    uint8_t  n_streams;
    uint8_t  pad[7];
    stream_desc_t streams[]; // n_streams entries
} session_payload_t;

typedef struct {
    uint64_t lost;          // samples lost starting at block_t.sample
} overrun_payload_t;

typedef struct {
    uint32_t blocks;        // blocks in the session, this END included
    uint32_t overruns;      // OVERRUN blocks emitted
    uint64_t lost;          // samples lost in total
} end_payload_t;
