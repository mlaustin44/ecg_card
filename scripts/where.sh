#!/usr/bin/env bash
# Show where the firmware is: halt the CPU briefly, print the registers, the
# source line of PC and LR, and the RTT control block, then let it run on.
#   scripts/where.sh [fw.elf]
# Default ELF: build/target/fw/bsp/ecg_fw.elf, else ecg_bsp_smoke.elf. It must
# be the image that is in flash, or the source lines are meaningless.
# Env: JLINK (JLinkExe), JLINK_DEVICE (MSPM0G3507), JLINK_SPEED (4000), JLINK_SN.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
elf="${1:-}"
if [ -z "$elf" ]; then
    for c in build/target/fw/bsp/ecg_fw.elf build/target/fw/bsp/ecg_bsp_smoke.elf; do
        if [ -f "$root/$c" ]; then elf="$root/$c"; break; fi
    done
fi
if [ -z "$elf" ] || [ ! -f "$elf" ]; then
    echo "usage: $0 [fw.elf]" >&2
    exit 1
fi

JLINK="${JLINK:-JLinkExe}"
DEVICE="${JLINK_DEVICE:-MSPM0G3507}"
SPEED="${JLINK_SPEED:-4000}"
bin="${ARM_GCC_ROOT:-$HOME/ti/arm-gnu-toolchain-14.2.rel1-x86_64-arm-none-eabi}/bin"
NM="$bin/arm-none-eabi-nm"
A2L="$bin/arm-none-eabi-addr2line"

rtt="$("$NM" "$elf" | awk '$3 == "_SEGGER_RTT" { print "0x" $1 }')"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
{
    echo "h"
    echo "regs"
    # Two more samples 200 ms apart show whether the PC is moving.
    echo "g"; echo "sleep 200"; echo "h"; echo "regs"
    echo "g"; echo "sleep 200"; echo "h"; echo "regs"
    if [ -n "$rtt" ]; then echo "mem32 $rtt, 12"; fi
    echo "g"
    echo "qc"
} >"$tmp/where.jlink"

sel=()
if [ -n "${JLINK_SN:-}" ]; then sel=(-USB "$JLINK_SN"); fi
out="$tmp/out.txt"
"$JLINK" "${sel[@]}" -NoGui 1 -device "$DEVICE" -if SWD -speed "$SPEED" \
    -autoconnect 1 -CommandFile "$tmp/where.jlink" >"$out" 2>&1 || true

grep -E "^PC|R14|XPSR|MSP|^[0-9A-Fa-f]{8} =|Cannot|Could not|FAILED|Error" "$out" || cat "$out"

echo
echo "ELF: $elf"
n=0
grep -oE "PC *= *[0-9A-Fa-f]{8}" "$out" | grep -oE "[0-9A-Fa-f]{8}$" | while read -r pc; do
    n=$((n + 1))
    echo "sample $n: PC 0x$pc  $("$A2L" -f -p -e "$elf" "0x$pc")"
done
lr="$(grep -oE "R14\(LR\) *= *[0-9A-Fa-f]{8}" "$out" | tail -1 | grep -oE "[0-9A-Fa-f]{8}$" || true)"
if [ -n "$lr" ]; then
    echo "last LR 0x$lr  $("$A2L" -f -p -e "$elf" "0x$lr")"
fi
ipsr="$(grep -oE "IPSR *= *[0-9A-Fa-f]{3}" "$out" | tail -1 | grep -oE "[0-9A-Fa-f]{3}$" || true)"
case "$ipsr" in
    "" ) ;;
    000) echo "IPSR 000: thread mode (not in an exception)" ;;
    003) echo "IPSR 003: HardFault" ;;
    *)   echo "IPSR $ipsr: in exception/interrupt number 0x$ipsr" ;;
esac
if [ -n "$rtt" ]; then
    echo "RTT block at $rtt: first words should read 47474553 52205245 00005454 ('SEGGER RTT');"
    echo "all zero means hal_init() has not reached (or keeps restarting before) RTT init."
fi
