# Protocol (v0 draft)

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
string match. Hub topology (the CH334 port numbers are fixed on the PCB) is a fallback, not
the primary mechanism.

**2. Self-describing stream.** Session ID, block sequence, timestamps and inline overrun
records mean the host never correlates the channels in time. It reads the stream and knows
exactly what it has.

## Capture stream

Fixed **512-byte blocks**, matching the FX2's EP6 packet size. With `AUTOIN` committing every
512 bytes, one block == one USB packet, so resynchronisation is trivial and a lost packet is
directly visible as a sequence gap.

```
struct block {           // exactly 512 bytes
    u32 magic;           // 'BSLY' 0x594C5342 — resync anchor
    u16 version;         // 0 for now
    u16 type;            // see below
    u32 session;         // from the ARM reply; discard blocks from stale sessions
    u32 seq;             // monotonic per session, +1 per block; gaps == loss
    u64 ts;              // RP2350 tick at the FIRST sample in this block
    u16 payload_len;     // valid bytes in payload[]
    u16 flags;
    u8  payload[488];
};
```

| type | Meaning |
|---|---|
| 0 | `SESSION` — payload is the session header: tick rate, sample rate, channel mask, firmware versions |
| 1 | `SAMPLES` — raw sampled bytes, one byte per sample of the GP16–23 group |
| 2 | `OVERRUN` — payload says how many samples were lost and when. **Emitted inline**, so the gap is located in the stream, not just counted in a status register |
| 3 | `IDLE` — heartbeat when armed but nothing captured; keeps `ts` advancing and proves the pipe is alive |
| 4 | `EVENT` — decoded/annotated events, once there is anything to annotate |

Design rules that follow from this:

- **All timestamps come from the RP2350.** It is the only thing that sees the bus. Host
  arrival times are worthless — never mix them in.
- **The RP2350 always writes whole 512-byte blocks to the FX2.** Short writes at capture stop
  are padded and committed with `PKTEND`, otherwise up to 511 bytes sit in the FIFO and the
  tail of the capture is invisible.
- **Never report loss only over the control channel.** An `OVERRUN` block in the right place
  is worth more than a counter.

## Control channel

v0 is an **ASCII line protocol over CDC** — debuggable from a serial terminal with no tooling,
which is worth a lot during bring-up. A binary vendor protocol can replace it later; the
stream format is unaffected.

```
> ping                       < pong
> ver                        < ver rp2350=0.1.0 fx2=0.1.0 hw=v1
> id                         < id serial=E66038B7134C2F27
> stat                       < stat armed=1 session=7 blocks=120345 overruns=0 fx2=up
> arm <rate_hz> <mask> <sink>
                             < arm ok session=8
> disarm                     < disarm ok session=8 blocks=200000 overruns=2
> fx2 reset|status           < fx2 ok
> pwr vcc on|off             < pwr ok
> pull on|off                < pull ok           (I2C pull-ups, standalone only)
```

`sink` is `fx2` or `usb` — the same stream, different transport.

`arm` returns the session ID *before* any data for that session is emitted, so the host can
reject stale blocks unambiguously even though the two channels are unordered.

## Open questions

- **Serial number into FX2 descriptors.** The FX2 image is a C2 blob served from the RP2350;
  patching a string descriptor in it at boot is easy in principle. Needs a fixed offset or a
  small patch table generated at build time. Decide before descriptors are written.
- **CDC vs vendor on the RP2350.** CDC is easier to debug; a vendor interface with MS OS
  descriptors gives one WinUSB backend for both devices. Possibly both, as a composite.
- **VID/PID allocation** for the RP2350 device *and* the FX2 device. Blocks descriptor work
  on both sides.
- **Compression.** 8 bits/sample at 24 Msps is 24 MB/s of mostly idle. RLE or edge-plus-delta
  encoding in PIO/DMA is the obvious next step, but it is not needed to prove the hardware —
  keep v0 raw.
