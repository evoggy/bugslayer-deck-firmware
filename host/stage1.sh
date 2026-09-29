#!/bin/sh
# Bring-up stage 1, end to end: reset the FX2, RAM-load its firmware, stream the
# RP2350's counter through it, verify on the PC. See ../docs/bringup-plan.md.
#
#   host/stage1.sh                 # 60 s at 37.5 MHz (clkdiv 1), then the stall test
#   IFCLK=6000000 DURATION=10 host/stage1.sh
set -e
cd "$(dirname "$0")"
PY=${PY:-.venv/bin/python}
FX2TOOL=${FX2TOOL:-.venv/bin/fx2tool}

$PY bslyctl.py disarm "fx2 down" "fx2 up" "clk ${IFCLK:-37500000}"
sleep 2   # boot ROM enumerates as 04b4:8613
$FX2TOOL -d 04b4:8613 load ../fx2/bugslayer-fx2.ihex
sleep 2   # re-enumerates as 35f0:db13
$PY bslyctl.py arm
$PY fx2_counter_test.py --ours --duration "${DURATION:-60}"
$PY fx2_counter_test.py --ours --stall "${STALL_ROUNDS:-10}"
