#!/usr/bin/env python3
"""Send numbered test packets from a TarangMini (TarangNet firmware) and log
ground truth, for over-the-air receiver validation.

Uses only the TarangNet data-send command (router or root broadcast, chosen
from the module's own read-back connection mode). It never writes
configuration, saves flash or restarts the module: frequency, data rate and
power stay whatever the module already has, and are read back and logged at
start. Every send is written to --out as one JSON line with host timestamps
and the module's raw response.

  tarangmini_send.py --port /dev/serial/by-id/usb-FTDI_... --out truth.ndjson \
      --duration 600 --interval 5 --jitter 1 --tag TM2
"""
import argparse
import json
import os
import random
import signal
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from tarangnet_api import TarangNet  # noqa: E402

stop = False


def _stop(*_):
    global stop
    stop = True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--duration", type=float, required=True)
    ap.add_argument("--interval", type=float, default=5.0)
    ap.add_argument("--jitter", type=float, default=1.0)
    ap.add_argument("--tag", default="TM")
    ap.add_argument("--count", type=int, default=0, help="stop after this many sends (0: duration only)")
    a = ap.parse_args()
    signal.signal(signal.SIGTERM, _stop)
    signal.signal(signal.SIGHUP, _stop)
    signal.signal(signal.SIGINT, _stop)

    tn = TarangNet(a.port)
    with open(a.out, "a") as out:
        def log(**kv):
            kv["host_ts"] = time.time()
            out.write(json.dumps(kv) + "\n")
            out.flush()

        try:
            firmware = tn.identify()
            tn.require_supported_firmware()
            settings = {}
            for name, cid in (("freq", 0x000F), ("rate", 0x0008), ("power", 0x000C), ("mode", 0x0015), ("eui", 0x0004)):
                r = tn.config_read(cid)
                settings[name] = r.get("value", b"").hex() if r.get("ok") else None
            freq_hz = int(settings["freq"], 16) if settings.get("freq") else None
            log(ev="settings", firmware=firmware, freq_hz=freq_hz, **settings)
            root = settings.get("mode") == "01"  # 0x0015: 1 = root, 0 = router
            start = time.time()
            seq = 0
            while not stop and time.time() - start < a.duration and (a.count <= 0 or seq < a.count):
                seq += 1
                payload = f"{a.tag} #{seq}".encode()
                t0 = time.time()
                resp = (tn.send_root(bytes.fromhex("ffffffffffffffff"), payload) if root
                        else tn.send_router(payload))
                log(ev="tx", seq=seq, payload=payload.decode(), payload_hex=payload.hex(),
                    host_send_ts=t0, freq_hz=freq_hz, mode="root" if root else "router",
                    response_hex=bytes(resp or b"").hex())
                wait = a.interval + random.uniform(-a.jitter, a.jitter)
                end = time.time() + max(0.5, wait)
                while not stop and time.time() < end:
                    time.sleep(0.1)
        finally:
            log(ev="stop")
            tn.close()


if __name__ == "__main__":
    main()
