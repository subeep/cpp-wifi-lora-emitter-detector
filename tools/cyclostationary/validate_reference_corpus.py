#!/usr/bin/env python3
"""Bounded DSP reference replay with source grouping; no training or radio use.

Dataset/model/state labels are evaluation context only. They never enter the
DSP worker or establish link-mode truth. Unknown unit/session metadata keeps
this entire corpus in development, rather than inventing independent tests.
"""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]


def load(path):
    return json.loads(path.read_text())


def save(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n")


def corpus(inventory, zenodo, fixtures):
    records = []
    for f in inventory["files"]:
        labels = f["directory_labels"]
        relative = Path(f["relative_path"])
        if relative.is_absolute() or ".." in relative.parts:
            raise ValueError("invalid inventory relative path")
        records.append({"recording_id": "dronedetect:" + f["relative_path"],
            "path": str(Path(inventory["root"]) / relative), "bytes": f["bytes"],
            "format": inventory["input"]["format"], "sample_rate_hz": inventory["input"]["sample_rate_hz"],
            "capture_center_hz": inventory["input"]["capture_center_hz"],
            "capture_samples": min(f["sample_count"], f["expected_sample_count"]),
            "source": "dronedetect", "context_labels": labels,
            "labels_status": "directory_derived_unverified", "rate_status": "provisional_publisher_unit_ambiguity",
            "split_group": "dronedetect:model:" + labels["model_code"],
            "drone_free_control": False, "link_family_truth": None})
    for f in zenodo["files"]:
        records.append({"recording_id": f["source_recording_id"], "path": f["path"], "bytes": f["bytes"],
            "format": f["format"], "sample_rate_hz": f["sample_rate_hz"], "capture_center_hz": f["capture_center_hz"],
            "capture_samples": f["bytes"] // 4, "source": "zenodo:4264467", "context_labels": {"file": f["file"]},
            "labels_status": "publisher_model_label_not_link_mode", "rate_status": "publisher_declared",
            "split_group": f["conservative_split_group"], "drone_free_control": False,
            "link_family_truth": None, "previous_publisher_md5_verification": f["md5"],
            "checksum_scope": "previous_manifest_full_file_verification_not_recomputed_here"})
    for name in ("airtel-2g4", "avgarde-5g", "avgarde-5g-offset"):
        header = load(fixtures / (name + ".json"))
        records.append({"recording_id": "received-wifi:" + name, "path": str(fixtures / header["iq_file"]),
            "bytes": header["samples"] * 8, "format": header["format"], "sample_rate_hz": header["sample_rate_hz"],
            "capture_center_hz": header["capture_center_hz"], "capture_samples": header["samples"],
            "source": "received-wifi-fixtures", "context_labels": {"fixture": name},
            "labels_status": "ordinary_wifi_decoded_mpdu_fcs_same_project", "rate_status": "capture_header",
            "split_group": "received-wifi:2026-09-19-session", "drone_free_control": False,
            "confusable_control": True, "link_family_truth": "ordinary_wifi",
            "control_scope": "ordinary_wifi_waveform_only_physical_platform_not_verified"})
    ids = set()
    for f in records:
        if f["recording_id"] in ids:
            raise ValueError("duplicate source recording")
        ids.add(f["recording_id"])
        f.update({"physical_unit_id": None, "session_id": None, "partition": "development_only",
                  "independent_unit_or_session_test_eligible": False,
                  "continuity": "caller_declared_within_selected_windows_not_full_file_verified"})
    return sorted(records, key=lambda f: f["recording_id"])


def audit_partitions(records):
    """Reject recording, group, physical-unit or session leakage if split later."""
    owners = {}
    for f in records:
        for field in ("recording_id", "split_group", "physical_unit_id", "session_id"):
            key = (field, f.get(field))
            if key[1] is None:
                continue
            if key in owners and owners[key] != f["partition"]:
                raise ValueError("partition leakage: " + field)
            owners[key] = f["partition"]
        if f["partition"] != "development_only" and (not f["physical_unit_id"] or not f["session_id"]):
            raise ValueError("independent evaluation requires unit and session metadata")


def replay(record, tool, output):
    path = Path(record["path"]).resolve()
    before = path.stat()
    if not path.is_file() or before.st_size != record["bytes"]:
        raise ValueError("file size differs from manifest: " + str(path))
    start = time.monotonic()
    command = [str(tool), "shadow-replay", "--input", str(path), "--format", record["format"],
        "--sample-rate", str(record["sample_rate_hz"]), "--center", str(record["capture_center_hz"]),
        "--capture-samples", str(record["capture_samples"]), "--continuous-samples", str(record["capture_samples"])]
    result = subprocess.run(command, text=True, capture_output=True, check=True, timeout=15)
    measured = json.loads(result.stdout)
    if measured["classification"]["link_family"] != "unknown" or measured["classification"]["model_status"] != "not_trained":
        raise ValueError("experimental replay unexpectedly published identity or model verdict")
    windows, best_chirps, best_ofdm = [], {}, {}
    statuses, patterns, quality_failures = Counter(), Counter(), Counter()
    width = 8 if record["format"] == "cf32_le" else 4
    with path.open("rb") as stream:
        for tile in measured["tiles"]:
            offset, count = tile["source_offset_samples"], tile["samples"]
            stream.seek(offset * width)
            data = stream.read(count * width)
            if len(data) != count * width:
                raise ValueError("short selected window hash read")
            windows.append({"offset_samples": offset, "samples": count, "sha256_iq_window": hashlib.sha256(data).hexdigest()})
            evidence = tile["dsp_evidence"]
            if evidence["named_family_acceptance_enabled"] or evidence["thresholds_calibrated"]:
                raise ValueError("unvalidated replay published acceptance/calibration")
            for candidate in evidence["candidates"]:
                statuses[candidate["kind"] + ":" + candidate["status"]] += 1
                if candidate["pattern_consistent"]:
                    patterns[candidate["kind"]] += 1
            for check in evidence["quality_checks"]:
                if check["known"] and not check["passed"]:
                    quality_failures[check["name"]] += 1
            for h in tile["chirp_structure"]["candidates"]:
                if h["status"] == "measured" and (h["label"] not in best_chirps or h["peak_coherence_squared"] > best_chirps[h["label"]]["peak_coherence_squared"]):
                    best_chirps[h["label"]] = h
            for h in tile["ofdm_candidates"]:
                if h["status"] == "measured" and (h["timing_label"] not in best_ofdm or h["holdout_contrast"] > best_ofdm[h["timing_label"]]["holdout_contrast"]):
                    best_ofdm[h["timing_label"]] = h
    after = path.stat()
    if (before.st_size, before.st_mtime_ns, before.st_ino) != (after.st_size, after.st_mtime_ns, after.st_ino):
        raise ValueError("source changed during replay")
    target = hashlib.sha256(record["recording_id"].encode()).hexdigest()[:20] + ".json"
    save(output / target, {"context": record, "selected_window_hashes": windows, "measurement": measured})
    return {"recording_id": record["recording_id"], "split_group": record["split_group"], "source": record["source"],
        "context_labels": record["context_labels"], "output": target, "windows": len(windows),
        "copied_samples": measured["selection"]["copied_samples"], "capture_samples": record["capture_samples"],
        "elapsed_seconds": round(time.monotonic() - start, 5), "worker_ms": measured["worker"]["processing_roundtrip_ms"],
        "best_chirps": best_chirps, "best_ofdm": best_ofdm, "classification": measured["classification"],
        "evidence_status_counts": dict(statuses), "pattern_counts": dict(patterns),
        "quality_failure_counts": dict(quality_failures)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inventory", type=Path, default=ROOT / "data/wifi_drone_analysis/offline_2026-10-02/dronedetect_inventory.json")
    parser.add_argument("--zenodo", type=Path, default=ROOT / "data/wifi_drone_analysis/offline_2026-10-03/zenodo_verified_local_manifest.json")
    parser.add_argument("--fixtures", type=Path, default=ROOT / "tests/fixtures/wifi_ofdm")
    parser.add_argument("--tool", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--workers", type=int, default=2, choices=range(1, 5))
    parser.add_argument("--manifest-only", action="store_true")
    args = parser.parse_args()
    output, tool = args.output.resolve(), args.tool.resolve()
    if output.exists():
        raise ValueError("output must be a new directory to preserve previous runs")
    output.mkdir(parents=True)
    records = corpus(load(args.inventory), load(args.zenodo), args.fixtures.resolve())
    audit_partitions(records)
    manifest = {"schema": "rfmon.cyclo.reference_corpus.v1", "records": records,
        "partition_policy": "all development only; unresolved physical units/sessions block independent testing",
        "group_policy": "all same-model DroneDetect conditions/states together; Zenodo same-model bands/parts together; Wi-Fi crops one session",
        "grouping_across_publishers": "physical_independence_not_verified",
        "interference_subsets_are_drone_free": False,
        "metadata_sha256": {str(p.resolve()): hashlib.sha256(p.read_bytes()).hexdigest() for p in (args.inventory, args.zenodo)}}
    save(output / "manifest.json", manifest)
    if args.manifest_only:
        print(json.dumps({"recordings": len(records), "manifest": str(output / "manifest.json")}))
        return
    start = time.monotonic()
    measurements, errors = [], []
    def run(f):
        try:
            return replay(f, tool, output), None
        except Exception as error:
            return None, {"recording_id": f["recording_id"], "error": str(error)}
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        for index, (measurement, error) in enumerate(pool.map(run, records), 1):
            (errors if error else measurements).append(error if error else measurement)
            if index % 25 == 0:
                print(f"Replayed {index}/{len(records)}, errors {len(errors)}", flush=True)
    summary = {"schema": "rfmon.cyclo.reference_replay.v1", "scope": "native-rate exploratory feature coverage, not drone/link accuracy",
        "elapsed_seconds": round(time.monotonic() - start, 3), "radio_opened": False, "originals_modified": False,
        "model_trained": False, "remote_id_decoded": False, "source_counts": dict(Counter(f["source"] for f in records)),
        "recordings_requested": len(records), "recordings_measured": len(measurements),
        "windows_measured": sum(m["windows"] for m in measurements),
        "group_count": len({f["split_group"] for f in records}), "verified_independent_units": 0,
        "verified_independent_sessions": 0, "named_drone_family_truth_records": 0,
        "named_family_acceptance_enabled": False, "family_precision_recall": None,
        "passband_matched_live_validation": False, "errors": errors, "measurements": measurements}
    for key in ("evidence_status_counts", "pattern_counts", "quality_failure_counts"):
        counts = Counter()
        for measurement in measurements:
            counts.update(measurement[key])
        summary[key] = dict(counts)
    summary["evidence_policy_version"] = "experimental_dsp_v1"
    summary["thresholds_calibrated"] = False
    summary["all_passbands_unverified"] = True
    save(output / "summary.json", summary)
    print(json.dumps({k: v for k, v in summary.items() if k not in ("measurements", "errors")}, indent=2))
    if errors:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
