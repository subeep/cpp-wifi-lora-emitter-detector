#!/usr/bin/env python3
"""Match Heltec ground truth (heltec_link.py) against lora_live_check output.

A transmission counts as received when a CRC-valid row carries exactly its
bytes. Rows are classified as:
  matched  - bytes equal a logged transmission
  ours_bad - carries our tag (ASCII "HLT<run>" or DevAddr 26FFBEEF) but matches
             no transmission: a false positive
  foreign  - anything else (other LoRa devices on the air)

  lora_match.py truth.ndjson receiver.json [--run TAG] [--json report.json]
"""
import argparse
import collections
import json

OUR_DEVADDR_LE = "EFBEFF26"  # DevAddr 26FFBEEF as it appears on air


def norm(h):
    return (h or "").replace(" ", "").upper()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("truth")
    ap.add_argument("receiver")
    ap.add_argument("--run", default=None, help="only transmissions with this run tag")
    ap.add_argument("--json", default=None)
    a = ap.parse_args()

    tx = []
    for line in open(a.truth):
        o = json.loads(line)
        if o.get("ev") == "tx" and o.get("status", 0) == 0 and (a.run is None or o.get("run") == a.run):
            tx.append(o)
    rx = json.load(open(a.receiver))
    rows = rx.get("crc_valid_rows", [])

    by_hex = collections.defaultdict(list)
    for r in rows:
        by_hex[norm(r["payload_hex"])].append(r)
    tags = {"HLT" + t["run"] for t in tx}

    def ours(h):
        try:
            text = bytes.fromhex(h).decode("latin-1")
        except ValueError:
            text = ""
        return any(t in text for t in tags) or h[2:10] == OUR_DEVADDR_LE

    groups = collections.defaultdict(lambda: [0, 0])  # (kind, freq, sf, bw) -> [sent, received]
    freq_err, wrong_phy, missed = [], 0, []
    truth_hex = set()
    for t in tx:
        h = norm(t["hex"])
        truth_hex.add(h)
        key = (t["kind"], t["freq"], t["sf"], t["bw"])
        groups[key][0] += 1
        hits = by_hex.get(h, [])
        if hits:
            groups[key][1] += 1
            for r in hits:
                freq_err.append((r["freq_mhz"] - t["freq"]) * 1e6)
                wrong_phy += r["sf"] != t["sf"] or abs(r["bandwidth_khz"] - t["bw"]) > 1
        else:
            missed.append({"seq": t["seq"], "freq": t["freq"], "sf": t["sf"], "air_ms": t["air_ms"], "host_ts": t["host_ts"]})

    matched = sum(1 for r in rows if norm(r["payload_hex"]) in truth_hex)
    ours_bad = [r for r in rows if norm(r["payload_hex"]) not in truth_hex and ours(norm(r["payload_hex"]))]
    foreign = [r for r in rows if norm(r["payload_hex"]) not in truth_hex and not ours(norm(r["payload_hex"]))]
    sent = len(tx)
    received = sum(v[1] for v in groups.values())

    wall = rx.get("wall_s", 0) or 1
    share = {k: v / wall for k, v in rx.get("listen", {}).get("channel_iq_seconds", {}).items()}
    report = {
        "sent": sent,
        "received": received,
        "recall": received / sent if sent else None,
        "per_group": [
            {"kind": k[0], "freq": k[1], "sf": k[2], "bw": k[3], "sent": v[0], "received": v[1],
             "recall": v[1] / v[0] if v[0] else None}
            for k, v in sorted(groups.items())
        ],
        "channel_listen_share": share,
        "crc_valid_rows": len(rows),
        "rows_matched": matched,
        "false_positives": len(ours_bad),
        "foreign_rows": len(foreign),
        "foreign_examples": [{k: r.get(k) for k in ("freq_mhz", "sf", "bandwidth_khz", "payload_hex", "lorawan_candidate")}
                             for r in foreign[:10]],
        "wrong_phy_rows": wrong_phy,
        "freq_error_hz": {"n": len(freq_err), "max_abs": max((abs(x) for x in freq_err), default=None)},
        "duplicate_rows_per_received": (matched / received) if received else None,
        "missed": missed[:50],
        "incidents": [{k: i[k] for k in ("rule", "subject", "count", "summary")} for i in rx.get("incidents", [])],
        "lorawan_devices": rx.get("lorawan_devices", []),
        "rf_emitters": [{k: e[k] for k in ("id", "readings", "policy", "last_freq_mhz")} for e in rx.get("rf_emitters", [])],
        "listen": rx.get("listen"),
        "overflow_polls": rx.get("overflow_polls"),
        "evidence_excluded": rx.get("evidence", {}).get("excluded"),
        "suggestions": rx.get("suggestions"),
    }
    if a.json:
        json.dump(report, open(a.json, "w"), indent=1)
    print(f"sent {sent}  received {received}  recall {report['recall'] if report['recall'] is None else round(report['recall'], 3)}"
          f"  false positives {len(ours_bad)}  foreign rows {len(foreign)}  wrong PHY {wrong_phy}")
    for g in report["per_group"]:
        print(f"  {g['kind']:4} {g['freq']:9.4f} MHz SF{g['sf']:<2} BW{g['bw']:<4} {g['received']:3}/{g['sent']:<3}"
              f" recall {g['recall']:.2f}  listen share {share.get(format(g['freq'], '.4f'), 0):.2f}")
    print(f"  incidents: {[(i['rule'], i['subject'], i['count']) for i in report['incidents']]}")
    print(f"  emitters: {report['rf_emitters']}")
    print(f"  overflow polls {report['overflow_polls']}  evidence excluded {report['evidence_excluded']}  listen {report['listen']}")


if __name__ == "__main__":
    main()
