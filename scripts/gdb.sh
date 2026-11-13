#!/usr/bin/env bash
# Debug the MSPM0G3507 with arm-none-eabi-gdb through J-Link GDB Server.
#   scripts/gdb.sh [fw.elf] [--no-load]
# Starts JLinkGDBServerCLExe (SWD, port 2331), then gdb: reset, load the
# image (unless --no-load), reset again and stop at main.
# The GDB server also serves RTT channel 0 on telnet localhost:19021.
# Env: JLINK_DEVICE (MSPM0G3507), JLINK_SPEED (4000), JLINK_SN, GDB_PORT (2331).
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
elf=""
load=1
for a in "$@"; do
    case "$a" in
    --no-load) load=0 ;;
    *) elf="$a" ;;
    esac
done
if [ -z "$elf" ]; then
    for c in build/target/fw/bsp/ecg_fw.elf build/target/fw/bsp/ecg_bsp_smoke.elf; do
        if [ -f "$root/$c" ]; then elf="$root/$c"; break; fi
    done
fi
if [ -z "$elf" ] || [ ! -f "$elf" ]; then
    echo "usage: $0 [firmware.elf] [--no-load]" >&2
    exit 1
fi

DEVICE="${JLINK_DEVICE:-MSPM0G3507}"
SPEED="${JLINK_SPEED:-4000}"
PORT="${GDB_PORT:-2331}"
SERVER="${JLINK_GDB_SERVER:-JLinkGDBServerCLExe}"
GDB="${GDB:-${ARM_GCC_ROOT:-$HOME/ti/arm-gnu-toolchain-14.2.rel1-x86_64-arm-none-eabi}/bin/arm-none-eabi-gdb}"
command -v "$GDB" >/dev/null 2>&1 || GDB=arm-none-eabi-gdb

sel=()
if [ -n "${JLINK_SN:-}" ]; then sel=(-select "USB=$JLINK_SN"); fi

"$SERVER" "${sel[@]}" -device "$DEVICE" -if SWD -speed "$SPEED" -port "$PORT" \
    -nogui -singlerun -silent &
server=$!
trap 'kill "$server" 2>/dev/null || true' EXIT
sleep 1

cmds=(-ex "target extended-remote :$PORT" -ex "monitor reset" -ex "monitor halt")
if [ "$load" = 1 ]; then cmds+=(-ex "load" -ex "monitor reset"); fi
cmds+=(-ex "break main" -ex "continue")
"$GDB" -q "$elf" "${cmds[@]}"
