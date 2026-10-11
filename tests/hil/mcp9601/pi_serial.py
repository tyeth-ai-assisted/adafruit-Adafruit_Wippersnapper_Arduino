#!/usr/bin/env python3
"""Pi USB-serial front-end for the ESP32-S31 MCP9601 HIL console.

Listing never opens a port. Writing requires --port and an explicit command.
Logs serial replies with timestamps. No automatic scan, power cycle or flash.
"""
import argparse
import datetime as dt
import json
import pathlib
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    raise SystemExit("Install pyserial: sudo apt install python3-serial")

COMMANDS = (
    "help", "levels", "wiring", "id", "scan", "probe50", "probemcp",
    "captureid", "capture50", "wait10", "reinit", "stop", "pulse1",
    "pulse9", "pulse18",
)
RISKY = set(COMMANDS) - {"help", "levels"}
BAUD_DEFAULT = 115200


def ports():
    entries = sorted(list_ports.comports(), key=lambda x: x.device)
    if not entries:
        print("No ttyUSB/ttyACM/serial devices detected")
    for item in entries:
        vid = f"{item.vid:04x}" if item.vid is not None else "????"
        pid = f"{item.pid:04x}" if item.pid is not None else "????"
        print(f"{item.device}  {vid}:{pid}  {item.description}  "
              f"SER={item.serial_number or '?'}  LOC={item.location or '?'}")


def run(args):
    if args.action in RISKY and not args.allow_io:
        raise SystemExit("Refusing bus-affecting command without --allow-io")
    port = args.port
    if not port:
        raise SystemExit("--port required for all actions except list")
    if args.action == "listen" and args.allow_io:
        raise SystemExit("--allow-io is not needed to listen")
    output = []
    def record(msg, direction):
        now = dt.datetime.now(dt.timezone.utc).isoformat(timespec="milliseconds")
        data = {"utc": now, "port": port, "direction": direction, "text": msg}
        output.append(data)
        print(f"{now} {direction:4s} {msg}", flush=True)

    # Some USB-UART bridges can reset the ESP32 as DTR/RTS change.
    # Configure inactive control signals before opening; still verify your bridge.
    device = serial.Serial(port=None, baudrate=args.baud, timeout=0.1, write_timeout=2)
    device.dtr = False
    device.rts = False
    device.port = port
    device.open()
    try:
        if args.action != "listen":
            payload = args.action + "\n"
            record(payload.rstrip(), "TX")
            device.write(payload.encode("ascii"))
            device.flush()
        deadline = time.monotonic() + args.duration
        pending = bytearray()
        while time.monotonic() < deadline:
            data = device.read(512)
            if not data:
                continue
            pending.extend(data)
            while b"\n" in pending:
                line, _, rest = pending.partition(b"\n")
                pending = bytearray(rest)
                record(line.rstrip(b"\r").decode("utf-8", "replace"), "RX")
        if pending:
            record(pending.decode("utf-8", "replace"), "RX")
    finally:
        device.close()
        if args.jsonl:
            target = pathlib.Path(args.jsonl).expanduser()
            target.parent.mkdir(parents=True, exist_ok=True)
            with target.open("a", encoding="utf-8") as fh:
                for event in output:
                    fh.write(json.dumps(event) + "\n")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("action", choices=("list", "listen") + COMMANDS)
    p.add_argument("--port", help="Prefer the stable /dev/serial/by-id/... symlink")
    p.add_argument("--baud", type=int, default=BAUD_DEFAULT)
    p.add_argument("--duration", type=float, default=3.0,
                   help="Read serial output for N seconds (default 3); "
                        "use 13 or more for wait10, longer for scan")
    p.add_argument("--jsonl", help="Append timestamped TX/RX events to this file")
    p.add_argument("--allow-io", action="store_true",
                   help="Required to send commands that touch the I2C bus")
    args = p.parse_args()
    if args.duration <= 0:
        p.error("--duration must be positive")
    if args.action == "list":
        ports()
    else:
        run(args)


if __name__ == "__main__":
    main()
