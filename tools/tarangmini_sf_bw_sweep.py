#!/usr/bin/env python3
"""Reconfigures a real TarangMini's Default Data Rate through every
documented Spreading-Factor / Bandwidth combination and sends a burst of
test packets at each, so rf_monitor_gui's LoRa PHY listen (locked to this
module's Default Frequency) can be watched catching them across the full
SF5-SF12 x BW125/250/500 matrix - not just whatever the module happens to
already be configured to.

Usage:
    python3 tools/tarangmini_sf_bw_sweep.py --port /dev/ttyUSB0

Watch it in the GUI: switch to LoRa (Sub-GHz) mode and check "Lock to
frequency" set to this module's actual Default Frequency (read live via
--read-freq, or see TARANGMINI_LORA_FINDINGS.md) - otherwise the listener
only visits this frequency 1-in-4 cycles (LORA_LISTEN_CHANNELS_HZ), and
will miss most combinations in the time this script spends on each.

Restores the module's original Default Data Rate when done (including on
Ctrl-C) - this changes real device configuration via flash writes, not
just a transient RF setting, so leaving it mid-sweep would strand the
module at whatever SF/BW the sweep last tried.
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


def set_data_rate(net: TarangNet, value_byte: int) -> None:
    resp = net.config_write(CMD_DEFAULT_DATA_RATE, [value_byte])
    if not resp.get("ok"):
        raise RuntimeError(f"config_write(Default Data Rate, 0x{value_byte:02X}) failed: {resp}")
    fw = net.flash_write()
    if not fw.get("ok"):
        raise RuntimeError(f"flash_write failed after setting 0x{value_byte:02X}: {fw}")
    net.restart()
    time.sleep(3.0)  # module reboots - give the radio time to come back up


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="/dev/ttyUSB0", help="TarangMini serial port")
    ap.add_argument("--reps", type=int, default=5, help="bursts sent per SF/BW combination")
    ap.add_argument("--gap", type=float, default=1.5, help="seconds between bursts within a combination")
    ap.add_argument(
        "--only",
        default=None,
        help="comma-separated subset of value bytes to test, e.g. --only 0x0F,0x12,0x15 "
        "(default: all 24 combinations)",
    )
    ap.add_argument("--read-freq", action="store_true", help="just read and print the Default Frequency, then exit")
    args = ap.parse_args()

    net = TarangNet(args.port)

    if args.read_freq:
        resp = net.config_read(CMD_DEFAULT_FREQUENCY)
        if resp.get("ok"):
            from tarangnet_api import FREQ_NAMES

            name = FREQ_NAMES.get(bytes(resp["value"]), "unknown")
            print(f"Default Frequency: {bytes(resp['value']).hex()} ({name})")
        else:
            print(f"config_read failed: {resp}")
        net.close()
        return 0

    combos = DATA_RATE_TABLE
    if args.only:
        wanted = {int(v, 16) for v in args.only.split(",")}
        combos = [c for c in DATA_RATE_TABLE if c[0] in wanted]
        if not combos:
            print(f"--only matched nothing in {args.only}", file=sys.stderr)
            return 1

    original = net.config_read(CMD_DEFAULT_DATA_RATE)
    if not original.get("ok") or not original.get("value"):
        print(f"could not read current Default Data Rate, aborting: {original}", file=sys.stderr)
        net.close()
        return 1
    original_value = original["value"][0]
    print(f"current Default Data Rate: 0x{original_value:02X} ({DATA_RATE_NAMES[original_value]})")
    print(f"sweeping {len(combos)} combination(s), {args.reps} bursts each, then restoring original\n")

    try:
        for value_byte, sf, bw_hz in combos:
            label = f"SF{sf:02d}_BW{bw_hz // 1000}"
            print(f"=== {label} (0x{value_byte:02X}) ===")
            set_data_rate(net, value_byte)
            payload = f"{label}_TEST".encode("ascii")
            for rep in range(args.reps):
                resp = net.send_root(BROADCAST, payload)
                ts = time.strftime("%H:%M:%S")
                print(f"  [{ts}] rep {rep}: sent {payload!r}, resp={resp.hex()}")
                time.sleep(args.gap)
    except KeyboardInterrupt:
        print("\ninterrupted - restoring original Default Data Rate before exiting")
    finally:
        print(f"\nrestoring original Default Data Rate 0x{original_value:02X}")
        set_data_rate(net, original_value)
        net.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
