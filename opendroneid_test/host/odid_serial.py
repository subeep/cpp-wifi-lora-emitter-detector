#!/usr/bin/env python3
"""Talk to the ESP32-S3 OpenDroneID test transmitter over its serial console.

    odid_serial.py check            sync the clock, grab the frames being sent and
                                    decode them with build/wifi_remote_id_inspect
    odid_serial.py cmd ch 11        send any console command (see "help")
    odid_serial.py monitor          print the transmitter's console output

Needs pyserial (python3 -m pip install pyserial, or the ESP-IDF Python env).
"""
import argparse
import json
import pathlib
import subprocess
import sys
import time

import serial

REPO = pathlib.Path(__file__).resolve().parents[2]
INSPECT = REPO / "build" / "wifi_remote_id_inspect"


def open_port(port):
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.2
    s.dtr = s.rts = False  # keep the auto-reset circuit from rebooting the board
    s.open()
    return s


def send(s, line):
    s.write((line + "\n").encode())


def read_lines(s, seconds):
    end, buf = time.time() + seconds, b""
    while time.time() < end:
        buf += s.read(4096)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            yield line.decode(errors="replace").strip()


def describe(pack):
    out = [f"  {pack['transport']} from {pack['transmitter']}, protocol v{pack['protocol_version']}, "
           f"counter {pack['message_counter']}, {pack['message_count']} messages"]
    for m in pack["messages"]:
        t = m["type"]
        if t == "Basic ID":
            detail = f"{m['id_type_name']} {m.get('reported_id')} ({m['ua_type_name']})"
        elif t == "Location":
            detail = (f"{m['latitude']:.7f}, {m['longitude']:.7f}  height {m['height_m']} m  "
                      f"{m['speed_horizontal_m_s']} m/s @ {m['direction_deg']} deg  {m['status_name']}")
        elif t == "System":
            detail = f"operator {m['latitude']:.7f}, {m['longitude']:.7f}  ts2019 {m['timestamp_2019_s']}"
        elif t == "Self ID":
            detail = repr(m["description"])
        elif t == "Operator ID":
            detail = repr(m["operator_id"])
        else:
            detail = ""
        out.append(f"    {t:12s} {detail}")
    return "\n".join(out)


def check(s, inspect):
    # Opening the CP210x port can still pulse the reset line; let the board boot first.
    for _ in read_lines(s, 1.5):
        pass
    send(s, f"time {int(time.time())}")
    send(s, "status")
    send(s, "hex 2")
    frames, config = [], None
    for line in read_lines(s, 2.0):
        if line.startswith("CONFIG"):
            config = line
        elif line.startswith("MPDU "):
            _, kind, hexstr = line.split(" ", 2)
            frames.append((kind, hexstr))
    if config:
        print(config)
    if not frames:
        print("no frames captured from the serial console", file=sys.stderr)
        return 1
    if not inspect.exists():
        print(f"{inspect} not found; build the main project first", file=sys.stderr)
        return 1
    result = subprocess.run([str(inspect)], input="\n".join(h for _, h in frames) + "\n",
                            capture_output=True, text=True, check=True)
    failures = 0
    for (kind, _), out in zip(frames, result.stdout.splitlines()):
        packs = json.loads(out)
        print(f"{kind} frame: {'decoded' if packs else 'NOT decoded'}")
        failures += not packs
        for p in packs:
            print(describe(p))
    return 1 if failures else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="/dev/ttyUSB0")
    ap.add_argument("--inspect", type=pathlib.Path, default=INSPECT)
    sub = ap.add_subparsers(dest="action", required=True)
    sub.add_parser("check")
    c = sub.add_parser("cmd")
    c.add_argument("words", nargs="+")
    m = sub.add_parser("monitor")
    m.add_argument("--seconds", type=float, default=1e9)
    args = ap.parse_args()

    with open_port(args.port) as s:
        if args.action == "check":
            return check(s, args.inspect)
        if args.action == "cmd":
            send(s, " ".join(args.words))
            for line in read_lines(s, 1.0):
                if not line.startswith(("STATUS", "MPDU")):
                    print(line)
            return 0
        try:
            for line in read_lines(s, args.seconds):
                print(line, flush=True)
        except KeyboardInterrupt:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
