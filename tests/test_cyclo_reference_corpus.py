#!/usr/bin/env python3
"""Check conservative source grouping and reject invented independent splits."""
import copy
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools/cyclostationary"))
from validate_reference_corpus import audit_partitions, corpus

inventory = {"root": "/example", "input": {"format": "cf32_le", "sample_rate_hz": 60e6, "capture_center_hz": 2437.5e6}, "files": []}
for condition in ("CLEAN", "WIFI"):
    inventory["files"].append({"relative_path": condition + "/AIR_FY/example.dat", "bytes": 960000000,
        "sample_count": 120000000, "expected_sample_count": 120000000,
        "directory_labels": {"condition": condition, "model_code": "AIR", "state_code": "FY"}})
zenodo = {"files": []}
for band in ("2G", "5G"):
    name = "DJI_inspire_2_" + band + ".bin"
    zenodo["files"].append({"source_recording_id": "zenodo:" + name, "path": "/example/" + name,
        "bytes": 480000000, "format": "ci16_le", "sample_rate_hz": 120e6, "capture_center_hz": 2440e6,
        "file": name, "conservative_split_group": "zenodo:inspire_2", "md5": "previous_checksum"})
records = corpus(inventory, zenodo, Path(sys.argv[1]))
audit_partitions(records)
assert len(records) == 7 and len({r["split_group"] for r in records}) == 3
assert all(r["partition"] == "development_only" and r["physical_unit_id"] is None for r in records)
assert all(r["link_family_truth"] is None and not r["drone_free_control"] for r in records if r["source"] != "received-wifi-fixtures")
assert len({r["split_group"] for r in records if r["source"] == "received-wifi-fixtures"}) == 1
assert all(not r["drone_free_control"] for r in records)

def rejected(rows):
    try:
        audit_partitions(rows)
    except ValueError:
        return
    raise AssertionError("leaking or unqualified independent split accepted")

# Filename/state/condition separation cannot split an unresolved physical unit.
leaking = copy.deepcopy(records)
leaking[0]["partition"] = "validation"
rejected(leaking)
leaking[0].update(physical_unit_id="known-unit", session_id="known-session")
rejected(leaking)
# Distinct source/group IDs still cannot split a known unit or session.
for key in ("physical_unit_id", "session_id", "recording_id", "split_group"):
    rows = [{"partition": "development_only", "recording_id": "a", "split_group": "a", "physical_unit_id": "a", "session_id": "a"},
            {"partition": "test", "recording_id": "b", "split_group": "b", "physical_unit_id": "b", "session_id": "b"}]
    rows[1][key] = rows[0][key]
    rejected(rows)
print("reference grouping, interference controls and independent split refusal passed")
