#!/usr/bin/env python3
"""Send control-plane commands to the Bugslayer deck's RP2350 and print the replies.

    bslyctl.py ping ver stat "fx2 up" "clk 6000000"

Each argument is one command line. The port is found by USB ID (35F0:DB12);
pass --port to override, --serial to pick one deck of several.
"""
import argparse
import sys
import time

import serial
from serial.tools import list_ports

VID, PID = 0x35F0, 0xDB12


def find_port(serial_number=None):
    for p in list_ports.comports():
        if p.vid == VID and p.pid == PID and (serial_number is None or p.serial_number == serial_number):
            return p.device
    sys.exit(f"no Bugslayer deck control port ({VID:04x}:{PID:04x}) found")


def command(port, line, quiet_s=0.2, timeout_s=3.0):
    """Send one command and collect reply lines until the port goes quiet."""
    port.reset_input_buffer()
    port.write((line + "\n").encode())
    out = []
    deadline = time.monotonic() + timeout_s
    last = None
    while time.monotonic() < deadline:
        raw = port.readline()
        if raw:
            out.append(raw.decode(errors="replace").rstrip("\r\n"))
            last = time.monotonic()
        elif last is not None and time.monotonic() - last > quiet_s:
            break
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    ap.add_argument("--serial")
    ap.add_argument("commands", nargs="+")
    args = ap.parse_args()

    with serial.Serial(args.port or find_port(args.serial), timeout=0.05) as port:
        for line in args.commands:
            print(f"> {line}")
            for reply in command(port, line):
                print(reply)


if __name__ == "__main__":
    main()
