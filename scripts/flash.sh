#!/usr/bin/env bash
# Flash the MSPM0G3507 over SWD with a SEGGER J-Link.
#   scripts/flash.sh [firmware.elf]
# Default: build/target/fw/bsp/ecg_fw.elf, else ecg_bsp_smoke.elf.
# Only the sectors the image occupies are erased, so the settings sector
# (last 1 KB, 0x1FC00) survives a reflash.
# Env: JLINK (JLinkExe), JLINK_DEVICE (MSPM0G3507), JLINK_SPEED (kHz, 4000),
#      JLINK_SN (probe serial number, if several are attached).
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
elf="${1:-}"
if [ -z "$elf" ]; then
    for c in build/target/fw/bsp/ecg_fw.elf build/target/fw/bsp/ecg_bsp_smoke.elf; do
        if [ -f "$root/$c" ]; then elf="$root/$c"; break; fi
    done
fi
if [ -z "$elf" ] || [ ! -f "$elf" ]; then
    echo "usage: $0 [firmware.elf]  (no default image found; build first)" >&2
    exit 1
fi

JLINK="${JLINK:-JLinkExe}"
DEVICE="${JLINK_DEVICE:-MSPM0G3507}"
SPEED="${JLINK_SPEED:-4000}"
OBJCOPY="${OBJCOPY:-${ARM_GCC_ROOT:-$HOME/ti/arm-gnu-toolchain-14.2.rel1-x86_64-arm-none-eabi}/bin/arm-none-eabi-objcopy}"
command -v "$OBJCOPY" >/dev/null 2>&1 || OBJCOPY=arm-none-eabi-objcopy

# J-Link Commander's loadfile is fed an Intel HEX made from the ELF.
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
hex="$tmp/image.hex"
"$OBJCOPY" -O ihex "$elf" "$hex"

cat >"$tmp/flash.jlink" <<EOF
r
h
loadfile $hex
r
g
qc
EOF

sel=()
if [ -n "${JLINK_SN:-}" ]; then sel=(-USB "$JLINK_SN"); fi

echo "Flashing $elf -> $DEVICE (SWD, ${SPEED} kHz)"
"$JLINK" "${sel[@]}" -NoGui 1 -ExitOnError 1 -device "$DEVICE" -if SWD \
    -speed "$SPEED" -autoconnect 1 -CommandFile "$tmp/flash.jlink"
