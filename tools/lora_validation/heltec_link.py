#!/usr/bin/env python3
"""Drive the Heltec LoRa test transmitter (tools/heltec_lora_telemetry) and log
ground truth.

Every serial line from the board is written to --out as one JSON object with
the host receive time added ("host_ts", Unix seconds). Commands sent by this
script are logged as {"ev":"host_cmd"}. Steps run at fixed offsets from start:

  heltec_link.py --out truth.ndjson --duration 600 \
      --step "0:RUN A1" --step "0:MODE TEL" --step "590:MODE OFF"

Host macros (steps starting with "@"), built from what the board logged:
  @REPLAY_AGE <s>  resend (RAW) the newest LoRaWAN uplink at least <s> seconds old,
                   on its original frequency
  @REUSE           send an uplink reusing the newest uplink's FCnt with new bytes

The board is always left in MODE OFF when the script exits.
Needs pyserial (the PlatformIO venv has it).
"""
import argparse
import json
import signal
import sys
import time

import serial


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyUSB0")
    ap.add_argument("--out", required=True)
    ap.add_argument("--duration", type=float, required=True)
    ap.add_argument("--step", action="append", default=[], help="SECONDS:COMMAND")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args()

    steps = []
    for s in a.step:
        at, _, cmd = s.partition(":")
        steps.append((float(at), cmd.strip()))
    steps.sort(key=lambda x: x[0])

    # Turn the transmitter off on SIGTERM/SIGHUP too (the finally block below).
    def stop(signum, _frame):
        raise KeyboardInterrupt(f"signal {signum}")
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGHUP, stop)

    port = serial.Serial()
    port.port, port.baudrate, port.timeout = a.port, 115200, 0.1
    port.dtr = False  # do not reset the board on open
    port.rts = False
    port.open()
    out = open(a.out, "a", buffering=1)
    buf = b""
    counts = {}

    def log(obj):
        obj["host_ts"] = time.time()
        out.write(json.dumps(obj) + "\n")

    sent_frames = []  # LoRaWAN uplinks the board logged: (host_ts, obj)

    def send(cmd):
        if cmd.startswith("@"):
            op, _, arg = cmd[1:].partition(" ")
            wan = [f for f in sent_frames if f[1].get("kind") in ("wan", "wanf")]
            if op == "REPLAY_AGE":
                old = [f for f in wan if f[0] <= time.time() - float(arg)]
                if not old:
                    log({"ev": "host_macro_skipped", "cmd": cmd})
                    return
                o = old[-1][1]
                log({"ev": "host_macro", "cmd": cmd, "replayed_seq": o["seq"], "replayed_fcnt": o.get("fcnt")})
                cmd = f"RAW {o['freq']:.4f} {o['hex']}"
            elif op == "REUSE":
                if not wan:
                    log({"ev": "host_macro_skipped", "cmd": cmd})
                    return
                o = wan[-1][1]
                log({"ev": "host_macro", "cmd": cmd, "reused_fcnt": o.get("fcnt")})
                cmd = f"WANF {o['fcnt']} " + ("REUSED " + str(o["seq"])).encode().hex().upper()
        port.write((cmd + "\n").encode())
        port.flush()
        log({"ev": "host_cmd", "cmd": cmd})

    def pump(deadline):
        nonlocal buf
        while time.time() < deadline:
            buf += port.read(4096)
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode(errors="replace").strip()
                if text:
                    try:
                        obj = json.loads(text)
                    except json.JSONDecodeError:
                        obj = {"ev": "text", "text": text}
                    log(obj)
                    if obj.get("ev") == "status":
                        return True
        return False

    # Opening the port can reset the board: wait for its boot status line
    # (or ask for one) before the step clock starts.
    if not pump(time.time() + 3.0):
        send("STATUS")
        pump(time.time() + 2.0)
    t0 = time.time()
    try:
        i = 0
        while True:
            now = time.time() - t0
            while i < len(steps) and steps[i][0] <= now:
                send(steps[i][1])
                i += 1
            if now >= a.duration and i >= len(steps):
                break
            buf += port.read(4096)
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode(errors="replace").strip()
                if not text:
                    continue
                try:
                    obj = json.loads(text)
                except json.JSONDecodeError:
                    obj = {"ev": "text", "text": text}
                log(obj)
                ev = obj.get("ev")
                counts[ev] = counts.get(ev, 0) + 1
                if ev == "tx":
                    sent_frames.append((obj["host_ts"], obj))
                if not a.quiet and ev in ("tx", "err", "bad", "boot"):
                    brief = {k: obj.get(k) for k in ("ev", "seq", "kind", "freq", "sf", "bw", "fcnt", "status", "cmd", "what")}
                    print(f"[{now:6.1f}s] {json.dumps(brief)}", flush=True)
    finally:
        send("MODE OFF")
        # A packet already on air (SF12 is ~2 s) logs its tx line after the ack.
        end = time.time() + 3.0
        while time.time() < end:
            buf += port.read(4096)
        for line in buf.split(b"\n"):
            if line.strip():
                try:
                    log(json.loads(line.decode(errors="replace")))
                except json.JSONDecodeError:
                    pass
        port.close()
        out.close()
    print(json.dumps({"logged": counts}), flush=True)


if __name__ == "__main__":
    sys.exit(main())
