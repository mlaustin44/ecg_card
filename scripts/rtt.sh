#!/usr/bin/env bash
# Read the firmware's RTT channels through a J-Link (target keeps running).
#   scripts/rtt.sh [-e fw.elf] log            channel 0 text log to the terminal
#   scripts/rtt.sh [-e fw.elf] capture FILE   channel 1 binary stream -> FILE
#   scripts/rtt.sh [-e fw.elf] both FILE      both of the above at once
# With -e (or a default ELF in build/target), the control block address is
# taken from the _SEGGER_RTT symbol instead of letting the J-Link scan RAM.
# The text log is also kept in build/rtt-log.txt. Ctrl-C stops everything.
# Env: JLINK_DEVICE (MSPM0G3507), JLINK_SPEED (4000), JLINK_SN.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
DEVICE="${JLINK_DEVICE:-MSPM0G3507}"
SPEED="${JLINK_SPEED:-4000}"
LOGGER="${JLINK_RTT_LOGGER:-JLinkRTTLogger}"
NM="${ARM_GCC_ROOT:-$HOME/ti/arm-gnu-toolchain-14.2.rel1-x86_64-arm-none-eabi}/bin/arm-none-eabi-nm"
command -v "$NM" >/dev/null 2>&1 || NM=arm-none-eabi-nm

usage() { sed -n '2,9p' "$0" >&2; exit 1; }

elf=""
if [ "${1:-}" = "-e" ]; then elf="${2:-}"; shift 2 || usage; fi
mode="${1:-log}"
out="${2:-}"
if [ -z "$elf" ]; then
    for c in build/target/fw/bsp/ecg_fw.elf build/target/fw/bsp/ecg_bsp_smoke.elf; do
        if [ -f "$root/$c" ]; then elf="$root/$c"; break; fi
    done
fi

common=(-Device "$DEVICE" -If SWD -Speed "$SPEED")
if [ -n "${JLINK_SN:-}" ]; then common+=(-USB "$JLINK_SN"); fi
if [ -n "$elf" ] && [ -f "$elf" ]; then
    addr="$("$NM" "$elf" 2>/dev/null | awk '$3 == "_SEGGER_RTT" { print "0x" $1 }')"
    if [ -n "$addr" ]; then
        common+=(-RTTAddress "$addr")
        echo "RTT control block at $addr ($elf)" >&2
    fi
fi

pids=()
cleanup() { for p in "${pids[@]}"; do kill "$p" 2>/dev/null || true; done; }
trap cleanup EXIT INT TERM

# JLinkRTTLogger quits on any key press: give it a stdin that never sends one.
run_logger() { # channel file
    sleep infinity | "$LOGGER" "${common[@]}" -RTTChannel "$1" "$2" &
    pids+=("$!")
}

text_log() {
    local log="$root/build/rtt-log.txt"
    mkdir -p "$root/build"
    : >"$log"
    run_logger 0 "$log" >/dev/null
    tail -n +1 -f "$log"
}

case "$mode" in
log)
    text_log
    ;;
capture)
    [ -n "$out" ] || usage
    sleep infinity | "$LOGGER" "${common[@]}" -RTTChannel 1 "$out"
    ;;
both)
    [ -n "$out" ] || usage
    run_logger 1 "$out" >/dev/null
    text_log
    ;;
*)
    usage
    ;;
esac
