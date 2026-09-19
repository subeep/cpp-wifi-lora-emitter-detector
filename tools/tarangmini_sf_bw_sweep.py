#!/usr/bin/env python3
"""Firmware-checked TarangNet SF/BW sweep - documented v0_0_4, plus
v0_0_6 ("TarangNet_TN_LW_STD_WL") accepted as the same command family
with a version bump (see tarangnet_api.py's SUPPORTED_FIRMWARE comment
for why that's an assumption, not an independently confirmed fact).

Inspect first:
    python3 tools/tarangmini_sf_bw_sweep.py --port /dev/ttyUSB0 --inspect

LW-S201 LoRaWAN firmware is identified but cannot transmit with this tool:
its matching API manual is not available. No writes occur on unsupported firmware.
Use API mode; a transparent serial bridge is not supported. The sweep changes
persistent data-rate settings and restores the original setting on exit.
"""
import argparse
import sys
import time

from tarangnet_api import TarangNet, DATA_RATE_NAMES

# (value_byte, spreading_factor, bandwidth_hz) - straight from the
# TarangNet API doc's Default Data Rate table (cmd 0x08). Note: the doc
# itself lists BOTH 0x01 and 0x02 as "SF12, BW250" (not a transcription
# slip here - that's how the source table reads) rather than the BW500
# you'd expect completing the SF12 group like every other SF does -
# included exactly as documented since this sweep is one way to notice
# whether 0x02 actually behaves differently from 0x01 in practice.
DATA_RATE_TABLE = [
    (0x00, 12, 125_000),
    (0x01, 12, 250_000),
    (0x02, 12, 250_000),  # doc says BW250 again - see note above
    (0x03, 11, 125_000),
    (0x04, 11, 250_000),
    (0x05, 11, 500_000),
    (0x06, 10, 125_000),
    (0x07, 10, 250_000),
    (0x08, 10, 500_000),
    (0x09, 9, 125_000),
    (0x0A, 9, 250_000),
    (0x0B, 9, 500_000),
    (0x0C, 8, 125_000),
    (0x0D, 8, 250_000),
    (0x0E, 8, 500_000),
    (0x0F, 7, 125_000),
    (0x10, 7, 250_000),
    (0x11, 7, 500_000),
    (0x12, 6, 125_000),
    (0x13, 6, 250_000),
    (0x14, 6, 500_000),
    (0x15, 5, 125_000),
    (0x16, 5, 250_000),
    (0x17, 5, 500_000),
]

CMD_DEFAULT_DATA_RATE = 0x0008
CMD_DEFAULT_FREQUENCY = 0x000F
BROADCAST = bytes.fromhex("ffffffffffffffff")


def read_value(net, command, size):
    response = net.config_read(command)
    value = response.get("value", b"")
    if not response.get("ok") or len(value) != size:
        raise RuntimeError(f"Read 0x{command:04x}: expected {size} bytes, got {response}")
    return bytes(value)


def checked(response, action):
    if not response.get("ok"):
        raise RuntimeError(f"{action} failed: {response}")


def set_data_rate(net: TarangNet, value_byte: int) -> None:
    checked(net.config_write(CMD_DEFAULT_DATA_RATE, [value_byte]), "Set data rate")
    checked(net.flash_write(), "Save configuration")
    checked(net.restart(), "Restart")
    time.sleep(3.0)
    firmware = net.identify()
    net.require_supported_firmware()
    if read_value(net, CMD_DEFAULT_DATA_RATE, 1) != bytes([value_byte]):
        raise RuntimeError(f"Rate readback differs from requested 0x{value_byte:02x} ({firmware})")


def run(net, args):
    firmware = net.identify()
    print(f"Firmware: {firmware}", flush=True)
    if args.inspect:
        if not net.verified_firmware:
            print("Inspection only. LoRaWAN/unknown firmware: no sweep, TX, flash or restore writes issued.")
            return 0
    net.require_supported_firmware()
    frequency = int.from_bytes(read_value(net, CMD_DEFAULT_FREQUENCY, 4), "big")
    if not 863_000_000 <= frequency <= 870_000_000:
        raise RuntimeError(f"Unexpected ST22LR01 frequency readback: {frequency} Hz")
    print(f"Module frequency: {frequency / 1e6:.6f} MHz; set the GUI frequency lock to this value.")
    if args.read_freq:
        return 0
    mode = read_value(net, 0x0015, 1)[0]
    if mode not in (0, 1):
        raise RuntimeError(f"Unsupported connection type: {mode}")
    original = read_value(net, CMD_DEFAULT_DATA_RATE, 1)[0]
    if original not in DATA_RATE_NAMES:
        raise RuntimeError(f"Unknown original data rate 0x{original:02x}; cannot safely restore")
    print(f"Connection: {'root' if mode else 'router'}; original rate: 0x{original:02x} ({DATA_RATE_NAMES[original]})")
    if args.inspect:
        return 0
    combos = args.combos
    dirty = False
    try:
        for value_byte, sf, bw_hz in combos:
            label = f"SF{sf:02d}_BW{bw_hz // 1000}"
            print(f"=== {label} (0x{value_byte:02X}; manual mapping, not measured) ===", flush=True)
            if read_value(net, CMD_DEFAULT_DATA_RATE, 1) != bytes([value_byte]):
                # Set before writing: timeout/interrupt may occur after the device applied a write.
                dirty = True
                set_data_rate(net, value_byte)
            if read_value(net, CMD_DEFAULT_FREQUENCY, 4) != frequency.to_bytes(4, "big"):
                raise RuntimeError("Frequency changed during sweep; stopping")
            if read_value(net, 0x0015, 1) != bytes([mode]):
                raise RuntimeError("Connection mode changed during sweep; stopping")
            payload = f"{label}_TEST".encode("ascii")
            for rep in range(args.reps):
                response = (net.send_root(BROADCAST, payload, wait_s=args.tx_wait) if mode else
                            net.send_router(payload, wait_s=args.tx_wait))
                result = net.parse_send_response(response)
                if not result["ok"]:
                    raise RuntimeError(f"TX rejected/unconfirmed: {result['error']}; response={response.hex()}. "
                                       "Stopping; no successful RF transmission claimed.")
                print(f"  [{time.strftime('%H:%M:%S')}] rep {rep}: module uplink ACK, "
                      f"payload={payload!r}, response={response.hex()} (SDR reception not verified)", flush=True)
                time.sleep(args.gap)
    finally:
        # Unsupported firmware never reaches this block. Restoration is needed only
        # after a configuration write was attempted; always verify identity again.
        if dirty:
            print(f"Restoring original rate 0x{original:02X}...", flush=True)
            try:
                net.identify()
                net.require_supported_firmware()
                set_data_rate(net, original)
                print("Original rate restored and verified.")
            except Exception as exc:
                print(f"RESTORE FAILED: {exc}. Current device settings are unverified.", file=sys.stderr)
                raise
    return 0


def main(argv=None) -> int:
    import math
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="/dev/ttyUSB0", help="Prefer the stable /dev/serial/by-id/... path")
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--gap", type=float, default=1.5)
    ap.add_argument("--tx-wait", type=float, default=5.0, help="Seconds to wait before reading uplink ACK")
    ap.add_argument("--only", help="Comma-separated hex rate indices; explicit selection permits experimental table entries")
    group = ap.add_mutually_exclusive_group()
    group.add_argument("--inspect", action="store_true", help="Read identity/settings only; never write or transmit")
    group.add_argument("--read-freq", action="store_true", help="Read frequency only after firmware validation")
    args = ap.parse_args(argv)
    if args.reps < 1 or not math.isfinite(args.gap) or args.gap < 0.5 or not math.isfinite(args.tx_wait) or args.tx_wait < 0.5:
        ap.error("--reps must be positive; --gap and --tx-wait must be finite and at least 0.5 seconds")
    # The manual duplicates SF12/BW250 at 0x02; SF5/6 appear only in its read table.
    args.combos = [c for c in DATA_RATE_TABLE if c[0] < 0x12 and c[0] != 0x02]
    if args.only:
        try:
            wanted = {int(v.strip(), 16) for v in args.only.split(",")}
        except ValueError:
            ap.error("--only expects comma-separated hexadecimal rate indices")
        if not wanted or not wanted.issubset(DATA_RATE_NAMES):
            ap.error("--only contains an unknown rate index")
        args.combos = [c for c in DATA_RATE_TABLE if c[0] in wanted]
        if any(v == 2 or v >= 0x12 for v in wanted):
            print("Warning: explicitly selected ambiguous/experimental manual rate entries.")
    net = None
    try:
        net = TarangNet(args.port)
        return run(net, args)
    except KeyboardInterrupt:
        print("Interrupted. No further sweep packets will be requested.", file=sys.stderr)
        return 130
    except Exception as exc:
        print(f"Sweep stopped: {exc}", file=sys.stderr)
        return 1
    finally:
        if net is not None:
            net.close()


if __name__ == "__main__":
    raise SystemExit(main())
