"""Byte sources for capture and plotting: static file, growing file, TCP socket, serial port."""
from __future__ import annotations

import socket
import time
from pathlib import Path
from typing import Callable, Iterator


def file_source(path: str | Path, chunk: int = 65536) -> Iterator[bytes]:
    """Read a static file to the end."""
    with open(path, "rb") as f:
        while True:
            b = f.read(chunk)
            if not b:
                return
            yield b


def follow_source(path: str | Path, poll_s: float = 0.05, from_start: bool = True,
                  idle_timeout_s: float | None = None, stop: Callable[[], bool] | None = None,
                  wait_for_file_s: float = 30.0) -> Iterator[bytes]:
    """Tail a growing file (tail -f). Yields b'' while idle so callers can update displays.

    Ends after `idle_timeout_s` without new data (if given) or when stop() is true.
    """
    p = Path(path)
    t0 = time.monotonic()
    while not p.exists():
        if stop and stop():
            return
        if time.monotonic() - t0 > wait_for_file_s:
            raise FileNotFoundError(f"{p} did not appear within {wait_for_file_s:.0f} s")
        time.sleep(0.1)
        yield b""
    last_data = time.monotonic()
    with open(p, "rb") as f:
        if not from_start:
            f.seek(0, 2)
        while not (stop and stop()):
            b = f.read(65536)
            if b:
                last_data = time.monotonic()
                yield b
            else:
                if idle_timeout_s is not None and time.monotonic() - last_data > idle_timeout_s:
                    return
                time.sleep(poll_s)
                yield b""


def tcp_source(hostport: str, timeout_s: float = 0.5, stop: Callable[[], bool] | None = None) -> Iterator[bytes]:
    """Connect to host:port and yield received data until the peer closes. Yields b'' on timeouts."""
    host, _, port = hostport.rpartition(":")
    with socket.create_connection((host or "127.0.0.1", int(port)), timeout=5) as s:
        s.settimeout(timeout_s)
        while not (stop and stop()):
            try:
                b = s.recv(65536)
            except socket.timeout:
                yield b""
                continue
            if not b:
                return
            yield b


def serial_source(port: str, baud: int = 115200, stop: Callable[[], bool] | None = None) -> Iterator[bytes]:
    """Read a serial port (pyserial is imported lazily)."""
    try:
        import serial  # type: ignore
    except ImportError as e:  # pragma: no cover
        raise RuntimeError("pyserial is not installed: pip install pyserial") from e
    with serial.Serial(port, baud, timeout=0.2) as s:  # pragma: no cover
        while not (stop and stop()):
            yield s.read(s.in_waiting or 1)


def add_source_args(ap) -> None:
    g = ap.add_mutually_exclusive_group(required=False)
    g.add_argument("--file", metavar="PATH", help="static file of raw stream bytes (or a saved .npz in plot mode)")
    g.add_argument("--follow", metavar="PATH", help="growing file written by JLinkRTTLogger (tail -f)")
    g.add_argument("--tcp", metavar="HOST:PORT", help="TCP socket carrying the raw stream")
    g.add_argument("--serial", metavar="PORT", help="serial port (needs pyserial)")
    ap.add_argument("--baud", type=int, default=115200, help="serial baud rate (default 115200)")
    ap.add_argument("--tail-only", action="store_true", help="with --follow: start at the end of the file")
    ap.add_argument("--idle-timeout", type=float, default=None, metavar="S",
                    help="with --follow: stop after S seconds without new data")


def open_source(args, stop: Callable[[], bool] | None = None) -> Iterator[bytes]:
    if args.file:
        return file_source(args.file)
    if args.follow:
        return follow_source(args.follow, from_start=not args.tail_only, idle_timeout_s=args.idle_timeout, stop=stop)
    if args.tcp:
        return tcp_source(args.tcp, stop=stop)
    if args.serial:
        return serial_source(args.serial, args.baud, stop=stop)
    raise SystemExit("choose a source: --file, --follow, --tcp or --serial")
