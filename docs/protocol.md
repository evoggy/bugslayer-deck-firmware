# Protocol (v0)

Two channels, one stream format.

```
  PC ──► RP2350 USB (CDC or vendor)   commands, replies, status     bidirectional, low rate
  PC ◄── FX2 bulk IN EP6              capture stream                one-way, ~25 MB/s
  PC ◄── RP2350 USB                   the same capture stream       one-way, ~1 MB/s
```

The capture stream format is **identical on both transports**. That is the single most
important decision in this document: it lets the host tool and every decoder be written and
tested over the RP2350's USB before the FX2 works at all, and it means low-rate captures
(1-Wire, I²C, UART) never need the FX2.

## Why the split, and what it costs

Data on the FX2 and control on the RP2350 is right because the FX2 is a **dumb one-way pipe
by design**. In AUTOIN mode the 8051 never touches a data byte. Adding an OUT endpoint would
need SLRD/SLOE/FIFOADR (strapped in hardware), 8051 involvement, and a second RP2350 PIO
program — to duplicate a control channel the RP2350 must expose anyway for CMSIS-DAP, UF2
recovery and configuration.

The cost is that **two USB devices appear for one physical board**, with no ordering guarantee
between them. Concretely:

- The `arm` reply may arrive after the first data block, or before it. Never assume.
- A `disarm` reply does not mean the data channel is drained.
- The FX2 appears on the bus *later* than the RP2350 — it does not exist until RP2350
  firmware releases `FX_RESET#` and serves the boot image. The host must wait and retry.
- RP2350 in BOOTSEL, or crashed, means no FX2 at all. (Not an added cost: the RP2350 is what
  produces the data, so it was always a single point of failure.)
- With two decks on one PC, or a deck plus another probe, the host must know which FX2 belongs
  to which RP2350.

Both problems are solved in the format rather than by cross-channel timing:

**1. Shared serial number.** The RP2350 derives a serial string from its flash unique ID and
patches it into the FX2 descriptors when it serves the boot image. Host pairing is then a
string match on `35F0:DB12` and `35F0:DB13` with equal serials. Hub topology (the CH334 port numbers are fixed on the PCB) is a fallback, not
the primary mechanism.

**2. Self-describing stream.** Session ID, block sequence, timestamps and inline overrun
records mean the host never correlates the channels in time. It reads the stream and knows
exactly what it has.

## USB identity

Bitcraze VID **0x35F0**. One PID per chip, because each enumerates as its own device behind
the CH334 hub:

| Device | VID:PID | Interface | Serial |
|---|---|---|---|
| RP2040 — 4× CMSIS-DAP probe | `35F0:DB11` | 4× CMSIS-DAP v2 (+ 2 CDC for SWO) | own flash unique ID |
| RP2350 — control plane | `35F0:DB12` | CDC (control) + vendor bulk IN (stream) | flash unique ID |
| CBM9002A — capture stream | `35F0:DB13` | vendor, bulk IN EP6 | **RP2350's** flash unique ID |

The FX2 deliberately reports the **RP2350's** serial, not one of its own — it has no unique ID
and no non-volatile storage, and matching serials is how the host pairs the control channel
with the data channel. The RP2350 patches the string into the boot image it serves.

Two identities that are not ours and will show up during bring-up:

- **`04B4:8613`** — the FX2's boot ROM, when no EEPROM answers at 0xA2. This is the expected
  state in stage 0c and while RAM-loading with `fx2tool`.
- **`2E8A:000F`** — the RP2350 bootrom in BOOTSEL. Note that the FX2 disappears entirely in
  this state, because nothing is holding its boot image.

## Capture stream

Fixed **512-byte blocks**, matching the FX2's EP6 packet size. With `AUTOIN` committing every
512 bytes, one block == one USB packet, so resynchronisation is trivial and a lost packet is
directly visible as a sequence gap. The layout is `rp2350/src/block.h`; `host/bsly.py` is the
reference reader. v0, little-endian, 28-byte naturally aligned header:

```
struct block {              // exactly 512 bytes
    u32 magic;              // 'BSLY' 0x594C5342 -- resync anchor
    u8  version;            // 0
    u8  type;               // see below
    u8  stream;             // SAMPLES/OVERRUN: which stream (SESSION lists them); else 0
    u8  flags;              // 0
    u32 session;            // from the `arm` reply; blocks of other sessions are stale
    u32 seq;                // per session, every block, +1; a gap is loss
    u64 sample;             // SAMPLES/OVERRUN: stream sample index of the first sample
                            // covered; END: total samples; SESSION: 0
    u16 payload_len;        // valid bytes in payload[]
    u16 reserved;
    u8  payload[484];
};
```

| type | Meaning | Payload |
|---|---|---|
| 0 | `SESSION` — always the first block (seq 0) | timebase, arm time, firmware/serial/hw strings, then one 20-byte descriptor per stream: id, encoding, first pin, pin count, rate = `rate_num/rate_den` Hz exactly, source name |
| 1 | `SAMPLES` | samples of `stream`, starting at sample index `sample`, in the stream's encoding |
| 2 | `OVERRUN` | `u64 lost`: samples `sample .. sample+lost-1` of `stream` were dropped. **Inline**, so the gap is located in the stream, not just counted |
| 3 | `IDLE` | reserved: heartbeat for event-driven encodings |
| 4 | `EVENT` | reserved: annotations |
| 5 | `END` — always the last block | `u32 blocks` (END included), `u32 overruns`, `u64 lost`. The host knows it has the whole session |

**Streams.** A session carries one or more sample streams, each with its own pins, rate and
encoding, all on one timebase: sample *n* of a stream is at `n × rate_den / rate_num` seconds
after `arm_time`. v0 has one stream, `ENC_RAW16`: a `u16` per sample, bit *n* = GP(16+*n*), so
every CF signal in one word. Raw fixed-rate sampling means the host sees real timing: clock
stretching, setup/hold, glitches, a missing STOP. Protocol decoding happens on the host.

The stream field exists because one rate cannot serve everything. The slow buses (I²C,
UART, 1-Wire, IO) are well served by 10–20 Msps raw. SPI at up to ~20 MHz needs ~80 Msps, which
no raw 16-channel stream fits through 37.5 MB/s. The fast SPI group will be a second stream
with its own encoding (time-boxed burst, edge/RLE, or SCK-clocked capture: stage 4), without a
format change.

**Rates that fit.** RP2350 Full-Speed sink: 768 kB/s measured, so ~380 ksps raw16. FX2 sink:
37.5 MB/s, so 18.75 Msps raw16 (stage 5).

**Loss.** A source samples into a RAM ring. When the sink falls behind by more than the ring,
the source drops down to half full and emits one `OVERRUN` covering exactly the dropped range.
Loss then comes in a few large chunks, with long contiguous runs between them, instead of an
`OVERRUN` block after every block eating the bandwidth.

Design rules that follow from this:

- **All timestamps come from the RP2350.** It is the only thing that sees the bus. Host
  arrival times are worthless — never mix them in.
- **The RP2350 always writes whole 512-byte blocks to the FX2.** Short writes at capture stop
  are padded and committed with `PKTEND`, otherwise up to 511 bytes sit in the FIFO and the
  tail of the capture is invisible.
- **Never report loss only over the control channel.** An `OVERRUN` block in the right place
  is worth more than a counter.
- **The `disarm` reply does not mean the stream is drained.** Read until `END`.

**On disk:** `host/bsly.py` writes sigrok session files (`.sr`: a zip of `version`, INI
`metadata`, and raw `logic-1-N` chunks). PulseView and `sigrok-cli` open them, and
libsigrokdecode's decoders (I²C, SPI, UART, 1-Wire, …) run on them unchanged. Overrun gaps are
filled by holding the last value and are reported by the tool, since `.sr` cannot mark them.

## Transports

**RP2350:** one composite device, `35F0:DB12`. Interfaces 0+1 are CDC, the ASCII control
channel below. Interface 2 is vendor-specific with bulk IN `0x83`, the capture stream. MS OS 2.0
descriptors bind WinUSB to interface 2 only (CDC keeps its inbox driver). Host tools claim
interface 2 with libusb and read it exactly like the FX2's EP6.

**FX2:** `35F0:DB13`, vendor-specific, bulk IN `0x86` (EP6), same blocks.

## Control channel

v0 is an **ASCII line protocol over CDC** — debuggable from a serial terminal with no tooling,
which is worth a lot during bring-up. A binary vendor protocol can replace it later; the
stream format is unaffected.

```
> ping                       < pong
> ver                        < ver rp2350=0.1.0 fx2=0.1.0 hw=v1
> id                         < id serial=E66038B7134C2F27
> stat                       < stat armed=1 session=7 blocks=120345 overruns=0 fx2=up
> arm <rate_hz>              < arm ok session=3852125629 rate=100000 source=counter sink=usb
> disarm                     < disarm ok session=3852125629 samples=305007 overruns=0 lost=0
> fx2 reset|status           < fx2 ok
> pwr vcc on|off             < pwr ok
> pull on|off                < pull ok           (I2C pull-ups, standalone only)
```

v0 (stage 2) has the synthetic `counter` source on the `usb` sink. Pin sources, the `fx2`
sink and stream selection extend `arm` from stage 3 on. Both carry the same blocks. `fx2 test
start|stop` is the stage 1 raw pipe test, which bypasses the block format entirely.

`arm` returns the session ID *before* any data for that session is emitted, so the host can
reject stale blocks unambiguously even though the two channels are unordered.

## Open questions

- ~~**Serial number into FX2 descriptors.**~~ Resolved: `mkc2.py` records the offset of the
  `BSLYSERIAL000000` placeholder in the generated header, and `fx2_boot.c` patches the RP2350's
  16-character unique ID over it before serving the image. Verified on hardware.
- ~~**CDC vs vendor on the RP2350.**~~ Resolved: both, as a composite (see Transports).
- **Compression / the fast SPI stream.** Raw fixed-rate is v0. RLE or edge-plus-delta
  encodings become new `stream_encoding` values in SESSION, not a format change. Stage 4
  decides how the ~20 MHz SPI group is captured.
