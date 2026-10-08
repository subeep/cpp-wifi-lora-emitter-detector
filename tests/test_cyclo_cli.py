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
    assert sampled["worker"]["wire_version"] == 10
    assert all(not t["dsp_evidence"]["named_family_acceptance_enabled"] for t in sampled["tiles"])
    assert sampled["tiles"][0]["dsp_evidence"]["context"]["usable_bandwidth_hz"] is None
    preview = run("analyze", "--input", iq, "--samples", 4096, "--assess-evidence", "--usable-bandwidth", 16000000, *common)
    assert preview["dsp_evidence"]["policy_version"] == "experimental_dsp_v1"
    assert preview["dsp_evidence"]["context"]["int16_rails_known"] is False
    assert not preview["dsp_evidence"]["thresholds_calibrated"]
    refined = run("analyze", "--input", iq, "--samples", 4096, "--offset", 111, "--refine-chirps", "--assess-evidence", *common)
    assert refined["chirp_refinement"]["offline_only"] is True
    assert not refined["chirp_refinement"]["classification_used"]
    assert not refined["chirp_refinement"]["live_worker_uses_refinement"]
    assert len(refined["chirp_refinement"]["candidates"]) == 3
    assert refined["classification"] == preview["classification"]
    for c in refined["chirp_refinement"]["candidates"]:
        if c["status"] == "measured":
            assert 111 <= c["first_original_source_sample_center"]
            assert c["last_original_source_sample_center"] < 111 + 4096
            assert c["spectra_examined"] <= 75
    run("analyze", "--input", large, "--samples", 65537, "--refine-chirps", *common, success=False)
    run("shadow-replay", "--input", iq, "--capture-samples", 4096, "--refine-chirps", *common, success=False)
    short_refined = run("analyze", "--input", iq, "--samples", 16, "--refine-chirps", *common)
    assert all(c["dechirped_band_power_fraction"] is None for c in short_refined["chirp_refinement"]["candidates"])
    filtered_refined = run("analyze", "--input", iq, *common, *band_options, "--refine-chirps")
    assert filtered_refined["chirp_refinement"]["source_sample_step"] == 3
    for c in filtered_refined["chirp_refinement"]["candidates"]:
        if c["status"] == "measured":
            assert c["first_original_source_sample_center"] >= prep["first_output_source_sample"]
            assert c["last_original_source_sample_center"] < 10000
    run("analyze", "--input", iq, "--remove-source-dc", *common, success=False)
    run("analyze", "--input", iq, "--usable-bandwidth", 61000000, *common, success=False)
    run("analyze", "--input", large, "--samples", 65537, "--assess-evidence", *common, success=False)
    assert len(sampled["tiles"][0]["chirp_structure"]["candidates"]) == 3
    assert all(t["waveform_features"]["samples_examined"]==t["samples"] for t in sampled["tiles"])
    expanded=run("analyze","--input",iq,"--offset",111,"--samples",6000,"--expand-waveforms",*common)
    assert expanded["classification"]==report["classification"]
    for key in ["features","measurement","ofdm_structure","preprocessing","warnings"]:
        assert expanded[key]==report[key], key
    w=expanded["waveform_features"]
    assert w["classification_used"] is False and w["thresholds_calibrated"] is False
    assert w["partition_means"]=="separate" and w["fft_calls"]<=8 and w["refinement_evaluations"]<=40
    assert w["discovery_first_original_sample"]==111
    assert w["discovery_last_original_sample"]<w["holdout_first_original_sample"]
    assert w["holdout_last_original_sample"]==6110
    assert not w["morphology"]["frequency_state_centres_available"]
    assert w["morphology"]["frequency_low_hz"] is None and w["morphology"]["frequency_high_hz"] is None
    small=run("analyze","--input",iq,"--samples",16,"--expand-waveforms",*common)["waveform_features"]
    assert small["morphology"]["amplitude_cv"] is None and small["holdout_first_original_sample"] is None
    run("analyze","--input",large,"--samples",65537,"--expand-waveforms",*common,success=False)
    run("shadow-replay","--input",iq,"--capture-samples",4096,"--prepare-candidates",*common,success=False)
    unknown=run("analyze","--input",iq,"--samples",6000,"--prepare-candidates",*common)
    assert unknown["candidate_preparation"]["fir_operations"]==0
    assert all(c["status"]=="passband_unknown" for c in unknown["candidate_preparation"]["candidates"])
    prepared_candidates=run("analyze","--input",iq,"--samples",6000,"--prepare-candidates","--usable-bandwidth",50000000,*common)
    assert len(prepared_candidates["candidate_preparation"]["candidates"])<=2
    assert prepared_candidates["candidate_preparation"]["context_retained"] is True
    expanded_band=run("analyze","--input",iq,*common,*band_options,"--expand-waveforms")
    assert expanded_band["waveform_features"]["source_sample_step"]==3
    assert expanded_band["waveform_features"]["discovery_first_original_sample"]==expanded_band["preprocessing"]["first_output_source_sample"]
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

    reviewed=run("analyze","--input",iq,"--offset",111,"--samples",6000,"--review-waveforms",*common)
    assert reviewed["waveform_review"]["method"]=="expanded_measurement_review_v2"
    assert not reviewed["waveform_review"]["classification_used"] and not reviewed["waveform_review"]["thresholds_calibrated"]
    assert reviewed["structure_discovery"]==expanded["structure_discovery"]
    assert reviewed["waveform_features"]==expanded["waveform_features"]
    assert reviewed["classification"]==report["classification"]
    assert len(reviewed["waveform_review"]["candidates"])<=18
    assert all("waveform_review" in t for t in sampled["tiles"])
    assert all(t["waveform_review"]["context"]["usable_bandwidth_hz"] is None for t in sampled["tiles"])
    run("analyze","--input",large,"--samples",65537,"--review-waveforms",*common,success=False)
    assert all(t["structure_discovery"]["samples_examined"]==t["samples"] for t in sampled["tiles"])
    assert expanded["linear_sweep_discovery"]["method"]=="bounded_linear_sweep_discovery_v2"
    assert expanded["linear_sweep_review"]["method"]=="linear_sweep_review_v1"
    assert expanded["linear_sweep_discovery"]["classification_used"] is False
    sweeps=run("analyze","--input",iq,"--offset",111,"--samples",6000,"--discover-sweeps",*common)
    assert sweeps["linear_sweep_discovery"]==expanded["linear_sweep_discovery"]
    assert "structure_discovery" not in sweeps
    assert expanded_band["linear_sweep_discovery"]["source_sample_step"]==3
    assert all("linear_sweep_discovery" in t and "linear_sweep_review" in t for t in sampled["tiles"])
    run("analyze","--input",large,"--samples",65537,"--discover-sweeps",*common,success=False)
    assert expanded["cyclic_background"]["method"]=="local_caf_background_v1"
    assert expanded["cyclic_background_review"]["method"]=="cyclic_background_review_v1"
    burst=run("analyze","--input",iq,"--offset",111,"--samples",6000,"--analyze-bursts",*common)
    assert burst["classification"]["drone_assessment"]=="insufficient_evidence"
    assert burst["burst_analysis"]["sample_limit_per_tile"]==16384
    assert not burst["burst_analysis"]["phase_concatenation"]
    assert burst["burst_analysis"]["max_regions_per_tile"]==1
    assert "burst_analysis" not in reviewed
    run("analyze","--input",large,"--samples",65537,"--analyze-bursts",*common,success=False)
    gated=root/"middle_burst.cf32"
    gated.write_bytes(b"".join(struct.pack("<ff",math.cos(math.pi*i/128)*(1 if i%7<3 else -1),
        math.sin(math.pi*i/128)*(1 if i%7<3 else -1)) if 16384<=i<24576 else struct.pack("<ff",0,0) for i in range(65536)))
    gated_sha=hashlib.sha256(gated.read_bytes()).hexdigest()
    local=run("analyze","--input",gated,"--analyze-bursts",*common)["burst_analysis"]
    transported=run("shadow-replay","--input",gated,"--capture-samples",65536,*common)["tiles"][0]["burst_analysis"]
    for b in (local,transported):
        assert b["source_first_original_sample"]==16384 and b["source_last_original_sample"]==24575
        assert b["samples_examined"]==8192 and any(p["background_supported"] for p in b["cyclic_background"]["peaks"])
    assert local["waveform_features"]==transported["waveform_features"]
    assert local["cyclic_background"]==transported["cyclic_background"]
    gap=run("shadow-replay","--input",gated,"--capture-samples",65536,"--continuous-samples",12000,*common)
    assert all(t["burst_analysis"]["samples_examined"]==0 for t in gap["tiles"])
    assert hashlib.sha256(gated.read_bytes()).hexdigest()==gated_sha
    retimed=run("analyze","--input",gated,"--refine-clock",*common)["clock_refinement"]
    assert retimed["scope"]=="selected_contiguous_burst" and retimed["samples_examined"]==8192
    assert retimed["input_origin_original_sample"]==16384
    assert not retimed["holdout_selects_grid_or_phase"] and retimed["grids_tested"]==9
    transported_clock=run("shadow-replay","--input",gated,"--capture-samples",65536,*common)["tiles"][0]["clock_refinement"]
    for key in ("cp_candidates","code_candidates","partition_samples","canonical_samples","interpolated_samples"):
        assert retimed[key]==transported_clock[key]
    assert "clock_refinement" not in reviewed
    run("analyze","--input",large,"--samples",65537,"--refine-clock",*common,success=False)
    short_clock=run("analyze","--input",iq,"--samples",2064,"--refine-clock",*common)["clock_refinement"]
    assert short_clock["status"]=="insufficient_resampling_support" and short_clock["interpolated_samples"]==0
    background=run("analyze","--input",iq,"--offset",111,"--samples",6000,"--check-cyclic-background",*common)
    assert background["cyclic_background"]==expanded["cyclic_background"] and background["waveform_features"]==expanded["waveform_features"]
    assert "structure_discovery" not in background
    assert not background["cyclic_background"]["selected_rates_or_lags_refitted"]
    assert len(background["cyclic_background"]["peaks"])<=8 and background["cyclic_background"]["fft_calls"]<=16
    assert expanded_band["cyclic_background"]["source_sample_step"]==3
    assert all("cyclic_background" in t for t in sampled["tiles"])
    run("analyze","--input",large,"--samples",65537,"--check-cyclic-background",*common,success=False)
    structure=expanded["structure_discovery"]
    assert structure["method"]=="bounded_ofdm_barker_discovery_v1"
    assert not structure["classification_used"] and not structure["thresholds_calibrated"]
    assert structure["discovery_first_original_sample"]==111
    assert structure["holdout_last_original_sample"]==6110
    assert structure["ofdm_fft_calls"]<=6 and structure["timing_hypotheses"]<=32 and structure["spread_hypotheses"]<=32
    explicit=run("analyze","--input",iq,"--offset",111,"--samples",6000,"--discover-structure",*common)
    assert explicit["structure_discovery"]==structure and "waveform_features" not in explicit
    for key,value in report.items():
        assert explicit[key]==value,key
    short_structure=run("analyze","--input",iq,"--samples",16,"--discover-structure",*common)["structure_discovery"]
    assert short_structure["status"]=="insufficient_samples" and short_structure["carrier_estimate_hz"] is None
    assert expanded_band["structure_discovery"]["source_sample_step"]==3
    assert expanded_band["structure_discovery"]["discovery_first_original_sample"]==expanded_band["preprocessing"]["first_output_source_sample"]
    run("analyze","--input",large,"--samples",65537,"--discover-structure",*common,success=False)

    run("analyze","--input",iq,"--qualify-candidates",*common,success=False)
    assert "candidate_qualification" not in prepared_candidates
    for options in ([], ["--usable-bandwidth", "48000000"], band_options):
        base=run("analyze","--input",iq,"--offset",111,"--samples",6000,
                 "--prepare-candidates",*common,*options)
        qualified=run("analyze","--input",iq,"--offset",111,"--samples",6000,
                 "--prepare-candidates","--qualify-candidates",*common,*options)
        q=qualified.pop("candidate_qualification")
        assert qualified==base, "qualification changed existing measurements"
        assert q["proposal_accounting_exact"]
        assert q["proposals_seen"]==q["returned_proposals"]+q["duplicates_of_returned"]+q["budget_omitted_proposals"]
        assert not q["receiver_passband_calibrated"] and q["signal_detection_recall"] is None
        assert q["prepared_region_time_union_samples"]<=q["retained_region_time_union_samples"]<=q["analysis_samples"]
        assert q["output_center_extent_union_samples"]<=q["filter_input_time_union_samples"]<=q["prepared_region_time_union_samples"]
        for p,c in zip(q["candidates"],base["candidate_preparation"]["candidates"]):
            assert "link_signature_unvalidated" in p["blocking_reasons"]
            if c["status"]=="prepared_declared_passband":
                half=c["filter_taps"]//2
                step=c["source_region_original_sample_step"]
                assert p["first_filter_input_original_sample"]==c["first_original_source_sample_center"]-half*step
                assert p["last_filter_input_original_sample"]==c["last_original_source_sample_center"]+half*step
                assert p["first_filter_input_original_sample"]==c["source_region_first_sample"]
                assert p["last_filter_input_original_sample"]<=c["source_region_last_original_sample"]
                upstream=base["preprocessing"].get("filter_half_length_source_samples",0)
                assert p["first_raw_dependency_original_sample"]==p["first_filter_input_original_sample"]-upstream
                assert p["last_raw_dependency_original_sample"]==p["last_filter_input_original_sample"]+upstream
                assert 111<=p["first_raw_dependency_original_sample"]<=p["last_raw_dependency_original_sample"]<6111
                assert p["unconsumed_tail_analysis_samples"]<c["original_source_sample_step"]//step
                # The narrow tone is filtered successfully but too short for
                # separate cyclic partitions after decimation.
                assert not p["cyclic_support_available"]
    assert hashlib.sha256(iq.read_bytes()).hexdigest()==checksum


print("cyclostationary CLI checks passed")

# General sweep options and metadata remain separate from legacy evidence.
