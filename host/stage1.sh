#!/bin/sh
# Bring-up stage 1, end to end: reboot the FX2, stream the RP2350's counter
# through it, verify on the PC. See ../docs/bringup-plan.md.
#
#   host/stage1.sh                 # self-boot, 60 s at 37.5 MHz, then the stall test
#   IFCLK=6000000 DURATION=10 host/stage1.sh
#   BOOT=ram host/stage1.sh        # RAM-load fx2/bugslayer-fx2.ihex with fx2tool
#                                  # instead of the image built into the RP2350
set -e
cd "$(dirname "$0")"
PY=${PY:-.venv/bin/python}
FX2TOOL=${FX2TOOL:-.venv/bin/fx2tool}

if [ "${BOOT:-c2}" = ram ]; then
    $PY bslyctl.py disarm "fx2 boot rom" "fx2 reboot" "clk ${IFCLK:-37500000}"
    sleep 2   # boot ROM enumerates as 04b4:8613
    $FX2TOOL -d 04b4:8613 load ../fx2/bugslayer-fx2.ihex
else
    # The RP2350 serves the C2 image; the FX2 boots our firmware by itself.
    $PY bslyctl.py disarm "fx2 boot c2" "fx2 reboot" "clk ${IFCLK:-37500000}"
fi
sleep 2   # 35f0:db13 enumerates
$PY bslyctl.py arm stat
$PY fx2_counter_test.py --ours --duration "${DURATION:-60}"
$PY fx2_counter_test.py --ours --stall "${STALL_ROUNDS:-10}"
