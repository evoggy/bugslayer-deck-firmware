# Host tools

**Bring-up and verification only.** The real capture/display application lives in a separate
repository. Nothing here should grow a UI.

| Tool | Stage | Purpose |
|---|---|---|
| `bslyctl.py` | 0+ | Send control-plane commands to the RP2350 (`35F0:DB12`) and print the replies |
| `fx2_counter_test.py` | 1 | Read the FX2 bulk IN endpoint, verify the 32-bit counter is gap-free, print MB/s. `--stall N` is the backpressure test, `--no-check` counts bytes only |
| `stage1.sh` | 1 | The whole stage 1 run: reset + RAM-load the FX2, arm, 60 s stream, stall test |
| `99-bugslayer-deck.rules` | — | udev access to `35F0:DB11/12/13` and the FX2 boot ROM `04B4:8613` |
| `bsly.py` | 2+ | `arm` / `disarm` / `capture` over the RP2350 CDC; parse 512-byte blocks; check `seq`; write a capture file |
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

`bslyctl.py`, `fx2_counter_test.py` and `stage1.sh` run against the rev-A prototype
(2026-09-29). The counter checker tells a real gap (a clean jump in the counter: words lost,
counted exactly) from a resync (the byte alignment moved: a partial word, from a disarm/arm
seam or a byte dropped at the FIFO-full edge), and a counter restart at a seam from both.

The rest are not started.
