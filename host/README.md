# Host tools

**Bring-up and verification only.** The real capture/display application lives in a separate
repository. Nothing here should grow a UI.

| Tool | Stage | Purpose |
|---|---|---|
| `bslyctl.py` | 0+ | Send control-plane commands to the RP2350 (`35F0:DB12`) and print the replies |
| `fx2_counter_test.py` | 1 | Read the FX2 bulk IN endpoint, verify the 32-bit counter is gap-free, print MB/s. `--stall N` is the backpressure test, `--no-check` counts bytes only |
| `stage1.sh` | 1 | The whole stage 1 run: reboot the FX2 (self-boot; `BOOT=ram` for fx2tool), arm, 60 s stream, stall test |
| `99-bugslayer-deck.rules` | — | udev access to `35F0:DB11/12/13` and the FX2 boot ROM `04B4:8613` |
| `bsly.py` | 2+ | `capture`: arm over the control CDC, read the block stream from the RP2350's vendor interface, verify it (session, seq, sample continuity, OVERRUN accounting, END totals, synthetic content) and write a sigrok `.sr`. `--sink fx2` for the FX2, `--spi` for the SCK-clocked SPI stream (decoded, timed from raw16, and cross-checked against raw16 when it resolves SCK). `spi <file.sr>` decodes a saved one |
| `decode_ow.py` | 3 | Decode 1-Wire from a capture and print the 64-bit deck ROM ID |
| `decode_spi.py` | 4 | Decode SPI, auto-detecting which of IO_1–4 is CS |

Python + `libusb1`/`pyserial`/`numpy`, plus `fx2` for `fx2tool`. Keep them single-file and
dependency-light:

```
python3 -m venv host/.venv && host/.venv/bin/pip install libusb1 pyserial numpy fx2
```

`fx2_counter_test.py` checks clean data with numpy (~3 GB/s). A pure-Python per-word check
throttled the link to ~32 MB/s by resubmitting transfers late. `--no-check` counts bytes only,
which is the way to tell the link's limit from the tool's.

## Pairing two devices

The RP2350 and the FX2 are separate USB devices. Match them by `iSerialNumber` — the RP2350
patches its own serial into the FX2 descriptors when it serves the boot image. Do not match by
enumeration order, and do not assume the FX2 is present at the same moment as the RP2350: it
does not exist until RP2350 firmware releases `FX_RESET#`. Wait and retry.

## Status

`bslyctl.py`, `fx2_counter_test.py`, `stage1.sh` and `bsly.py capture` run against the rev-A
prototype (2026-09-29). The counter checker tells a real gap (a clean jump in the counter:
words lost, counted exactly) from a resync (the byte alignment moved: a partial word, from a
disarm/arm seam or a byte dropped at the FIFO-full edge), and a counter restart at a seam from
both.

A bulk transfer that times out can still carry data. The USB-sink reader keeps it. Dropping it
lost the SESSION block, which is typically sent alone after a quiet spell. The FX2 reader
(`bsly.py capture --sink fx2`) uses no timeout at all: cancelling a part-filled High Speed
transfer lost packets. Instead the deck ends transfers with a zero-length packet whenever it
goes quiet. The FX2 reader finds the FX2 by the control port's serial, reads whole packets
(one packet = one block), drops a short packet (a flushed partial block from an earlier
session), and verifies each transfer with numpy. Per-block Python cannot keep up with ~70k
blocks/s.

`decode_ow.py` and `decode_spi.py` are superseded: captures are `.sr`, so sigrok's decoders
apply directly (`sigrok-cli -i capture.sr -P i2c:scl=SCL:sda=SDA`).
