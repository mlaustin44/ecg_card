#!/usr/bin/env python3
"""Read the firmware's RTT channels through a J-Link (the target keeps running).

Channel 0 (text log) goes to the terminal, and optionally to a file.
Channel 1 (binary stream frames, fw/include/stream_fmt.h) goes to a file that
stream_capture.py / stream_plot.py can follow.

    tools/rtt_read.py --elf build/target/fw/bsp/ecg_fw.elf
    tools/rtt_read.py --elf <elf> --stream build/rtt.bin
    tools/rtt_read.py --address 0x20201254 --no-log --stream build/rtt.bin

Uses the SEGGER J-Link library through the pylink-square package:
    pip install pylink-square
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_ELFS = (
    "build/target/fw/bsp/ecg_fw.elf",
    "build/target/fw/bsp/ecg_bsp_smoke.elf",
)
NM_CANDIDATES = (
    Path.home() / "ti/arm-gnu-toolchain-14.2.rel1-x86_64-arm-none-eabi/bin/arm-none-eabi-nm",
    Path("arm-none-eabi-nm"),
    Path("nm"),
)


def rtt_address_from_elf(elf: Path) -> int | None:
    """Address of the _SEGGER_RTT control block, from the ELF symbol table."""
    for nm in NM_CANDIDATES:
        try:
            out = subprocess.run(
                [str(nm), str(elf)], capture_output=True, text=True, check=True
            ).stdout
        except (OSError, subprocess.CalledProcessError):
            continue
        for line in out.splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[2] == "_SEGGER_RTT":
                return int(parts[0], 16)
    return None


def find_default_elf() -> Path | None:
    for rel in DEFAULT_ELFS:
        p = ROOT / rel
        if p.is_file():
            return p
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--elf", type=Path, help="firmware ELF that is in flash (for the RTT address)")
    ap.add_argument("--address", type=lambda s: int(s, 0), help="RTT control block address")
    ap.add_argument("--device", default="MSPM0G3507")
    ap.add_argument("--speed", type=int, default=4000, help="SWD speed in kHz")
    ap.add_argument("--serial", type=int, help="J-Link serial number, if several are attached")
    ap.add_argument("--stream", type=Path, help="write channel 1 (binary stream) to this file")
    ap.add_argument("--log-file", type=Path, help="also write channel 0 text to this file")
    ap.add_argument("--no-log", action="store_true", help="do not print channel 0")
    ap.add_argument("--duration", type=float, help="stop after this many seconds")
    args = ap.parse_args()

    try:
        import pylink
    except ImportError:
        print("pylink-square is not installed: pip install pylink-square", file=sys.stderr)
        return 2

    address = args.address
    if address is None:
        elf = args.elf or find_default_elf()
        if elf is not None:
            address = rtt_address_from_elf(elf)
            if address is not None:
                print(f"RTT control block at 0x{address:08X} ({elf})", file=sys.stderr)
    if address is None:
        print("No RTT address (--elf or --address): the J-Link will scan RAM", file=sys.stderr)

    jlink = pylink.JLink()
    try:
        jlink.open(serial_no=args.serial)
        jlink.set_tif(pylink.enums.JLinkInterfaces.SWD)
        jlink.connect(args.device, speed=args.speed)
    except pylink.errors.JLinkException as e:
        print(f"J-Link: cannot connect to {args.device}: {e}", file=sys.stderr)
        return 1

    stream_f = None
    log_f = None
    try:
        jlink.rtt_start(address)

        # The control block is found asynchronously.
        deadline = time.monotonic() + 5.0
        n_up = 0
        while time.monotonic() < deadline:
            try:
                n_up = jlink.rtt_get_num_up_buffers()
                if n_up > 0:
                    break
            except pylink.errors.JLinkRTTException:
                pass
            time.sleep(0.05)
        if n_up == 0:
            print("RTT control block not found (wrong ELF, or firmware not running?)",
                  file=sys.stderr)
            return 1
        print(f"RTT connected, {n_up} up channels. Ctrl-C to stop.", file=sys.stderr)

        if args.stream is not None:
            if n_up < 2:
                print("firmware has no channel 1; --stream ignored", file=sys.stderr)
            else:
                args.stream.parent.mkdir(parents=True, exist_ok=True)
                stream_f = open(args.stream, "wb")
        if args.log_file is not None:
            args.log_file.parent.mkdir(parents=True, exist_ok=True)
            log_f = open(args.log_file, "wb")

        t_end = None if args.duration is None else time.monotonic() + args.duration
        stream_bytes = 0
        while t_end is None or time.monotonic() < t_end:
            got = False
            data = bytes(jlink.rtt_read(0, 1024))
            if data:
                got = True
                if not args.no_log:
                    sys.stdout.write(data.decode("utf-8", errors="replace"))
                    sys.stdout.flush()
                if log_f is not None:
                    log_f.write(data)
                    log_f.flush()
            if stream_f is not None:
                data = bytes(jlink.rtt_read(1, 4096))
                if data:
                    got = True
                    stream_f.write(data)
                    stream_f.flush()
                    stream_bytes += len(data)
            if not got:
                time.sleep(0.005)
        if stream_f is not None:
            print(f"\n{stream_bytes} stream bytes written to {args.stream}", file=sys.stderr)
    except KeyboardInterrupt:
        print("", file=sys.stderr)
    except pylink.errors.JLinkException as e:
        print(f"J-Link error: {e}", file=sys.stderr)
        return 1
    finally:
        for f in (stream_f, log_f):
            if f is not None:
                f.close()
        try:
            jlink.rtt_stop()
        except Exception:  # noqa: BLE001 - shutting down regardless
            pass
        jlink.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
