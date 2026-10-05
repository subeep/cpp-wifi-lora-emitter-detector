"""Wire-format, provenance and bounded-I/O integration tests; no hardware."""
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

tool = str(Path(sys.argv[1]).resolve())


def run(*args, success=True):
    result = subprocess.run([tool, *map(str, args)], capture_output=True, text=True, timeout=20)
    if success:
        assert result.returncode == 0, result.stderr
        return json.loads(result.stdout)
    assert result.returncode != 0, args
    return result


with tempfile.TemporaryDirectory(prefix="rfmon-cyclo-cli-") as directory:
    root = Path(directory)
    iq = root / "CLEAN" / "AIR_FY" / "AIR_0010_00.dat"
    iq.parent.mkdir(parents=True)
    (root / "BLUE" / "PHA_FY").mkdir(parents=True)
    iq.write_bytes(b"".join(struct.pack("<ff", math.cos(i * 0.21), math.sin(i * 0.21))
                            for i in range(10000)))
    checksum = hashlib.sha256(iq.read_bytes()).hexdigest()
    common = ["--format", "cf32_le", "--sample-rate", "60000000", "--center", "2437500000"]
    report = run("analyze", "--input", iq, "--offset", 111, "--samples", 6000, *common)
    assert report["schema"] == "rfmon.cyclo.features.v1"
    assert report["input"]["offset_samples"] == 111
    assert report["input"]["samples_read"] == 6000
    assert report["input"]["continuity"] == "not_verified"
    assert report["classification"]["model_status"] == "not_trained"
    assert report["classification"]["link_family"] == "unknown"
    assert report["measurement"]["coherence_is_probability"] is False
    assert report["measurement"]["mixture_status"] == "not_estimated"
    assert "spectrum" not in report
    assert "roi_measurements" not in report
    assert "chirp_structure" not in report
    chirps = run("analyze", "--input", iq, "--offset", 111, "--samples", 6000, "--measure-chirps", *common)["chirp_structure"]
    assert chirps["vendor_or_drone_identity"] is False and chirps["coherence_is_probability"] is False
    assert len(chirps["candidates"]) == 3
    for h in chirps["candidates"]:
        assert h["lag_samples"] == h["gate_samples"] == 1920
        assert h["positions_examined"] == 2161
        assert h["first_original_source_sample_center"] == 111 + h["peak_window_offset_samples"]
        assert h["last_original_source_sample_center"] == h["first_original_source_sample_center"] + 3839
    short_chirp = run("analyze", "--input", iq, "--samples", 16, "--measure-chirps", *common)["chirp_structure"]
    assert all(h["peak_coherence_squared"] is None for h in short_chirp["candidates"])
    measured_roi = run("analyze", "--input", iq, "--offset", 111, "--samples", 6000, "--measure-rois", *common)
    roi = measured_roi["roi_measurements"]
    assert roi["samples_examined"] == 6000 and roi["block_samples"] == 128
    assert roi["detection_or_drone_threshold"] is False
    assert roi["status"] == "uniform_energy" and roi["regions"][0]["contrast_selected"] is False
    assert roi["regions"][0]["first_original_source_sample_center"] == 111
    assert roi["regions"][0]["last_original_source_sample_center"] == 6110
    assert roi["regions"][0]["spectral_samples"] <= 6000
    assert roi["regions"][0]["spectral_intervals"][0]["rf_low_hz"] == 2437500000 + roi["regions"][0]["spectral_intervals"][0]["low_offset_hz"]
    assert len(report["ofdm_structure"]["candidates"]) == 5
    assert report["ofdm_structure"]["vendor_or_drone_identity"] is False
    assert report["ofdm_structure"]["calibrated_detection_threshold"] is None
    custom = run("analyze", "--input", iq, *common, "--ofdm-useful", 64, "--ofdm-prefix", 16)
    assert len(custom["ofdm_structure"]["candidates"]) == 1
    assert custom["ofdm_structure"]["candidates"][0]["symbol_rate_hz"] == 750000
    run("analyze", "--input", iq, *common, "--ofdm-useful", 64, success=False)
    run("analyze", "--input", iq, *common, "--ofdm-useful", 64, "--ofdm-prefix", 64, success=False)
    short = run("analyze", "--input", iq, "--samples", 16, *common)
    assert short["status"] == "insufficient_samples"
    assert short["features"]["spectral_flatness"] is None
    assert short["features"]["mean_power"] is None
    assert all(c["holdout_contrast"] is None for c in short["ofdm_structure"]["candidates"])
    full = run("analyze", "--input", iq, "--samples", 6000, "--include-spectrum", *common)
    assert len(full["spectrum"]["power_fraction"]) == 512
    assert all(p["alpha_hz"] > 0 for p in full["features"]["cyclic_peaks"])
    for p in full["features"]["cyclic_peaks"]:
        assert p["rf_frequency_hz"] == 2437500000 + p["frequency_offset_hz"]
    band_options = ["--roi-offset", "100000", "--roi-bandwidth", "8000000",
                    "--source-bandwidth", "20000000", "--decimate", "3"]
    band = run("analyze", "--input", iq, *common, *band_options)
    prep = band["preprocessing"]
    assert prep["source_mean_removed_before_downmix"] is False
    assert prep["removed_source_mean_i"] == prep["removed_source_mean_q"] == 0
    dc_band = run("analyze", "--input", iq, *common, *band_options, "--remove-source-dc", "--assess-evidence")
    assert dc_band["preprocessing"]["source_mean_removed_before_downmix"] is True
    assert dc_band["dsp_evidence"]["context"]["usable_bandwidth_hz"] == 8000000
    assert dc_band["classification"] == band["classification"]
    run("analyze", "--input", iq, *common, *band_options, "--usable-bandwidth", 8000000, success=False)
    filtered_chirp = run("analyze", "--input", iq, *common, *band_options, "--measure-chirps")["chirp_structure"]
    for h in filtered_chirp["candidates"]:
        assert h["lag_samples"] == 640 and filtered_chirp["source_sample_step"] == 3
        assert h["first_original_source_sample_center"] == prep["first_output_source_sample"] + 3 * h["peak_window_offset_samples"]
        assert h["last_original_source_sample_center"] == h["first_original_source_sample_center"] + 3 * 1279
    assert prep["sample_rate_hz"] == 20000000
    assert prep["analysis_center_hz"] == 2437600000
    assert prep["first_output_source_sample"] == prep["filter_half_length_source_samples"]
    assert prep["input_sample_step"] == 3
    assert prep["output_samples"] == 1 + (10000 - prep["filter_taps"]) // 3
    assert band["measurement"]["duration_seconds"] == band["measurement"]["samples_used"] / 20000000
    for p in band["features"]["cyclic_peaks"]:
        assert p["rf_frequency_hz"] == 2437600000 + p["frequency_offset_hz"]
    filtered_roi = run("analyze", "--input", iq, *common, *band_options, "--measure-rois")["roi_measurements"]
    assert filtered_roi["source_sample_step"] == 3
    first_region = filtered_roi["regions"][0]
    assert first_region["first_original_source_sample_center"] == prep["filter_half_length_source_samples"] + first_region["window_offset_samples"] * 3
    assert first_region["last_original_source_sample_center"] == first_region["first_original_source_sample_center"] + (first_region["samples"] - 1) * 3
    assert first_region["spectral_intervals"][0]["rf_low_hz"] == 2437600000 + first_region["spectral_intervals"][0]["low_offset_hz"]
    run("analyze", "--input", iq, *common, "--decimate", "3", success=False)
    run("analyze", "--input", iq, *common, "--roi-offset", "10000000",
        "--roi-bandwidth", "8000000", "--source-bandwidth", "20000000", "--decimate", "3", success=False)
    audit = run("inventory", "--root", root, "--expected-samples", 120000000, *common)
    assert audit["summary"]["file_count"] == 1
    assert audit["summary"]["short_files"] == 1
    assert "BLUE/PHA_FY" in audit["empty_directories"]
    assert audit["files"][0]["split_group"] == "CLEAN/AIR_FY/AIR_0010_00.dat"
    assert audit["provenance"]["physical_units"] == "unknown"
    assert hashlib.sha256(iq.read_bytes()).hexdigest() == checksum
    for value in ["nan", "inf", "0", "-1", "20oops"]:
        run("analyze", "--input", iq, "--format", "cf32_le", "--sample-rate", value, success=False)
    run("analyze", "--input", iq, "--samples", 1048577, *common, success=False)
    run("analyze", "--input", iq, "--offset", "-1", *common, success=False)
    run("analyze", "--input", iq, "--offset", 10000, *common, success=False)
    run("analyze", "--input", iq, "--hop", 256, *common, success=False)
    run("analyze", "--input", iq, "--frames", 1025, *common, success=False)
    run("analyze", "--input", iq, "--samples", 100, "--samples", 200, *common, success=False)
    corrupt = root / "bad.dat"
    corrupt.write_bytes(struct.pack("<ff", float("nan"), 0))
    bad = run("inventory", "--root", root, *common, success=False)
    assert bad.returncode == 3
    assert json.loads(bad.stdout)["summary"]["probe_errors"] == 1
    corrupt.write_bytes(b"x")
    run("analyze", "--input", corrupt, *common, success=False)
    signed = root / "signed.bin"
    signed.write_bytes(struct.pack("<hhhh", -32768, 32767, 16384, -16384) * 1024)
    s = run("analyze", "--input", signed, "--format", "ci16_le", "--sample-rate", 120000000)
    assert s["input"]["file_samples"] == 2048
    assert s["features"]["max_component_abs"] == 1
    used = s["measurement"]["samples_used"]
    assert s["features"]["int16_full_scale_component_fraction"] == math.ceil(used / 2) / used
    # Sparse 8 GB input proves analysis allocates by window, not file size.
    large = root / "large.dat"
    with large.open("wb") as stream:
        stream.write(iq.read_bytes())
        stream.truncate(8_000_000_000)
    bounded = run("analyze", "--input", large, "--samples", 4096, *common)
    run("analyze", "--input", large, "--samples", 65537, "--measure-chirps", *common, success=False)
    assert bounded["input"]["file_samples"] == 1_000_000_000
    assert bounded["input"]["samples_read"] == 4096
    sampled = run("shadow-replay", "--input", large, "--capture-samples", 1000000000, *common)
    assert sampled["schema"] == "rfmon.cyclo.shadow_replay.v1"
    assert sampled["selection"]["copied_samples"] == 262144
    assert sampled["selection"]["phase_concatenation"] is False
    assert sampled["worker"]["process_isolated"] is True
    assert sampled["worker"]["wire_version"] == 4
    assert all(not t["dsp_evidence"]["named_family_acceptance_enabled"] for t in sampled["tiles"])
    assert sampled["tiles"][0]["dsp_evidence"]["context"]["usable_bandwidth_hz"] is None
    preview = run("analyze", "--input", iq, "--samples", 4096, "--assess-evidence", "--usable-bandwidth", 16000000, *common)
    assert preview["dsp_evidence"]["policy_version"] == "experimental_dsp_v1"
    assert preview["dsp_evidence"]["context"]["int16_rails_known"] is False
    assert not preview["dsp_evidence"]["thresholds_calibrated"]
    run("analyze", "--input", iq, "--remove-source-dc", *common, success=False)
    run("analyze", "--input", iq, "--usable-bandwidth", 61000000, *common, success=False)
    run("analyze", "--input", large, "--samples", 65537, "--assess-evidence", *common, success=False)
    assert len(sampled["tiles"][0]["chirp_structure"]["candidates"]) == 3
    assert all(t["roi_measurements"]["samples_examined"] == t["samples"] for t in sampled["tiles"])
    assert len(sampled["tiles"]) == 4
    assert sampled["tiles"][-1]["file_offset_samples"] + sampled["tiles"][-1]["samples"] == 1000000000
    run("analyze", "--input", large, "--samples", 65537, "--measure-rois", *common, success=False)
    hints = root / "hints.json"
    hints.write_text(json.dumps({"ranges": [{"start": 450000, "length": 12000},
                                           {"start": 750000, "length": 8000},
                                           {"start": 999999995, "length": 100}], "detector_capped": True}))
    guided = run("shadow-replay", "--input", large, "--capture-samples", 1000000000,
                 "--offset", 111, "--burst-hints", hints, *common)
    assert guided["selection"]["burst_guided_windows"] == 2
    assert guided["selection"]["hints_rejected"] == 1
    assert guided["selection"]["detector_capped"] is True
    assert guided["selection"]["hints_source"] == "caller_declared_not_verified"
    assert guided["selection"]["copied_samples"] <= 262144
    assert sum(t["selection_origin"] == "raw_burst_hint" for t in guided["tiles"]) == 2
    assert all(t["file_offset_samples"] == 111 + t["source_offset_samples"] for t in guided["tiles"])
    for invalid in ({"ranges": [{"start": -1, "length": 40}]}, {"ranges": [{"start": 2.5, "length": 40}]},
                    {"ranges": [] , "detector_capped": "true"}, {"ranges": [{}]},
                    {"ranges": [{"start": 0, "length": 2048}] * 513}):
        hints.write_text(json.dumps(invalid))
        run("shadow-replay", "--input", large, "--capture-samples", 1000000,
            "--burst-hints", hints, *common, success=False)
    hints.write_text(" " * 65537)
    run("shadow-replay", "--input", large, "--capture-samples", 1000000,
        "--burst-hints", hints, *common, success=False)
    prefix = run("shadow-replay", "--input", large, "--capture-samples", 1000000000,
                 "--continuous-samples", 5000, *common)
    assert prefix["selection"]["copied_samples"] == 5000 and len(prefix["tiles"]) == 1
    run("shadow-replay", "--input", large, "--capture-samples", 5000,
        "--continuous-samples", 5001, *common, success=False)
    run("shadow-replay", "--input", large, "--capture-samples", 2047, *common, success=False)
    run("shadow-replay", "--input", iq, "--capture-samples", 1000000, "--offset", 111, *common)

print("cyclostationary CLI checks passed")
