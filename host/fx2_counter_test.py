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

try:
    import usb1
except ImportError:
    sys.exit("libusb1 is required: pip install libusb1")

# Bring-up default: with the RP2350 silent on I2C the FX2 falls back to its
# ROM identity and the firmware is RAM-loaded with fx2tool. Once the boot
# image comes from the emulated EEPROM this becomes our own VID/PID.
DEFAULT_VID = 0x04B4
DEFAULT_PID = 0x8613

EP_IN = 0x86
XFER_SIZE = 64 * 1024
XFER_COUNT = 16


class Checker:
    """Verifies a stream of little-endian uint32 counter words."""

    def __init__(self):
        self.expected = None
        self.partial = b""
        self.bytes = 0
        self.gaps = 0
        self.words_lost = 0
        self.first_gap = None

    def feed(self, data):
        self.bytes += len(data)
        buf = self.partial + data
        n = len(buf) // 4
        for (word,) in struct.iter_unpack("<I", buf[:n * 4]):
            if self.expected is not None and word != self.expected:
                # Modular arithmetic so a wrap past 2**32 is not a false gap.
                lost = (word - self.expected) & 0xFFFFFFFF
                self.gaps += 1
                self.words_lost += lost
                if self.first_gap is None:
                    self.first_gap = (self.expected, word)
            self.expected = (word + 1) & 0xFFFFFFFF
        self.partial = buf[n * 4:]


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--vid", type=lambda s: int(s, 0), default=DEFAULT_VID)
    p.add_argument("--pid", type=lambda s: int(s, 0), default=DEFAULT_PID)
    p.add_argument("--duration", type=float, default=10.0, help="seconds")
    p.add_argument("--serial", help="only match this iSerialNumber")
    args = p.parse_args()

    checker = Checker()
    inflight = [0]

    def on_transfer(transfer):
        status = transfer.getStatus()
        if status == usb1.TRANSFER_COMPLETED:
            checker.feed(transfer.getBuffer()[:transfer.getActualLength()])
        elif status != usb1.TRANSFER_TIMED_OUT:
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
        try:
            transfers = []
            for _ in range(XFER_COUNT):
                t = handle.getTransfer()
                t.setBulk(EP_IN, XFER_SIZE, callback=on_transfer, timeout=1000)
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
                          f"gaps={checker.gaps} lost={checker.words_lost * 4} B")
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

    # Stage 1 passes on zero gaps. Throughput is reported, not asserted: the
    # PIO engine is deliberately conservative until the link is proven.
    ok = checker.gaps == 0 and checker.bytes > 0
    print("\nPASS" if ok else "\nFAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
