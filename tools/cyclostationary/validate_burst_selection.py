#!/usr/bin/env python3
"""Controlled selection check with received ordinary Wi-Fi, no radio or model.

Embeds existing received crops into seeded noise between the unguided windows.
Hints are known constructed ranges, not evidence of detector recall. Saves JSON
measurements/provenance, discards temporary derived IQ, preserves originals.
"""
import argparse
from array import array
import hashlib
import json
from pathlib import Path
import random
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    tool = args.tool.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    summary = {"scope": "controlled ordinary-Wi-Fi selection check; constructed hints; not drone detection accuracy",
               "capture_samples": 1000000, "seed": 27019, "sample_offset": 450000,
               "metadata_source": "fixture-declared, not inferred", "originals_modified": False,
               "radio_opened": False, "measurements": []}
    rng = random.Random(summary["seed"])
    noise = array("f", (rng.gauss(0, 1) for _ in range(2 * summary["capture_samples"])))
    with tempfile.TemporaryDirectory(prefix="rfmon-burst-selection-") as directory:
        temp = Path(directory)
        for stem in ("airtel-2g4", "avgarde-5g", "avgarde-5g-offset"):
            source = root / "tests" / "fixtures" / "wifi_ofdm" / (stem + ".cf32")
            raw = source.read_bytes()
            metadata = json.loads(source.with_suffix(".json").read_text())
            crop = array("f")
            crop.frombytes(raw)
            if sys.byteorder != "little":
                crop.byteswap()
            scale = (sum(value * value for value in crop) / len(crop)) ** 0.5 * 0.01
            derived = array("f", (value * scale for value in noise))
            offset = summary["sample_offset"]
            derived[2 * offset:2 * offset + len(crop)] = crop
            if sys.byteorder != "little":
                derived.byteswap()
            iq = temp / (stem + ".cf32")
            iq.write_bytes(derived.tobytes())
            hints = temp / "hints.json"
            hints.write_text(json.dumps({"ranges": [{"start": offset, "length": len(crop) // 2}]}))
            common = [str(tool), "shadow-replay", "--input", str(iq), "--format", "cf32_le",
                      "--sample-rate", str(metadata["sample_rate_hz"]), "--center", str(metadata["capture_center_hz"]),
                      "--capture-samples", str(summary["capture_samples"])]
            reports = {}
            for mode in ("unguided", "guided"):
                command = common + (["--burst-hints", str(hints)] if mode == "guided" else [])
                run = subprocess.run(command, check=True, text=True, capture_output=True, timeout=10)
                report = json.loads(run.stdout)
                # The derived IQ is ephemeral. Keep durable original provenance
                # and exact construction metadata with the measured report.
                report["input"]["path"] = None
                report["input"]["derived_from"] = str(source)
                report["input"]["derived_iq_retained"] = False
                path = output / (stem + "_" + mode + ".json")
                path.write_text(json.dumps(report, indent=2) + "\n")
                contrasts = [candidate["holdout_contrast"] for tile in report["tiles"]
                             for candidate in tile["ofdm_candidates"] if candidate["status"] == "measured"]
                reports[mode] = {"report": path.name, "copied_samples": report["selection"]["copied_samples"],
                                 "burst_guided_windows": report["selection"]["burst_guided_windows"],
                                 "highest_cp_contrast": max(contrasts, default=None),
                                 "roundtrip_ms": report["worker"]["processing_roundtrip_ms"]}
            item = {"source": str(source), "original_sha256": hashlib.sha256(raw).hexdigest(),
                    "sample_rate_hz": metadata["sample_rate_hz"], "crop_samples": len(crop) // 2,
                    "noise_component_stddev": scale, **reports}
            # A controlled improvement check, not a classifier acceptance rule.
            assert reports["unguided"]["highest_cp_contrast"] < 0.1, item
            assert reports["guided"]["highest_cp_contrast"] > 0.8, item
            assert reports["guided"]["burst_guided_windows"] == 1, item
            assert reports["guided"]["copied_samples"] <= 262144, item
            assert hashlib.sha256(source.read_bytes()).hexdigest() == item["original_sha256"]
            summary["measurements"].append(item)
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
