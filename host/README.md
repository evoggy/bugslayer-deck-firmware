# Host tools

**Bring-up and verification only.** The real capture/display application lives in a separate
repository. Nothing here should grow a UI.

| Tool | Stage | Purpose |
|---|---|---|
| `fx2_counter_test.py` | 1 | Read the FX2 bulk IN endpoint, verify the 32-bit counter is gap-free, print MB/s |
| `bsly.py` | 2+ | `arm` / `disarm` / `capture` over the RP2350 CDC; parse 512-byte blocks; check `seq`; write a capture file |
| `decode_ow.py` | 3 | Decode 1-Wire from a capture and print the 64-bit deck ROM ID |
| `decode_spi.py` | 4 | Decode SPI, auto-detecting which of IO_1–4 is CS |

Python + `pyusb`/`pyserial`. Keep them single-file and dependency-light.

## Pairing two devices

The RP2350 and the FX2 are separate USB devices. Match them by `iSerialNumber` — the RP2350
patches its own serial into the FX2 descriptors when it serves the boot image. Do not match by
enumeration order, and do not assume the FX2 is present at the same moment as the RP2350: it
does not exist until RP2350 firmware releases `FX_RESET#`. Wait and retry.

## Status

`fx2_counter_test.py` is written and its stream checker is unit-tested (clean stream, injected
gap, and the 2³² wrap). It has never talked to hardware. `pip install libusb1`.

The rest are not started.
