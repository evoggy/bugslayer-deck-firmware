#!/usr/bin/env python3
"""Stage 1 pipe test: read the FX2 bulk IN endpoint and verify the counter.

The RP2350 streams a 32-bit little-endian incrementing counter (see
rp2350/src/fx2_link.c). Any loss shows up as a jump in that counter, so this
reports not just *that* bytes were lost but *how many* -- which is the whole
reason for using a 32-bit counter rather than a byte pattern.

    pip install libusb1
    python3 host/fx2_counter_test.py --duration 60

Needs queued async transfers to reach full speed; a synchronous read loop
tops out well below the link rate and would report false failures.
"""

import argparse
import struct
import sys
import time

import numpy as np

try:
    import usb1
except ImportError:
    sys.exit("libusb1 is required: pip install libusb1")

# Bring-up default: with the RP2350 silent on I2C the FX2 falls back to its
# ROM identity and the firmware is RAM-loaded with fx2tool. Once the boot image
# comes from the emulated EEPROM it enumerates as 35F0:DB13 instead --
# pass `--ours` for that.
DEFAULT_VID = 0x04B4
DEFAULT_PID = 0x8613
BSLY_VID = 0x35F0
BSLY_PID_FX2 = 0xDB13

EP_IN = 0x86
XFER_SIZE = 64 * 1024
XFER_COUNT = 16


class Checker:
    """Verifies a stream of little-endian uint32 counter words.

    A jump in the counter is a gap: words lost in transit, counted exactly.
    A stream that stops parsing as consecutive words at any byte alignment is
    a resync -- a partial word landed in the FIFO, e.g. at a disarm/arm seam.
    Resyncs are reported separately and never counted as lost words.
    """

    RUN = 4   # consecutive words needed to accept an alignment

    def __init__(self):
        self.expected = None
        self.buf = b""
        self.bytes = 0
        self.gaps = 0
        self.words_lost = 0
        self.first_gap = None
        self.resyncs = 0
        self.restarts = 0

    def _fast_path(self):
        """Clean-stream case, vectorised: every whole word in the buffer is the
        next counter value. Anything else falls back to the per-word walk,
        which classifies gaps, resyncs and restarts exactly."""
        n = len(self.buf) // 4
        if n == 0:
            return True
        w = np.frombuffer(self.buf, dtype="<u4", count=n)
        expect = (np.arange(n, dtype=np.uint64) + self.expected).astype(np.uint32)
        if not np.array_equal(w, expect):
            return False
        self.expected = (int(w[-1]) + 1) & 0xFFFFFFFF
        self.buf = self.buf[n * 4:]
        return True

    def _find_alignment(self):
        need = 4 * self.RUN
        for off in range(4):
            if len(self.buf) < off + need:
                return None
            w = struct.unpack_from(f"<{self.RUN}I", self.buf, off)
            if all((w[i] + 1) & 0xFFFFFFFF == w[i + 1] for i in range(self.RUN - 1)):
                return off
        return -1

    def feed(self, data):
        self.bytes += len(data)
        self.buf += data
        if self.expected is not None and self._fast_path():
            return
        pos = 0
        while True:
            if self.expected is None:
                self.buf = self.buf[pos:]
                pos = 0
                off = self._find_alignment()
                if off is None:
                    return                      # need more data
                if off < 0:
                    self.buf = self.buf[1:]     # no alignment works here; slide
                    continue
                pos = off
                self.expected = struct.unpack_from("<I", self.buf, pos)[0]
            if len(self.buf) - pos < 4:
                break
            (word,) = struct.unpack_from("<I", self.buf, pos)
            if word != self.expected:
                nxt = struct.unpack_from("<I", self.buf, pos + 4)[0] if len(self.buf) - pos >= 8 else None
                if nxt is not None and nxt != (word + 1) & 0xFFFFFFFF:
                    # Not a clean jump in the counter: the byte alignment moved.
                    self.resyncs += 1
                    self.expected = None
                    continue
                if word == 0:
                    # The RP2350 restarted the counter (disarm/arm): a session
                    # seam behind data still buffered in EP6, not lost words.
                    self.restarts += 1
                    self.expected = 1
                    pos += 4
                    continue
                # Modular arithmetic so a wrap past 2**32 is not a false gap.
                lost = (word - self.expected) & 0xFFFFFFFF
                self.gaps += 1
                self.words_lost += lost
                if self.first_gap is None:
                    self.first_gap = (self.expected, word)
            self.expected = (word + 1) & 0xFFFFFFFF
            pos += 4
        self.buf = self.buf[pos:]


def report(checker, resync_fails=False):
    print(f"resyncs    {checker.resyncs}")
    print(f"restarts   {checker.restarts}")
    # In a streaming run a resync can only come from a disarm/arm seam, so it is
    # reported, not failed. Inside the stall test there is no seam.
    ok = checker.gaps == 0 and checker.bytes > 0
    if resync_fails:
        ok = ok and checker.resyncs == 0
    print("\nPASS" if ok else "\nFAIL")
    return 0 if ok else 1


def stall_test(handle, checker, rounds):
    """Every pause fills EP6, so every round crosses the FLAGB edge. That edge
    is where flow control that reacts too late loses a byte (shows up as a
    resync)."""
    for r in range(rounds):
        for _ in range(48):
            checker.feed(handle.bulkRead(EP_IN, XFER_SIZE, timeout=1000))
        time.sleep(0.2 + 0.1 * r)
        print(f"  round {r + 1}/{rounds}  gaps={checker.gaps} resyncs={checker.resyncs}")
    print(f"\nbytes      {checker.bytes} ({checker.bytes / 1e6:.1f} MB)")
    print(f"gaps       {checker.gaps}")
    return report(checker, resync_fails=True)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--vid", type=lambda s: int(s, 0), default=DEFAULT_VID)
    p.add_argument("--pid", type=lambda s: int(s, 0), default=DEFAULT_PID)
    p.add_argument("--duration", type=float, default=10.0, help="seconds")
    p.add_argument("--serial", help="only match this iSerialNumber")
    p.add_argument("--xfer-size", type=int, default=XFER_SIZE, help="bytes per queued transfer")
    p.add_argument("--xfers", type=int, default=XFER_COUNT, help="transfers kept in flight")
    p.add_argument("--no-check", action="store_true",
                   help="count bytes only; measures the link, not the Python checker")
    p.add_argument("--stall", type=int, metavar="N", default=0,
                   help="backpressure test instead: N rounds of reading 3 MB then "
                        "pausing, so EP6 fills and FLAGB stalls the RP2350")
    p.add_argument("--ours", action="store_true",
                   help=f"look for {BSLY_VID:04x}:{BSLY_PID_FX2:04x} instead of the FX2 ROM ID")
    args = p.parse_args()
    if args.ours:
        args.vid, args.pid = BSLY_VID, BSLY_PID_FX2

    checker = Checker()
    inflight = [0]

    def on_transfer(transfer):
        status = transfer.getStatus()
        # A timed-out transfer can still carry data; dropping it would look
        # exactly like loss on the link.
        if status in (usb1.TRANSFER_COMPLETED, usb1.TRANSFER_TIMED_OUT):
            if args.no_check:
                checker.bytes += transfer.getActualLength()
            else:
                checker.feed(transfer.getBuffer()[:transfer.getActualLength()])
        else:
            print(f"transfer status {status}", file=sys.stderr)
            inflight[0] -= 1
            return
        if not done[0]:
            transfer.submit()
        else:
            inflight[0] -= 1

    done = [False]

    with usb1.USBContext() as ctx:
        handle = None
        for dev in ctx.getDeviceIterator(skip_on_error=True):
            if (dev.getVendorID(), dev.getProductID()) != (args.vid, args.pid):
                continue
            if args.serial:
                try:
                    if dev.getSerialNumber() != args.serial:
                        continue
                except usb1.USBError:
                    continue
            handle = dev.open()
            break
        if handle is None:
            sys.exit(f"no device {args.vid:04x}:{args.pid:04x} found")

        handle.claimInterface(0)
        if args.stall:
            try:
                return stall_test(handle, checker, args.stall)
            finally:
                handle.releaseInterface(0)
        try:
            transfers = []
            for _ in range(args.xfers):
                t = handle.getTransfer()
                t.setBulk(EP_IN, args.xfer_size, callback=on_transfer, timeout=1000)
                t.submit()
                inflight[0] += 1
                transfers.append(t)

            start = time.monotonic()
            last_report = start
            last_bytes = 0
            while time.monotonic() - start < args.duration:
                ctx.handleEvents()
                now = time.monotonic()
                if now - last_report >= 1.0:
                    rate = (checker.bytes - last_bytes) / (now - last_report) / 1e6
                    print(f"  {now - start:5.1f}s  {rate:6.2f} MB/s  "
                          f"gaps={checker.gaps} lost={checker.words_lost * 4} B "
                          f"resyncs={checker.resyncs}")
                    last_report, last_bytes = now, checker.bytes
            elapsed = time.monotonic() - start

            done[0] = True
            while inflight[0]:
                ctx.handleEvents()
        finally:
            handle.releaseInterface(0)

    print()
    print(f"bytes      {checker.bytes} ({checker.bytes / 1e6:.1f} MB)")
    print(f"throughput {checker.bytes / elapsed / 1e6:.2f} MB/s over {elapsed:.1f}s")
    print(f"gaps       {checker.gaps}")
    print(f"lost       {checker.words_lost * 4} bytes")
    if checker.first_gap:
        print(f"first gap  expected 0x{checker.first_gap[0]:08x}, "
              f"got 0x{checker.first_gap[1]:08x}")
    return report(checker)


if __name__ == "__main__":
    sys.exit(main())
