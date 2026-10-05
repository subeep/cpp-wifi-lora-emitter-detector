#!/usr/bin/env python3
"""Replay saved process-stage cases; preserve IQ and compare pre-ROI DSP fields."""
import argparse
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    baseline = args.baseline.resolve()
    previous = json.loads((baseline / "summary.json").read_text())
    summary = {"scope": "same native-rate windows; ROI measurement/protocol exercise; not drone detection accuracy",
               "baseline": str(baseline), "originals_modified": False, "radio_opened": False,
               "ml_or_remote_id_added": False, "measurements": []}
    for case in previous["measurements"]:
        old = json.loads((baseline / case["output"]).read_text())
        metadata, selection = old["input"], old["selection"]
        command = [str(args.tool.resolve()), "shadow-replay", "--input", metadata["path"],
                   "--format", metadata["format"], "--sample-rate", str(metadata["sample_rate_hz"]),
                   "--offset", str(metadata["capture_offset_samples"]),
                   "--capture-samples", str(selection["capture_samples_requested"]),
                   "--continuous-samples", str(selection["continuous_samples_declared"])]
        if metadata["capture_center_hz"] is not None:
            command += ["--center", str(metadata["capture_center_hz"])]
        if "power_fraction" in old["tiles"][0]:
            command += ["--include-spectrum"]
        result = subprocess.run(command, check=True, text=True, capture_output=True, timeout=10)
        new = json.loads(result.stdout)
        assert new["classification"] == old["classification"]
        assert new["selection"]["copied_samples"] == old["selection"]["copied_samples"]
        assert len(new["tiles"]) == len(old["tiles"])
        region_count = band_count = omitted = 0
        for before, after in zip(old["tiles"], new["tiles"]):
            assert all(after[key] == value for key, value in before.items()), case["case"]
            roi = after["roi_measurements"]
            assert roi["samples_examined"] == after["samples"] and len(roi["regions"]) <= 8
            region_count += len(roi["regions"])
            omitted += roi["regions_omitted"]
            for region in roi["regions"]:
                assert region["window_offset_samples"] + region["samples"] <= after["samples"]
                assert region["first_original_source_sample_center"] == after["file_offset_samples"] + region["window_offset_samples"]
                assert region["spectral_samples"] <= region["samples"] and region["spectral_frames"] <= 32
                band_count += len(region["spectral_intervals"])
        (output / case["output"]).write_text(json.dumps(new, indent=2) + "\n")
        summary["measurements"].append({"case": case["case"], "output": case["output"],
            "sample_rate_hz": metadata["sample_rate_hz"], "windows": len(new["tiles"]),
            "copied_samples": new["selection"]["copied_samples"], "old_dsp_fields_exactly_equal": True,
            "roi_statuses": [tile["roi_measurements"]["status"] for tile in new["tiles"]],
            "regions_shown": region_count, "contrast_regions_omitted": omitted,
            "spectral_intervals_shown": band_count,
            "roundtrip_ms": new["worker"]["processing_roundtrip_ms"], "classification": new["classification"]})
    summary["windows"] = sum(item["windows"] for item in summary["measurements"])
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
