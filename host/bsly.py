#!/usr/bin/env python3
"""Bugslayer deck capture tool (stage 2).

    bsly.py capture --rate 200000 --duration 5 -o boot.sr

Arms a session over the control CDC, reads the block stream from the RP2350's
vendor bulk IN interface, verifies it, and writes a sigrok session (.sr) that
PulseView and sigrok-cli open directly.

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

VID, PID_CTRL = 0x35F0, 0xDB12
STREAM_ITF, STREAM_EP = 2, 0x83

BLOCK_SIZE = 512
MAGIC = 0x594C5342
HDR = struct.Struct("<IBBBBIIQHH")       # 28 bytes, see rp2350/src/block.h
SESSION, SAMPLES, OVERRUN, IDLE, EVENT, END = range(6)
TYPE_NAMES = {SESSION: "SESSION", SAMPLES: "SAMPLES", OVERRUN: "OVERRUN",
              IDLE: "IDLE", EVENT: "EVENT", END: "END"}
ENC_RAW16 = 1

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


class Verifier:
    """Checks one session's blocks as they arrive and collects the samples."""

    def __init__(self, session):
        self.session = session
        self.errors = []
        self.info = None
        self.next_seq = 0
        self.next_sample = 0
        self.chunks = []           # (first_sample_index, np.uint16 array)
        self.overruns = []         # (first_lost, count)
        self.end = None
        self.blocks = 0
        self.stale = 0

    def err(self, msg):
        if len(self.errors) < 20:
            self.errors.append(msg)

    def feed(self, b):
        if b.magic != MAGIC:
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
        elif b.type == SAMPLES:
            if b.sample != self.next_sample:
                self.err(f"sample index {b.sample}, expected {self.next_sample}")
            s = np.frombuffer(b.payload, dtype="<u2")
            self.chunks.append((b.sample, s))
            self.next_sample = b.sample + len(s)
        elif b.type == OVERRUN:
            (lost,) = struct.unpack_from("<Q", b.payload)
            if b.sample != self.next_sample:
                self.err(f"OVERRUN at {b.sample}, expected {self.next_sample}")
            self.overruns.append((b.sample, lost))
            self.next_sample = b.sample + lost
        elif b.type == END:
            blocks, overruns, lost = struct.unpack_from("<IIQ", b.payload)
            self.end = dict(total=b.sample, blocks=blocks, overruns=overruns, lost=lost)
            if b.sample != self.next_sample:
                self.err(f"END says {b.sample} samples, stream reached {self.next_sample}")
            if blocks != self.blocks:
                self.err(f"END says {blocks} blocks, received {self.blocks}")
            if overruns != len(self.overruns) or lost != sum(n for _, n in self.overruns):
                self.err("END overrun totals do not match the OVERRUN blocks")

    def check_counter(self):
        """Synthetic source: sample n has the value n mod 2**16."""
        bad = 0
        for first, s in self.chunks:
            expect = (np.arange(len(s), dtype=np.uint64) + first).astype(np.uint16)
            bad += int(np.count_nonzero(s != expect))
        if bad:
            self.err(f"{bad} samples do not match the synthetic counter")

    def samples(self):
        """All samples, overrun gaps filled by holding the last value."""
        total = self.next_sample
        out = np.zeros(total, dtype=np.uint16)
        filled = np.zeros(total, dtype=bool)
        for first, s in self.chunks:
            out[first:first + len(s)] = s
            filled[first:first + len(s)] = True
        for first, n in self.overruns:
            if first > 0:
                out[first:first + n] = out[first - 1]
        return out


def samplerate_string(hz):
    if hz % 1_000_000 == 0:
        return f"{hz // 1_000_000} MHz"
    if hz % 1_000 == 0:
        return f"{hz // 1_000} kHz"
    return f"{hz} Hz"


def write_sr(path, samples, rate_hz, names):
    """sigrok session v2: a zip of `version`, `metadata`, and raw logic chunks."""
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
    with usb1.USBContext() as ctx:
        port, handle, serial_number = find_devices(ctx, args.serial)
        handle.claimInterface(STREAM_ITF)
        received = []                       # whole blocks, in arrival order
        carry = bytearray()                 # a block split across transfers
        pending = [0]
        stop = [False]

        def on_transfer(t):
            status = t.getStatus()
            # A transfer that times out can still carry data -- often exactly the
            # one block sent after a quiet spell, like SESSION. Keep it.
            if status in (usb1.TRANSFER_COMPLETED, usb1.TRANSFER_TIMED_OUT):
                carry.extend(t.getBuffer()[:t.getActualLength()])
                n = len(carry) // BLOCK_SIZE * BLOCK_SIZE
                for i in range(0, n, BLOCK_SIZE):
                    received.append(Block(bytes(carry[i:i + BLOCK_SIZE])))
                del carry[:n]
            elif status != usb1.TRANSFER_CANCELLED:
                print(f"transfer status {status}", file=sys.stderr)
            if stop[0]:
                pending[0] -= 1
            else:
                t.submit()

        for _ in range(8):
            t = handle.getTransfer()
            t.setBulk(STREAM_EP, 16 * BLOCK_SIZE, callback=on_transfer, timeout=100)
            t.submit()
            pending[0] += 1

        def pump(seconds):
            end = time.monotonic() + seconds
            while time.monotonic() < end:
                ctx.handleEvents()

        with serial.Serial(port, timeout=0.05) as ser:
            pump(0.2)                        # drain any earlier session's tail
            reply = command(ser, f"arm {args.rate} {args.source}")
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
                    v.feed(received[done_idx])
                    done_idx += 1
            print(command(ser, "disarm"))

            # Wait for END: the disarm reply does not mean the stream is drained.
            deadline = time.monotonic() + 5
            while v.end is None and time.monotonic() < deadline:
                pump(0.05)
                while done_idx < len(received):
                    v.feed(received[done_idx])
                    done_idx += 1
            elapsed = time.monotonic() - start

        stop[0] = True
        while pending[0]:
            ctx.handleEvents()
        handle.releaseInterface(STREAM_ITF)

    if v.end is None:
        v.err("no END block within 5 s of disarm")
    stream = v.info["streams"][0] if v.info else None
    if stream and stream["source"] == "counter":
        v.check_counter()

    rate = stream["rate_num"] / stream["rate_den"] if stream else 0
    lost = sum(n for _, n in v.overruns)
    print()
    print(f"device     {serial_number}  fw {v.info['fw'] if v.info else '?'}  "
          f"source {stream['source'] if stream else '?'}")
    print(f"session    {session}  ({v.blocks} blocks, {v.stale} stale from earlier sessions)")
    print(f"samples    {v.next_sample} at {rate:g} Hz = {v.next_sample / rate if rate else 0:.3f} s")
    print(f"throughput {v.blocks * BLOCK_SIZE / elapsed / 1e3:.0f} kB/s")
    print(f"overruns   {len(v.overruns)} ({lost} samples lost)")
    for first, n in v.overruns[:5]:
        print(f"           {n} samples from #{first} ({first / rate:.4f} s)")

    if args.output and stream:
        if stream["rate_num"] % stream["rate_den"]:
            print(f"note       {rate:.3f} Hz is not an integer; the .sr says {round(rate)} Hz")
        write_sr(args.output, v.samples(), round(rate), CHANNEL_NAMES[:stream["n_pins"]])
        print(f"wrote      {args.output}")

    if v.errors:
        print("\nFAIL")
        for e in v.errors:
            print(f"  {e}")
        return 1
    if args.no_overrun and v.overruns:
        print("\nFAIL (overruns not allowed)")
        return 1
    print("\nPASS")
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
    c.add_argument("--duration", type=float, default=2.0, help="seconds")
    c.add_argument("-o", "--output", help="sigrok .sr file to write")
    c.add_argument("--no-overrun", action="store_true", help="fail if any samples were lost")
    args = ap.parse_args()
    if args.cmd == "capture":
        sys.exit(capture(args))


if __name__ == "__main__":
    main()
