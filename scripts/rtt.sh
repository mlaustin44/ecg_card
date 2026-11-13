#!/usr/bin/env bash
# Read the firmware's RTT channels through a J-Link (target keeps running).
#   scripts/rtt.sh [-e fw.elf] log            channel 0 text log to the terminal
#   scripts/rtt.sh [-e fw.elf] capture FILE   channel 1 binary stream -> FILE
#   scripts/rtt.sh [-e fw.elf] both FILE      both of the above at once
# The control block address comes from the _SEGGER_RTT symbol of the ELF
# (default: build/target/fw/bsp/ecg_fw.elf, else ecg_bsp_smoke.elf), which
# must be the image that is in flash.
# The text log is also kept in build/rtt-log.txt. Ctrl-C stops.
# This wraps tools/rtt_read.py, which needs Python with pylink-square:
#   python3 -m venv --system-site-packages .venv
#   .venv/bin/pip install -r tools/requirements.txt
# (SEGGER's JLinkRTTLogger is not used: it shut down without connecting on
# the bench laptop.)
# Env: JLINK_DEVICE (MSPM0G3507), JLINK_SPEED (4000), JLINK_SN.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
usage() { sed -n '2,15p' "$0" >&2; exit 1; }

py="$root/.venv/bin/python"
[ -x "$py" ] || py=python3

args=(--device "${JLINK_DEVICE:-MSPM0G3507}" --speed "${JLINK_SPEED:-4000}")
if [ -n "${JLINK_SN:-}" ]; then args+=(--serial "$JLINK_SN"); fi
if [ "${1:-}" = "-e" ]; then
    [ -n "${2:-}" ] || usage
    args+=(--elf "$2")
    shift 2
fi
mode="${1:-log}"
out="${2:-}"
mkdir -p "$root/build"

case "$mode" in
log)
    exec "$py" "$root/tools/rtt_read.py" "${args[@]}" --log-file "$root/build/rtt-log.txt"
    ;;
capture)
    [ -n "$out" ] || usage
    exec "$py" "$root/tools/rtt_read.py" "${args[@]}" --no-log --stream "$out"
    ;;
both)
    [ -n "$out" ] || usage
    exec "$py" "$root/tools/rtt_read.py" "${args[@]}" --log-file "$root/build/rtt-log.txt" --stream "$out"
    ;;
*)
    usage
    ;;
esac
