#!/usr/bin/env python3
"""Bugslayer deck capture tool.

    bsly.py capture --rate 200000 --duration 5 -o boot.sr
    bsly.py capture --sink fx2 --rate 15000000 --duration 5 -o boot.sr

Arms a session over the control CDC, reads the block stream from the RP2350's
vendor bulk IN interface (--sink usb, Full Speed, ~380 ksps) or the FX2's EP6
(--sink fx2, High Speed, ~17 Msps), verifies it, and writes a sigrok session
(.sr) that PulseView and sigrok-cli open directly. The FX2 is paired with the
control port by serial number: the RP2350 patches its own into the FX2's boot
image.

Verification, per docs/protocol.md: the session ID matches the `arm` reply,
SESSION is the first block, sequence numbers have no gaps, sample indices are
continuous except where an OVERRUN block says samples were lost, and END
arrives with totals matching what was received. For the synthetic `counter`
source every sample must equal its own index (mod 2**16).
"""

import argparse
import configparser
import io
import struct
import sys
import time
import zipfile

import numpy as np
import serial
import usb1
from serial.tools import list_ports

VID, PID_CTRL, PID_FX2 = 0x35F0, 0xDB12, 0xDB13
STREAM_ITF, STREAM_EP = 2, 0x83            # RP2350 vendor interface
FX2_ITF, FX2_EP = 0, 0x86                  # FX2 EP6

BLOCK_SIZE = 512
MAGIC = 0x594C5342
HDR = struct.Struct("<IBBBBIIQHH")       # 28 bytes, see rp2350/src/block.h
SESSION, SAMPLES, OVERRUN, IDLE, EVENT, END = range(6)
TYPE_NAMES = {SESSION: "SESSION", SAMPLES: "SAMPLES", OVERRUN: "OVERRUN",
              IDLE: "IDLE", EVENT: "EVENT", END: "END"}
ENC_RAW16 = 1

# The same header as a numpy record, for checking a whole transfer at once.
BLOCK_DT = np.dtype([("magic", "<u4"), ("version", "u1"), ("type", "u1"), ("stream", "u1"),
                     ("flags", "u1"), ("session", "<u4"), ("seq", "<u4"), ("sample", "<u8"),
                     ("plen", "<u2"), ("reserved", "<u2"), ("payload", "V484")])
assert BLOCK_DT.itemsize == 512
BLOCK_PAYLOAD_MAX = 484

# GP16..31 in bit order, as wired on rev A (docs/hardware.md).
CHANNEL_NAMES = ["IO_1", "IO_2", "IO_3", "IO_4", "MISO", "OW", "SCK", "MOSI",
                 "WKUP", "N_IO_1", "TX2", "RX2", "TX1", "RX1", "SDA", "SCL"]


class Block:
    __slots__ = ("magic", "version", "type", "stream", "flags", "session", "seq",
                 "sample", "payload")

    def __init__(self, raw):
        (self.magic, self.version, self.type, self.stream, self.flags, self.session,
         self.seq, self.sample, plen, _) = HDR.unpack_from(raw)
        self.payload = raw[HDR.size:HDR.size + plen]


def parse_session(payload):
    timebase, _, arm_us = struct.unpack_from("<IIQ", payload, 0)
    fw, ser, hw = (payload[16:32], payload[32:48], payload[48:56])
    n = payload[56]
    streams = []
    for i in range(n):
        sid, enc, first_pin, n_pins, num, den = struct.unpack_from("<BBBBII", payload, 64 + 20 * i)
        src = payload[64 + 20 * i + 12:64 + 20 * i + 20].rstrip(b"\0").decode()
        streams.append(dict(id=sid, encoding=enc, first_pin=first_pin, n_pins=n_pins,
                            rate_num=num, rate_den=den, source=src))
    txt = lambda b: b.rstrip(b"\0").decode(errors="replace")
    return dict(timebase_hz=timebase, arm_time_us=arm_us, fw=txt(fw), serial=txt(ser),
                hw=txt(hw), streams=streams)


class Stream:
    """One stream of a session, as received."""

    def __init__(self, desc):
        self.desc = desc
        self.dtype = "<u2" if desc["encoding"] == ENC_RAW16 else "u1"
        self.per_block = BLOCK_PAYLOAD_MAX // np.dtype(self.dtype).itemsize
        self.next_sample = 0
        self.chunks = []           # (first_sample_index, array)
        self.overruns = []         # (first_lost, count)

    def samples(self):
        """All samples, overrun gaps filled by holding the last value."""
        out = np.zeros(self.next_sample, dtype=self.dtype)
        for first, s in self.chunks:
            out[first:first + len(s)] = s
        for first, n in self.overruns:
            if first > 0:
                out[first:first + n] = out[first - 1]
        return out


class Verifier:
    """Checks one session's blocks as they arrive and collects each stream."""

    def __init__(self, session):
        self.session = session
        self.errors = []
        self.info = None
        self.streams = []          # Stream, by id, once SESSION has arrived
        self.next_seq = 0
        self.end = None
        self.blocks = 0
        self.stale = 0

    def err(self, msg):
        if len(self.errors) < 20:
            self.errors.append(msg)

    def feed_raw(self, buf):
        """A run of whole blocks. The steady state -- every block a full SAMPLES
        block of this session, each stream continuing exactly where it left off
        -- is checked and stored with numpy in one go; anything else (SESSION,
        OVERRUN, END, stale or broken blocks) goes through feed() one block at a
        time."""
        a = np.frombuffer(buf, dtype=BLOCK_DT)
        n = len(a)
        if n and self.streams and self.end is None and self._fast(buf, a):
            return
        for i in range(0, len(buf), BLOCK_SIZE):
            self.feed(Block(buf[i:i + BLOCK_SIZE]))

    def _fast(self, buf, a):
        n = len(a)
        seq0 = self.next_seq
        if not (np.all(a["magic"] == MAGIC) and np.all(a["session"] == self.session)
                and np.all(a["type"] == SAMPLES) and np.all(a["plen"] == BLOCK_PAYLOAD_MAX)
                and np.all(a["stream"] < len(self.streams))
                and np.array_equal(a["seq"], (np.arange(n, dtype=np.uint64) + seq0).astype(np.uint32))):
            return False
        picks = []
        for sid, st in enumerate(self.streams):
            idx = np.nonzero(a["stream"] == sid)[0]
            if not len(idx):
                continue
            k = np.arange(len(idx), dtype=np.uint64)
            if not np.array_equal(a["sample"][idx], k * st.per_block + st.next_sample):
                return False
            picks.append((st, idx))
        raw = np.frombuffer(buf, dtype=np.uint8).reshape(n, BLOCK_SIZE)
        for st, idx in picks:
            s = raw[idx, 28:].copy().view(st.dtype).ravel()
            st.chunks.append((st.next_sample, s))
            st.next_sample += len(s)
        self.blocks += n
        self.next_seq = seq0 + n
        return True

    def feed(self, b):
        if b.magic != MAGIC:
            if self.blocks == 0:
                self.stale += 1      # residue from before this session: the stage 1
                return               # test, or an aborted session's tail
            self.err(f"bad magic 0x{b.magic:08x}")
            return
        if b.session != self.session:
            self.stale += 1          # an earlier session's tail; expected, not an error
            return
        self.blocks += 1
        if b.seq != self.next_seq:
            self.err(f"seq gap: expected {self.next_seq}, got {b.seq}")
        self.next_seq = b.seq + 1
        if self.blocks == 1 and b.type != SESSION:
            self.err(f"first block is {TYPE_NAMES.get(b.type, b.type)}, not SESSION")

        if b.type == SESSION:
            self.info = parse_session(b.payload)
            self.streams = [Stream(d) for d in self.info["streams"]]
            return
        if b.type in (SAMPLES, OVERRUN):
            if b.stream >= len(self.streams):
                self.err(f"{TYPE_NAMES[b.type]} for unknown stream {b.stream}")
                return
            st = self.streams[b.stream]
            if b.sample != st.next_sample:
                self.err(f"stream {b.stream}: {TYPE_NAMES[b.type]} at {b.sample}, "
                         f"expected {st.next_sample}")
            if b.type == SAMPLES:
                s = np.frombuffer(b.payload, dtype=st.dtype)
                st.chunks.append((b.sample, s))
                st.next_sample = b.sample + len(s)
            else:
                (lost,) = struct.unpack_from("<Q", b.payload)
                st.overruns.append((b.sample, lost))
                st.next_sample = b.sample + lost
        elif b.type == END:
            blocks, overruns, lost = struct.unpack_from("<IIQ", b.payload)
            totals = [b.sample]
            if len(b.payload) >= 24:
                n = b.payload[16]
                totals = list(struct.unpack_from(f"<{n}Q", b.payload, 24))
            self.end = dict(total=b.sample, blocks=blocks, overruns=overruns, lost=lost,
                            totals=totals)
            for sid, st in enumerate(self.streams):
                if sid < len(totals) and totals[sid] != st.next_sample:
                    self.err(f"END says stream {sid} has {totals[sid]} samples, "
                             f"it reached {st.next_sample}")
            if blocks != self.blocks:
                self.err(f"END says {blocks} blocks, received {self.blocks}")
            all_ovr = [o for st in self.streams for o in st.overruns]
            if overruns != len(all_ovr) or lost != sum(n for _, n in all_ovr):
                self.err("END overrun totals do not match the OVERRUN blocks")

    def check_counter(self):
        """Synthetic source: sample n has the value n mod 2**16."""
        bad = 0
        for first, s in self.streams[0].chunks:
            expect = (np.arange(len(s), dtype=np.uint64) + first).astype(np.uint16)
            bad += int(np.count_nonzero(s != expect))
        if bad:
            self.err(f"{bad} samples do not match the synthetic counter")


# --- SPI (stage 4) ---------------------------------------------------------
#
# The sck8 stream holds GP16-23 at every rising SCK edge: bits 0-3 IO_1..IO_4
# (the CS candidates), 4 MISO, 5 OW, 6 SCK, 7 MOSI. Bit 6 is 1 in every edge
# byte, so a byte with bit 6 clear is a marker the deck inserts when CS changed
# since the previous edge: that is what separates two transactions back to back
# on the same CS. The stream has exact bits but no time. The raw16 stream has
# time: its CS windows, in order, are the sck8 stream's transactions. When
# raw16 also resolves SCK (a few samples per SCK period) it counts each
# window's edges too, which gives an independent decode to check sck8 against.

IO_MASK, MISO_BIT, SCK_BIT, MOSI_BIT = 0x0F, 4, 6, 7


def bits_to_bytes(bits):
    """MSB-first bytes from a 0/1 array; a trailing partial byte is dropped."""
    n = len(bits) // 8
    return np.packbits(bits[:n * 8].astype(np.uint8)) if n else np.zeros(0, np.uint8)


def cs_name(pattern, idle):
    diff = (pattern ^ idle) & IO_MASK
    names = [CHANNEL_NAMES[i] for i in range(4) if diff >> i & 1]
    return "+".join(names) if names else "none"


def raw16_windows(raw, rate):
    """CS windows in the raw16 stream: runs where IO_1..4 differ from idle,
    with the SCK rising edges raw16 saw inside. Idle is the commonest pattern,
    not the first: a capture can start inside a transaction."""
    io = (raw & IO_MASK).astype(np.uint8)
    idle = int(np.bincount(io, minlength=16).argmax()) if len(io) else 0
    change = np.flatnonzero(np.diff(io.astype(np.int16))) + 1
    bounds = np.r_[0, change, len(io)]
    sck = (raw >> SCK_BIT & 1).astype(np.int8)
    rises = np.flatnonzero(np.diff(sck) == 1) + 1
    out = []
    for a, b in zip(bounds[:-1], bounds[1:]):
        if io[a] == idle:
            continue
        r = rises[(rises >= a) & (rises < b)]
        out.append(dict(t=a / rate, t_end=b / rate, pattern=int(io[a]), edges=len(r), rises=r))
    return out, idle


def sck8_segments(b):
    """Split the sck8 stream at markers and at CS pattern changes. Returns
    (pattern, edge bytes) per segment."""
    b = np.asarray(b, dtype=np.uint8)
    marker = (b >> SCK_BIT & 1) == 0
    edges = b[~marker]
    # A marker after edge k (k-th edge byte, counting edges only) starts a
    # segment at edge k.
    starts = set(int(i) for i in np.cumsum(~marker)[marker] - 1)
    pat = edges & IO_MASK
    starts |= set(int(i) for i in np.flatnonzero(np.diff(pat.astype(np.int16))) + 1)
    starts.discard(0)
    cuts = [0] + sorted(i for i in starts if 0 < i < len(edges)) + [len(edges)]
    return [(int(pat[a]), edges[a:e]) for a, e in zip(cuts[:-1], cuts[1:]) if e > a]


def decode_spi(sck8, raw=None, rate=None):
    """Transactions from the sck8 stream, timed by raw16 when given.
    Returns (transactions, notes)."""
    notes = []
    segs = sck8_segments(sck8)
    windows, idle = raw16_windows(raw, rate) if raw is not None else ([], None)
    active = [i for i, (p, _) in enumerate(segs) if idle is None or p != idle]
    timed = {}
    if windows:
        # Windows with SCK activity are the ones sck8 can see; raw16 may miss
        # SCK entirely when it is too slow, so fall back to all windows.
        seen = [w for w in windows if w["edges"]]
        for cand in (seen, windows):
            if len(cand) == len(active) and all(
                    w["pattern"] == segs[i][0] for w, i in zip(cand, active)):
                timed = dict(zip(active, cand))
                break
        else:
            notes.append(f"{len(active)} sck8 transactions vs {len(seen)} raw16 CS windows "
                         f"with SCK ({len(windows)} in all): not timed")
    txns = []
    for i, (p, e) in enumerate(segs):
        w = timed.get(i)
        txns.append(dict(
            t=w["t"] if w else None, dur=(w["t_end"] - w["t"]) if w else None,
            cs=cs_name(p, idle) if idle is not None else f"0x{p:x}",
            edges=len(e),
            mosi=bits_to_bytes(e >> MOSI_BIT & 1), miso=bits_to_bytes(e >> MISO_BIT & 1),
            window=w))
    return txns, notes


def raw16_spi_check(txns, raw):
    """For transactions whose raw16 window resolved every SCK edge, decode the
    same bytes from raw16 alone and compare. Returns (checked, mismatches)."""
    checked = bad = 0
    for t in txns:
        w = t["window"]
        if w is None or w["edges"] != t["edges"]:
            continue
        v = raw[w["rises"]]          # the sample at each rising edge
        if not (np.array_equal(bits_to_bytes(v >> MOSI_BIT & 1), t["mosi"]) and
                np.array_equal(bits_to_bytes(v >> MISO_BIT & 1), t["miso"])):
            bad += 1
        checked += 1
    return checked, bad


def format_txn(t):
    hx = lambda a: " ".join(f"{x:02X}" for x in a)
    when = f"{t['t']:11.6f} s" if t["t"] is not None else "          ? s"
    dur = f"{t['dur'] * 1e6:8.1f} us" if t["dur"] is not None else "           "
    return (f"{when} {dur}  {t['cs']:<6} {t['edges']:5d} clk  "
            f"MOSI {hx(t['mosi'][:16])}{' ...' if len(t['mosi']) > 16 else ''}\n"
            f"{'':44}MISO {hx(t['miso'][:16])}{' ...' if len(t['miso']) > 16 else ''}")


def samplerate_string(hz):
    if hz % 1_000_000 == 0:
        return f"{hz // 1_000_000} MHz"
    if hz % 1_000 == 0:
        return f"{hz // 1_000} kHz"
    return f"{hz} Hz"


SCK8_MEMBER = "bugslayer-sck8"      # extra zip member: the sck8 stream, raw bytes


def write_sr(path, samples, rate_hz, names, sck8=None):
    """sigrok session v2: a zip of `version`, `metadata`, and raw logic chunks.
    The sck8 stream, if any, rides along as one more zip member, which sigrok
    ignores and `bsly.py spi` reads."""
    meta = configparser.RawConfigParser()
    meta.optionxform = str
    meta["global"] = {"sigrok version": "0.5.2"}
    dev = {"capturefile": "logic-1", "total probes": str(len(names)),
           "samplerate": samplerate_string(rate_hz), "total analog": "0"}
    for i, n in enumerate(names):
        dev[f"probe{i + 1}"] = n
    dev["unitsize"] = "2"
    meta["device 1"] = dev
    buf = io.StringIO()
    meta.write(buf, space_around_delimiters=False)

    raw = samples.astype("<u2").tobytes()
    chunk = 4 * 1024 * 1024
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("version", "2")
        z.writestr("metadata", buf.getvalue())
        for i in range(0, max(len(raw), 1), chunk):
            z.writestr(f"logic-1-{i // chunk + 1}", raw[i:i + chunk])
        if sck8 is not None:
            z.writestr(SCK8_MEMBER, np.asarray(sck8, dtype=np.uint8).tobytes())


def read_sr(path):
    """(raw16 samples, rate in Hz, sck8 bytes or None) from a .sr we wrote."""
    with zipfile.ZipFile(path) as z:
        meta = configparser.RawConfigParser()
        meta.read_string(z.read("metadata").decode())
        dev = meta["device 1"]
        num, unit = dev["samplerate"].split()
        rate = int(num) * {"Hz": 1, "kHz": 1000, "MHz": 1000000}[unit]
        names = sorted((n for n in z.namelist() if n.startswith(dev["capturefile"] + "-")),
                       key=lambda n: int(n.rsplit("-", 1)[1]))
        raw = np.frombuffer(b"".join(z.read(n) for n in names), dtype="<u2")
        sck8 = (np.frombuffer(z.read(SCK8_MEMBER), dtype=np.uint8)
                if SCK8_MEMBER in z.namelist() else None)
    return raw, rate, sck8


def find_fx2(ctx, serial_number, timeout=5.0):
    """The FX2 with the control port's serial. It enumerates a moment after the
    RP2350 (it only exists once the RP2350 has booted it), so wait for it."""
    deadline = time.monotonic() + timeout
    while True:
        for dev in ctx.getDeviceIterator(skip_on_error=True):
            if (dev.getVendorID(), dev.getProductID()) == (VID, PID_FX2):
                try:
                    if dev.getSerialNumber() == serial_number:
                        return dev.open()
                except usb1.USBError:
                    pass
        if time.monotonic() > deadline:
            sys.exit(f"no FX2 (35f0:db13) with serial {serial_number}; "
                     "is it up? (`fx2 up`, `stat`)")
        time.sleep(0.2)


def find_devices(ctx, serial_number=None):
    ports = [p for p in list_ports.comports()
             if p.vid == VID and p.pid == PID_CTRL
             and (serial_number is None or p.serial_number == serial_number)]
    if not ports:
        sys.exit("no Bugslayer deck control port (35f0:db12) found")
    port = ports[0]
    for dev in ctx.getDeviceIterator(skip_on_error=True):
        if (dev.getVendorID(), dev.getProductID()) == (VID, PID_CTRL):
            if dev.getSerialNumber() == port.serial_number:
                return port.device, dev.open(), port.serial_number
    sys.exit(f"control port {port.device} has no matching USB device")


def command(ser, line, timeout=2.0):
    ser.reset_input_buffer()
    ser.write((line + "\n").encode())
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        reply = ser.readline().decode(errors="replace").strip()
        if reply:
            return reply
    raise TimeoutError(f"no reply to {line!r}")


def capture(args):
    fx2 = args.sink == "fx2"
    with usb1.USBContext() as ctx:
        port, ctrl_handle, serial_number = find_devices(ctx, args.serial)
        if fx2:
            # No timeout: cancelling a part-filled High Speed transfer can lose
            # the packets already in it. The deck ends transfers instead, with a
            # zero-length packet whenever it goes quiet (and after END).
            handle, itf, ep = find_fx2(ctx, serial_number), FX2_ITF, FX2_EP
            n_xfers, xfer_size, xfer_timeout = 64, 32 * BLOCK_SIZE, 0   # 1 MB, ~28 ms
        else:
            handle, itf, ep = ctrl_handle, STREAM_ITF, STREAM_EP
            n_xfers, xfer_size, xfer_timeout = 8, 16 * BLOCK_SIZE, 100
        handle.claimInterface(itf)
        received = []                       # runs of whole blocks, in arrival order
        carry = bytearray()                 # USB sink: a block split across transfers
        short = [0]                         # FX2: bytes of short packets discarded
        pending = [0]
        stop = [False]
        transfers = []

        def on_transfer(t):
            status = t.getStatus()
            # A transfer that times out can still carry data -- often exactly the
            # one block sent after a quiet spell, like SESSION. Keep it.
            if status in (usb1.TRANSFER_COMPLETED, usb1.TRANSFER_TIMED_OUT):
                data = t.getBuffer()[:t.getActualLength()]
                if fx2:
                    # One block is one packet, and a short packet ends a transfer,
                    # so a transfer is whole blocks plus at most one short packet:
                    # the RP2350's PKTEND flush of an earlier session's partial
                    # block. It is never part of this session; drop it.
                    n = len(data) // BLOCK_SIZE * BLOCK_SIZE
                    short[0] += len(data) - n
                    if n:
                        received.append(bytes(data[:n]))
                else:
                    carry.extend(data)
                    n = len(carry) // BLOCK_SIZE * BLOCK_SIZE
                    if n:
                        received.append(bytes(carry[:n]))
                    del carry[:n]
            elif status != usb1.TRANSFER_CANCELLED:
                print(f"transfer status {status}", file=sys.stderr)
            if stop[0]:
                pending[0] -= 1
            else:
                t.submit()

        for _ in range(n_xfers):
            t = handle.getTransfer()
            t.setBulk(ep, xfer_size, callback=on_transfer, timeout=xfer_timeout)
            t.submit()
            pending[0] += 1
            transfers.append(t)

        def pump(seconds):
            end = time.monotonic() + seconds
            while time.monotonic() < end:
                ctx.handleEventsTimeout(0.02)

        with serial.Serial(port, timeout=0.05) as ser:
            pump(0.2)                        # drain any earlier session's tail
            reply = command(ser, f"arm {args.rate} {args.source} {args.sink}"
                                 + (" spi" if args.spi else ""))
            print(reply)
            if not reply.startswith("arm ok"):
                sys.exit(1)
            session = int(dict(kv.split("=") for kv in reply.split()[2:])["session"])
            v = Verifier(session)

            start = time.monotonic()
            done_idx = 0
            while time.monotonic() - start < args.duration:
                pump(0.1)
                while done_idx < len(received):
                    v.feed_raw(received[done_idx])
                    received[done_idx] = None
                    done_idx += 1
            print(command(ser, "disarm"))

            # Wait for END: the disarm reply does not mean the stream is drained.
            deadline = time.monotonic() + 5
            while v.end is None and time.monotonic() < deadline:
                pump(0.05)
                while done_idx < len(received):
                    v.feed_raw(received[done_idx])
                    received[done_idx] = None
                    done_idx += 1
            elapsed = time.monotonic() - start

        stop[0] = True
        for t in transfers:                 # FX2 transfers never time out
            try:
                t.cancel()
            except usb1.USBError:
                pass                        # already completed
        while pending[0]:
            ctx.handleEventsTimeout(0.02)
        handle.releaseInterface(itf)

    if v.end is None:
        v.err("no END block within 5 s of disarm")
    raw = v.streams[0] if v.streams else None
    stream = raw.desc if raw else None
    if stream and stream["source"] == "counter":
        v.check_counter()

    rate = stream["rate_num"] / stream["rate_den"] if stream else 0
    all_ovr = [(sid, o) for sid, st in enumerate(v.streams) for o in st.overruns]
    lost = sum(n for _, (_, n) in all_ovr)
    print()
    print(f"device     {serial_number}  fw {v.info['fw'] if v.info else '?'}  "
          f"source {stream['source'] if stream else '?'}")
    print(f"session    {session}  via {args.sink}  ({v.blocks} blocks, "
          f"{v.stale} stale from earlier sessions)")
    if short[0]:
        print(f"flushed    {short[0]} bytes of an earlier session's partial block")
    if raw:
        n = raw.next_sample
        print(f"samples    {n} at {rate:g} Hz = {n / rate if rate else 0:.3f} s")
    sck = v.streams[1] if len(v.streams) > 1 else None
    if sck:
        print(f"spi        {sck.next_sample} SCK edges")
    print(f"throughput {v.blocks * BLOCK_SIZE / elapsed / 1e3:.0f} kB/s")
    print(f"overruns   {len(all_ovr)} ({lost} samples lost)")
    for sid, (first, n) in all_ovr[:5]:
        where = f"{first / rate:.4f} s" if sid == 0 and rate else f"edge #{first}"
        print(f"           stream {sid}: {n} lost from #{first} ({where})")

    raw16 = raw.samples() if raw else None
    sck8 = sck.samples() if sck else None
    if sck8 is not None:
        report_spi(sck8, raw16, rate, args.spi_show)

    if args.output and stream:
        if stream["rate_num"] % stream["rate_den"]:
            print(f"note       {rate:.3f} Hz is not an integer; the .sr says {round(rate)} Hz")
        write_sr(args.output, raw16, round(rate), CHANNEL_NAMES[:stream["n_pins"]], sck8)
        print(f"wrote      {args.output}")

    if v.errors:
        print("\nFAIL")
        for e in v.errors:
            print(f"  {e}")
        return 1
    if args.no_overrun and all_ovr:
        print("\nFAIL (overruns not allowed)")
        return 1
    print("\nPASS")
    return 0


def report_spi(sck8, raw16, rate, show):
    """Decode the sck8 stream, cross-check it against raw16, print a summary."""
    txns, notes = decode_spi(sck8, raw16, rate)
    by_cs = {}
    for t in txns:
        by_cs[t["cs"]] = by_cs.get(t["cs"], 0) + 1
    print(f"spi txns   {len(txns)}  " + "  ".join(f"{k}: {n}" for k, n in sorted(by_cs.items())))
    if raw16 is not None:
        checked, bad = raw16_spi_check(txns, raw16)
        print(f"spi check  {checked} transactions also decoded from raw16: "
              f"{checked - bad} identical, {bad} different")
    for note in notes[:5]:
        print(f"spi note   {note}")
    for t in txns[:show]:
        print(format_txn(t))
    return txns


def spi_cmd(args):
    raw16, rate, sck8 = read_sr(args.file)
    if sck8 is None:
        sys.exit(f"{args.file} has no sck8 stream (capture with --spi)")
    txns = report_spi(sck8, raw16, rate, 0)
    for t in txns:
        if args.cs and t["cs"] != args.cs:
            continue
        print(format_txn(t))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--serial", help="pick one deck by its serial number")
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("capture", help="arm, capture, disarm, verify, write .sr")
    c.add_argument("--rate", type=int, default=250000,
                   help="sample rate, Hz; the deck rounds to 150 MHz / integer, "
                        "the exact rate is in the SESSION block")
    c.add_argument("--source", choices=("pins", "counter"), default="pins",
                   help="the 16 CF signals, or the synthetic counter (stage 2 test)")
    c.add_argument("--sink", choices=("usb", "fx2"), default="usb",
                   help="RP2350's own USB (Full Speed, <=~380 ksps) or the FX2 (High Speed)")
    c.add_argument("--duration", type=float, default=2.0, help="seconds")
    c.add_argument("-o", "--output", help="sigrok .sr file to write")
    c.add_argument("--no-overrun", action="store_true", help="fail if any samples were lost")
    c.add_argument("--spi", action="store_true",
                   help="also capture GP16-23 at every rising SCK edge (exact SPI at any speed)")
    c.add_argument("--spi-show", type=int, default=10, metavar="N",
                   help="print the first N decoded SPI transactions")
    p = sub.add_parser("spi", help="decode the SPI stream of a .sr written with --spi")
    p.add_argument("file")
    p.add_argument("--cs", help="only this CS, e.g. IO_3")
    args = ap.parse_args()
    if args.cmd == "capture":
        sys.exit(capture(args))
    if args.cmd == "spi":
        sys.exit(spi_cmd(args))


if __name__ == "__main__":
    main()
