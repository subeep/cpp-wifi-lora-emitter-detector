#include "cyclostationary/shadow_worker.hpp"
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <limits>
#include <random>
#include <regex>
#include <unistd.h>
#include <fcntl.h>
#include <sys/resource.h>

using namespace rfmon::cyclo;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F f, const char* message) { try { f(); } catch (const std::exception&) { return; } throw std::runtime_error(message); }
template<class F> void until(F f) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!f()) { if (std::chrono::steady_clock::now() > end) throw std::runtime_error("process wait timed out"); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
}
std::string read_file(const std::string& name) { std::ifstream f(name); return {std::istreambuf_iterator<char>(f), {}}; }
void fill_clock(TileMeasurement& t,const std::vector<std::complex<float>>& iq) {
    if(t.burst.selection.samples) {
        const auto begin=iq.begin()+t.burst.selection.offset;
        t.clock=refine_structure_clock({begin,begin+t.burst.selection.samples},20e6,t.burst.structure);
    } else t.clock=refine_structure_clock(iq,20e6,t.structure);
}
void protocol_tests(const std::vector<std::complex<float>>& iq) {
    const auto plan = plan_sample_tiles(iq.size());
    auto input = encode_request(171, 20e6, plan, iq); const auto decoded = decode_request(input);
    check(decoded.job_id == 171 && decoded.tiles.size() == 1 && decoded.tiles[0].iq == iq, "portable request round trip");
    auto overflowing = plan; overflowing.tiles[0].source_offset = std::numeric_limits<std::size_t>::max();
    rejects([&] { encode_request(171, 20e6, overflowing, iq); }, "overflowing source span encoded");
    auto malformed = input; std::fill_n(malformed.begin() + 20, 8, 255);
    rejects([&] { decode_request(malformed); }, "overflowing source span decoded");
    TileMeasurement measured; measured.source = plan.tiles[0];
    SpectralConfig c; c.sample_rate_hz = 20e6; c.max_frames = 256;
    measured.spectral = measure_spectral_correlation(iq, c);
    measured.ofdm = measure_ofdm_structure(iq, 20e6, wlan_ofdm_hypotheses(20e6));
    measured.roi = measure_rois(iq, 20e6);
    measured.chirps = measure_chirp_structure(iq, 20e6);
    measured.waveform=measure_waveform_features(iq,20e6);
    measured.cyclic_background=measure_cyclic_background(iq,20e6,measured.waveform);
    measured.structure=discover_waveform_structure(iq,20e6);
    measured.sweeps=discover_linear_sweeps(iq,20e6);
    measured.burst=analyze_burst(iq,20e6,measured.roi);fill_clock(measured,iq);
    auto bad_sweep=measured;bad_sweep.sweeps.discovery_trials++;
    rejects([&] { decode_result(encode_result(171,{bad_sweep}),171,20e6,plan); },"invented sweep search coverage accepted");
    bad_sweep=measured;bad_sweep.sweeps.holdout_partition_offset=0;
    rejects([&] { decode_result(encode_result(171,{bad_sweep}),171,20e6,plan); },"overlapping sweep partitions accepted");
    std::vector<std::complex<float>> sweep_iq(iq.size());
    for(std::size_t i=0;i<sweep_iq.size();++i){const double t=double(i%128)-63.5;
        sweep_iq[i]=std::polar(1.f,float(2*3.14159265358979323846*.5*(.5/127)*t*t));}
    auto with_sweep=measured;with_sweep.sweeps=discover_linear_sweeps(sweep_iq,20e6);
    check(!with_sweep.sweeps.candidates.empty(),"sweep protocol positive fixture absent");
    const auto sweep_roundtrip=decode_result(encode_result(171,{with_sweep}),171,20e6,plan);
    check(sweep_roundtrip[0].sweeps.candidates[0].pattern_consistent &&
        sweep_roundtrip[0].sweep_review.candidates[0].kind=="linear_sweep","sweep protocol or supervisor review missing");
    bad_sweep=with_sweep;bad_sweep.sweeps.candidates[0].holdout_offset=0;
    rejects([&] {decode_result(encode_result(171,{bad_sweep}),171,20e6,plan);},"held sweep coordinates escape partition");
    bad_sweep=with_sweep;bad_sweep.sweeps.candidates[0].slope_hz_per_second=NAN;
    rejects([&] {decode_result(encode_result(171,{bad_sweep}),171,20e6,plan);},"nonfinite sweep slope accepted");
    bad_sweep=with_sweep;bad_sweep.sweeps.candidates[0].pattern_consistent=false;
    rejects([&] {decode_result(encode_result(171,{bad_sweep}),171,20e6,plan);},"inconsistent sweep flag accepted");
    bad_sweep=with_sweep;bad_sweep.sweeps.candidates[0].span_samples=65;
    rejects([&] {decode_result(encode_result(171,{bad_sweep}),171,20e6,plan);},"unsearched sweep span accepted");
    bad_sweep=with_sweep;bad_sweep.sweeps.candidates[0].holdout_trials++;
    rejects([&] {decode_result(encode_result(171,{bad_sweep}),171,20e6,plan);},"invented held trials accepted");
    const auto bytes = encode_result(171, {measured}); const auto restored = decode_result(bytes, 171, 20e6, plan);
    check(restored[0].spectral.peaks.size() == measured.spectral.peaks.size(), "validated result round trip");
    check(!restored[0].review.candidates.empty() && restored[0].review.context.usable_bandwidth_hz==0,"supervisor did not populate bounded review with unknown live passband");
    rejects([&] { decode_result(bytes, 172, 20e6, plan); }, "wrong job id accepted");
    auto wrong_plan = plan; ++wrong_plan.tiles[0].source_offset;
    rejects([&] { decode_result(bytes, 171, 20e6, wrong_plan); }, "wrong source coordinates accepted");
    check(restored[0].cyclic_background.peaks.size()==measured.waveform.peaks.size() &&
        restored[0].background_review.candidates.size()==measured.waveform.peaks.size(),"background review not populated by supervisor");
    auto bad_background=measured;bad_background.cyclic_background.fft_calls++;
    rejects([&]{decode_result(encode_result(171,{bad_background}),171,20e6,plan);},"invented background FFT count accepted");
    bad_background=measured;bad_background.cyclic_background.peaks[0].holdout.line_to_upper+=1;
    rejects([&]{decode_result(encode_result(171,{bad_background}),171,20e6,plan);},"inconsistent local background ratio accepted");
    bad_background=measured;bad_background.cyclic_background.peaks[0].holdout.reference_bins=999;
    rejects([&]{decode_result(encode_result(171,{bad_background}),171,20e6,plan);},"invented local background coverage accepted");
    bad_background=measured;bad_background.cyclic_background.peaks[0].discovery.block_phase_coherence_squared=NAN;
    rejects([&]{decode_result(encode_result(171,{bad_background}),171,20e6,plan);},"nonfinite block phase consistency accepted");
    bad_background=measured;bad_background.cyclic_background.peaks[0].background_supported=!bad_background.cyclic_background.peaks[0].background_supported;
    rejects([&]{decode_result(encode_result(171,{bad_background}),171,20e6,plan);},"inconsistent background support flag accepted");
    auto bad = measured; bad.spectral.peaks[0].coherence_squared = 1.01;
    rejects([&] { decode_result(encode_result(171, {bad}), 171, 20e6, plan); }, "out-of-range coherence accepted");
    bad = measured; bad.spectral.mean_i = NAN;
    rejects([&] { decode_result(encode_result(171, {bad}), 171, 20e6, plan); }, "non-finite result accepted");
    bad = measured; bad.roi.regions[0].samples = iq.size() + 1;
    rejects([&] { decode_result(encode_result(171, {bad}), 171, 20e6, plan); }, "escaping ROI accepted");
    bad = measured; bad.roi.regions[0].spectral_samples = 1;
    rejects([&] { decode_result(encode_result(171, {bad}), 171, 20e6, plan); }, "invented PSD coverage accepted");
    bad = measured; bad.roi.high_threshold_to_mean += 1;
    rejects([&] { decode_result(encode_result(171, {bad}), 171, 20e6, plan); }, "inconsistent ROI threshold accepted");
    bad = measured; bad.roi.regions[0].energy_fraction = NAN;
    rejects([&] { decode_result(encode_result(171, {bad}), 171, 20e6, plan); }, "non-finite ROI energy accepted");
    bad = measured; bad.chirps[0].peak_offset = iq.size();
    rejects([&] { decode_result(encode_result(171, {bad}), 171, 20e6, plan); }, "escaping chirp position accepted");
    bad = measured; bad.chirps[0].positions_examined += 1;
    rejects([&] { decode_result(encode_result(171, {bad}), 171, 20e6, plan); }, "invented chirp search coverage accepted");
    bad = measured; bad.chirps[0].slope_hz_per_second *= -1;
    rejects([&] { decode_result(encode_result(171, {bad}), 171, 20e6, plan); }, "unexpected chirp hypothesis accepted");
    bad = measured; bad.chirps[0].peak_coherence_squared = NAN;
    rejects([&] { decode_result(encode_result(171, {bad}), 171, 20e6, plan); }, "non-finite chirp result accepted");
    bad = measured; bad.chirps[0].status = "insufficient_samples";
    rejects([&] { decode_result(encode_result(171, {bad}), 171, 20e6, plan); }, "invented chirp status accepted");
    bad = measured; bad.chirps[0].frequency_pairs = iq.size();
    rejects([&] { decode_result(encode_result(171, {bad}), 171, 20e6, plan); }, "invented phase support accepted");
    bad = measured; bad.chirps[0].frequency_rmse_hz = INFINITY;
    rejects([&] { decode_result(encode_result(171, {bad}), 171, 20e6, plan); }, "non-finite frequency residual accepted");
    bad = measured; bad.chirps[0].frequency_mean_hz = NAN;
    rejects([&] { decode_result(encode_result(171, {bad}), 171, 20e6, plan); }, "non-finite frequency center accepted");
    bad=measured; bad.waveform.holdout_offset=0;
    rejects([&] {decode_result(encode_result(171,{bad}),171,20e6,plan);},"overlapping waveform partitions accepted");
    bad=measured; bad.waveform.amplitude_cv=NAN;
    rejects([&] {decode_result(encode_result(171,{bad}),171,20e6,plan);},"nonfinite morphology accepted");
    bad=measured; bad.waveform.two_frequency_pattern=!bad.waveform.two_frequency_pattern;
    rejects([&] {decode_result(encode_result(171,{bad}),171,20e6,plan);},"invented morphology flag accepted");
    if(!measured.waveform.peaks.empty()) {
        bad=measured; bad.waveform.peaks[0].alpha_hz=20e6;
        rejects([&] {decode_result(encode_result(171,{bad}),171,20e6,plan);},"outside cyclic frequency accepted");
        bad=measured; bad.waveform.peaks[0].lag_samples=3;
        rejects([&] {decode_result(encode_result(171,{bad}),171,20e6,plan);},"unrequested cyclic lag accepted");
    }

    bad=measured;bad.structure.holdout_offset=0;
    rejects([&]{decode_result(encode_result(171,{bad}),171,20e6,plan);},"overlapping structure partitions accepted");
    bad=measured;bad.structure.spread_hypotheses=33;
    rejects([&]{decode_result(encode_result(171,{bad}),171,20e6,plan);},"excess structure search accepted");
    if(!measured.structure.ofdm.empty()) {
        bad=measured;bad.structure.ofdm[0].phase_samples=99999;
        rejects([&]{decode_result(encode_result(171,{bad}),171,20e6,plan);},"invalid discovered CP phase accepted");
        bad=measured;bad.structure.ofdm[0].pattern_consistent=!bad.structure.ofdm[0].pattern_consistent;
        rejects([&]{decode_result(encode_result(171,{bad}),171,20e6,plan);},"invented CP pattern accepted");
    }
    if(!measured.structure.spread.empty()) {
        bad=measured;bad.structure.spread[0].carrier_hz=20e6;
        rejects([&]{decode_result(encode_result(171,{bad}),171,20e6,plan);},"outside short-code carrier accepted");
        bad=measured;bad.structure.spread[0].holdout_words=99999;
        rejects([&]{decode_result(encode_result(171,{bad}),171,20e6,plan);},"invented short-code support accepted");
        bad=measured;bad.structure.spread[0].holdout_code_coherence_squared=NAN;
        rejects([&]{decode_result(encode_result(171,{bad}),171,20e6,plan);},"nonfinite short-code score accepted");
    }

    // Sparse zero-mean impulses can legitimately have fewer than four code
    // candidates: eight complete nonzero words are a prerequisite, not a quota.
    auto sparse=measured; std::vector<std::complex<float>> impulses(iq.size());
    const auto n=measured.structure.partition_samples, offset=measured.structure.holdout_offset;
    if(n){impulses[0]={1,0};impulses[1]={-1,0};impulses[offset]={1,0};impulses[offset+1]={-1,0};}
    sparse.structure=discover_waveform_structure(impulses,20e6);fill_clock(sparse,impulses);
    check(sparse.structure.spread.empty(),"sparse fixture unexpectedly supplies eight code words");
    decode_result(encode_result(171,{sparse}),171,20e6,plan);
    auto tone = iq;
    for (std::size_t i = 0; i < tone.size(); ++i)
        tone[i] = 0.001f * tone[i] + std::complex<float>(std::polar(1.0, 2 * 3.14159265358979323846 * 64 * (i % 512) / 512));
    auto with_band = measured; with_band.roi = measure_rois(tone, 20e6); with_band.burst=analyze_burst(tone,20e6,with_band.roi);fill_clock(with_band,tone);
    check(!with_band.roi.regions[0].bands.empty(), "band validation fixture has no interval");
    decode_result(encode_result(171, {with_band}), 171, 20e6, plan);
    bad = with_band; bad.roi.regions[0].bands[0].low_hz = -20e6;
    rejects([&] { decode_result(encode_result(171, {bad}), 171, 20e6, plan); }, "outside-Nyquist interval accepted");
    bad = with_band; bad.roi.regions[0].bands[0].contains_dc = true;
    rejects([&] { decode_result(encode_result(171, {bad}), 171, 20e6, plan); }, "invented DC membership accepted");
    bad = with_band; bad.roi.regions[0].bands[0].edge_bin = true;
    rejects([&] { decode_result(encode_result(171, {bad}), 171, 20e6, plan); }, "invented edge flag accepted");
    for (std::size_t n : {std::size_t(0), std::size_t(8), bytes.size() - 1}) {
        auto truncated = bytes; truncated.resize(n);
        rejects([&] { decode_result(truncated, 171, 20e6, plan); }, "truncated result accepted");
    }
    auto tail = bytes; tail.push_back(0);
    rejects([&] { decode_result(tail, 171, 20e6, plan); }, "trailing result bytes accepted");
    auto header = encode_header(MessageType::result, reply_byte_limit + 1);
    rejects([&] { decode_header(header, reply_byte_limit); }, "oversized message header accepted");
    header = encode_header(MessageType::result, 0); header[4] = wire_version + 1;
    rejects([&] { decode_header(header, reply_byte_limit); }, "wrong wire version accepted");
    header[4] = 2;
    rejects([&] { decode_header(header, reply_byte_limit); }, "old worker wire version accepted");
    auto burst_iq=iq;std::fill(burst_iq.begin(),burst_iq.end(),std::complex<float>{});
    for(std::size_t i=1024;i<4096;++i)burst_iq[i]=(i%7<3?1.f:-1.f)*std::polar(1.f,float(3.14159265358979323846*(i%256)/128));
    auto burst_tile=measured;burst_tile.roi=measure_rois(burst_iq,20e6);
    burst_tile.burst=analyze_burst(burst_iq,20e6,burst_tile.roi);fill_clock(burst_tile,burst_iq);
    check(burst_tile.burst.selection.samples>0,"protocol burst fixture absent");
    const auto burst_wire=encode_result(171,{burst_tile});const auto br=decode_result(burst_wire,171,20e6,plan);
    check(br[0].burst.selection.offset==burst_tile.burst.selection.offset &&
        br[0].burst.background_review.candidates.size()==burst_tile.burst.waveform.peaks.size(),"burst supervisor review absent");
    auto broken=burst_tile;broken.burst.selection.offset++;
    rejects([&]{decode_result(encode_result(171,{broken}),171,20e6,plan);},"invented burst coordinates accepted");
    broken=burst_tile;broken.burst.selection.eligible_regions++;
    rejects([&]{decode_result(encode_result(171,{broken}),171,20e6,plan);},"invented burst selection coverage accepted");
    broken=burst_tile;broken.burst.selection.cropped=true;
    rejects([&]{decode_result(encode_result(171,{broken}),171,20e6,plan);},"invented burst crop flag accepted");
    broken=burst_tile;broken.burst.selection.samples=burst_sample_limit+1;
    rejects([&]{decode_result(encode_result(171,{broken}),171,20e6,plan);},"burst sample budget escape accepted");
    broken=burst_tile;broken.burst.spectral.power_fraction[0]=NAN;
    rejects([&]{decode_result(encode_result(171,{broken}),171,20e6,plan);},"nonfinite compact PSD accepted");
    broken=burst_tile;broken.burst.waveform.holdout_offset=0;
    rejects([&]{decode_result(encode_result(171,{broken}),171,20e6,plan);},"burst partitions overlap");
    broken=burst_tile;broken.burst.background.peaks[0].background_supported=!broken.burst.background.peaks[0].background_supported;
    rejects([&]{decode_result(encode_result(171,{broken}),171,20e6,plan);},"invented burst cyclic support accepted");
    check(!measured.clock.cp.empty() && !measured.clock.code.empty(),"clock corruption fixture lacks retained measurements");
    auto bad_clock=measured;bad_clock.clock.grids_tested++;
    rejects([&]{decode_result(encode_result(171,{bad_clock}),171,20e6,plan);},"invented timing grid count accepted");
    bad_clock=measured;bad_clock.clock.holdout_offset=0;
    rejects([&]{decode_result(encode_result(171,{bad_clock}),171,20e6,plan);},"overlapping timing partitions accepted");
    bad_clock=measured;bad_clock.clock.cp[0].grid_index=9;
    rejects([&]{decode_result(encode_result(171,{bad_clock}),171,20e6,plan);},"unsearched timing grid accepted");
    bad_clock=measured;bad_clock.clock.cp[0].seed_index=2;
    rejects([&]{decode_result(encode_result(171,{bad_clock}),171,20e6,plan);},"unretained timing seed accepted");
    bad_clock=measured;bad_clock.clock.interpolated_samples++;
    rejects([&]{decode_result(encode_result(171,{bad_clock}),171,20e6,plan);},"invented interpolation count accepted");
    bad_clock=measured;bad_clock.clock.cp[0].measurement.pattern_consistent=!bad_clock.clock.cp[0].measurement.pattern_consistent;
    rejects([&]{decode_result(encode_result(171,{bad_clock}),171,20e6,plan);},"invented refined CP flag accepted");
    bad_clock=measured;bad_clock.clock.code[0].measurement.carrier_hz+=100;
    rejects([&]{decode_result(encode_result(171,{bad_clock}),171,20e6,plan);},"held-refitted code carrier accepted");
    bad_clock=measured;bad_clock.clock.code[0].measurement.train_code_coherence_squared=NAN;
    rejects([&]{decode_result(encode_result(171,{bad_clock}),171,20e6,plan);},"nonfinite timing metric accepted");
    bad_clock=measured;bad_clock.clock.code.push_back(bad_clock.clock.code[0]);
    rejects([&]{encode_result(171,{bad_clock});},"timing retained candidate budget escape accepted");
    // Maximum retained ROI/band metadata plus full spectra across four tiles
    // must remain inside the unchanged reply budget and decode consistently.
    std::vector<std::complex<float>> dense(65536);
    for (std::size_t i = 0; i < dense.size(); ++i) {
        dense[i] = 0.001f * iq[i % iq.size()];
        if ((i>=2048 && i<18432) || (i>=20480 && i<49152 && (i-20480)%4096<3072))
            for (int bin : {-64, 32, 96})
                dense[i] += std::complex<float>(std::polar(1.0, 2 * 3.14159265358979323846 * bin * (i % 512) / 512));
    }
    TileMeasurement full; full.spectral = measure_spectral_correlation(dense, c);
    full.ofdm = measure_ofdm_structure(dense, 20e6, wlan_ofdm_hypotheses(20e6)); full.roi = measure_rois(dense, 20e6);
    full.chirps = measure_chirp_structure(dense, 20e6);
    full.burst=analyze_burst(dense,20e6,full.roi);
    full.waveform=measure_waveform_features(dense,20e6);
    full.cyclic_background=measure_cyclic_background(dense,20e6,full.waveform);
    full.structure=discover_waveform_structure(dense,20e6);
    sweep_iq.resize(dense.size());
    for(std::size_t i=0;i<sweep_iq.size();++i){const double t=double(i%512)-255.5;
        const double slope=(i/512)%3==0?.4/511:(i/512)%3==1?-.5/511:.6/511;
        sweep_iq[i]=std::polar(1.f,float(2*3.14159265358979323846*.5*slope*t*t));}
    full.sweeps=discover_linear_sweeps(sweep_iq,20e6);
    check(full.burst.selection.samples>0,"full protocol fixture lacks burst payload");
    const std::vector<std::complex<float>> full_local(dense.begin()+full.burst.selection.offset,dense.begin()+full.burst.selection.offset+full.burst.selection.samples);
    full.burst.waveform=measure_waveform_features(full_local,20e6);
    full.burst.background=measure_cyclic_background(full_local,20e6,full.burst.waveform);
    // Force all eight legal selected CAF records and distinct cached lags in
    // both scopes, plus three locally observed sweeps. Metadata stress only;
    // this intentionally combines measurements from different software signals.
    auto populate=[](WaveformFeatures& w) {
        w.peaks.clear();w.refinement_evaluations=40;
        for(bool conjugate:{false,true})for(std::size_t j=0;j<4;++j) {
            const double alpha=(32+16*j)*w.alpha_bin_hz;
            w.peaks.push_back({conjugate,cyclic_lags[j],alpha,alpha,.1,.1,true});
        }
    };
    populate(full.waveform);full.cyclic_background=measure_cyclic_background(dense,20e6,full.waveform);
    populate(full.burst.waveform);full.burst.background=measure_cyclic_background(full_local,20e6,full.burst.waveform);
    const std::vector<std::complex<float>> local_sweeps(sweep_iq.begin(),sweep_iq.begin()+full.burst.selection.samples);
    full.burst.sweeps=discover_linear_sweeps(local_sweeps,20e6);
    check(full.burst.sweeps.candidates.size()==3 && full.burst.background.fft_calls==16 && full.cyclic_background.fft_calls==16,
        "maximum local metadata fixture not populated");
    check(full.sweeps.candidates.size()==3,"maximum sweep metadata fixture not populated");
    check(full.roi.regions.size() == 8 && full.roi.regions[0].bands.size() == 3, "maximum metadata fixture not populated");
    fill_clock(full,dense);
    const auto complete_plan = plan_sample_tiles(capture_sample_budget);
    std::vector<TileMeasurement> complete;
    for (std::size_t i = 0; i < complete_plan.count; ++i) { full.source = complete_plan.tiles[i]; complete.push_back(full); }
    const auto largest = encode_result(172, complete);
    check(largest.size() <= reply_byte_limit && decode_result(largest, 172, 20e6, complete_plan).size() == 4,
          "full ROI metadata exceeds bounded IPC profile");
    std::cout<<"four-tile burst metadata reply bytes "<<largest.size()<<" / "<<reply_byte_limit<<'\n';
    std::mt19937 rng(581);
    for (int trial = 0; trial < 500; ++trial) {
        WireBytes random(std::size_t(trial % 300)); for (auto& v : random) v = rng() & 255;
        rejects([&] { decode_result(random, 171, 20e6, plan); }, "random result accepted");
    }
}
void sampling_tests() {
    for (std::size_t size : {2048ul, 5000ul, 65536ul, 100003ul, 262144ul, 20000000ul,
                             std::numeric_limits<std::size_t>::max()}) {
        const auto p = plan_sample_tiles(size);
        check(p.count >= 1 && p.count <= 4 && p.samples <= 262144, "tile budget exceeded");
        std::size_t last_end = 0;
        for (std::size_t t = 0; t < p.count; ++t) {
            const auto& tile = p.tiles[t];
            check(tile.samples >= 2048 && tile.samples <= 65536 && tile.source_offset >= last_end &&
                  tile.source_offset <= size - tile.samples, "tile range overlaps or escapes source");
            last_end = tile.source_offset + tile.samples;
        }
        check(last_end == size, "last bounded tile misses capture end");
    }
    check(plan_sample_tiles(2047).count == 0, "short prefix must abstain");
    std::size_t calls = 0, last_index = 0;
    const auto capped = collect_burst_hints(4000, 20000000, true, [&](std::size_t i) {
        ++calls; last_index = i; return SampleTile{100000 + i * 4000, 12000};
    });
    check(calls == 512 && last_index == 3999 && capped.count == 512 && capped.detector_capped,
          "raw hints bounded and distributed over detector output");
    const auto extreme = collect_burst_hints(std::numeric_limits<std::size_t>::max(),
        std::numeric_limits<std::size_t>::max(), false, [](std::size_t i) { return SampleTile{i, 1}; });
    check(extreme.examined == 512 && extreme.count == 512, "descriptor sampling index overflow");
    const std::array<SampleTile, 5> gaps = {{{100, 0}, {9000, 2000}, {10000, 1},
        {std::numeric_limits<std::size_t>::max(), 100}, {2000, 1000}}};
    const auto valid = collect_burst_hints(gaps.size(), 10000, false, [&](std::size_t i) { return gaps[i]; });
    check(valid.count == 1 && valid.rejected == 4, "invalid or gap-crossing hints must be rejected");
    const auto short_plan = select_sample_tiles(10000, valid);
    check(short_plan.plan.samples == 10000 && short_plan.burst_windows == 0,
          "fully covered short capture should remain unchanged");
    std::mt19937_64 rng(87132);
    for (int trial = 0; trial < 1000; ++trial) {
        const std::size_t prefix = trial == 0 ? std::numeric_limits<std::size_t>::max() : 262145 + rng() % 1000000000;
        const auto hints = collect_burst_hints(512, prefix, false, [&](std::size_t) {
            const auto offset = rng() % prefix;
            return SampleTile{offset, std::min(std::size_t(1 + rng() % 200000), prefix - offset)};
        });
        const auto selected = select_sample_tiles(prefix, hints);
        check(selected.plan.count <= 4 && selected.plan.samples <= capture_sample_budget &&
              selected.burst_windows <= 2, "guided selection budget exceeded");
        std::size_t end = 0, total = 0;
        for (std::size_t i = 0; i < selected.plan.count; ++i) {
            const auto& b = selected.plan.tiles[i];
            check(b.samples >= 2048 && b.samples <= tile_sample_limit && b.source_offset >= end &&
                  b.source_offset <= prefix - b.samples, "guided selection overlap or out-of-range window");
            end = b.source_offset + b.samples; total += b.samples;
        }
        check(selected.plan.tiles[0].source_offset == 0 && end == prefix && total == selected.plan.samples,
              "guided selection lost context endpoints or sample accounting");
    }
}
}
int main(int argc, char** argv) {
    try {
        check(argc == 6, "requires real worker and four broken worker paths");
        std::mt19937 rng(111); std::normal_distribution<float> normal;
        std::vector<std::complex<float>> iq(5000); for (auto& z : iq) z = {normal(rng), normal(rng)};
        protocol_tests(iq); sampling_tests();
        WorkerOptions options; options.executable = argv[1];
        ProcessClient client(options);
        const int sentinel = open("/dev/null", O_RDONLY); check(sentinel >= 3, "sentinel descriptor failed");
        client.start([] { return false; }); const int pid = client.pid();
        const auto limits = read_file("/proc/" + std::to_string(pid) + "/limits");
        check(std::regex_search(limits, std::regex("Max address space\\s+134217728\\s+134217728")) &&
              std::regex_search(limits, std::regex("Max cpu time\\s+30\\s+30")) &&
              std::regex_search(limits, std::regex("Max open files\\s+32\\s+32")) &&
              std::regex_search(limits, std::regex("Max core file size\\s+0\\s+0")),
              "worker limits missing");
        std::size_t handles = 0;
        for (const auto& fd : std::filesystem::directory_iterator("/proc/" + std::to_string(pid) + "/fd")) {
            ++handles; check(std::stoi(fd.path().filename().string()) <= 3, "unrelated parent descriptor inherited");
        }
        check(handles == 4, "worker descriptor profile changed"); close(sentinel);
        const auto status = read_file("/proc/" + std::to_string(pid) + "/status");
        check(std::regex_search(status, std::regex("NoNewPrivs:\\s+1")) &&
              std::regex_search(status, std::regex("Threads:\\s+1")) && getpriority(PRIO_PROCESS, pid) >= 10,
              "worker privilege/priority/thread profile changed");
        const auto result = client.analyze(19, 20e6, plan_sample_tiles(iq.size()), iq, [] { return false; });
        check(result.size() == 1 && result[0].spectral.quality == "measured", "real process measurement missing");
        client.terminate(); check(!std::filesystem::exists("/proc/" + std::to_string(pid)), "worker was not reaped");
        for (int fixture = 2; fixture < argc; ++fixture) {
            WorkerOptions faults; faults.executable = argv[fixture]; faults.deadline_ms = 150; faults.restart_limit = 1;
            ShadowWorker worker(faults); check(worker.set_enabled(true), "fault test starts");
            CaptureContext c; c.band_ghz = 2; c.sample_rate_hz = 20e6; c.capture_center_hz = 2438.5e6;
            c.source_samples = iq.size(); c.submission_epoch = worker.epoch();
            until([&] { return worker.try_submit(iq, iq.size(), c); });
            until([&] { return worker.stats().discarded > 0; });
            check(!worker.snapshot() && worker.enabled(), "fault published a result or skipped allowed recovery");
            until([&] { return worker.try_submit(iq, iq.size(), c); });
            until([&] { return !worker.enabled(); });
            const auto stats = worker.stats();
            check(stats.failures == 2 && stats.worker_restarts == 1 && !worker.snapshot(), "restart limit not honored");
            if (fixture == 3) check(stats.worker_timeouts == 2, "hung worker missed deadline");
            if (fixture >= 4) check(stats.protocol_errors == 2, "bad reply not rejected");
            const auto start = std::chrono::steady_clock::now(); worker.set_enabled(false);
            check(std::chrono::steady_clock::now() - start < std::chrono::seconds(1), "fault shutdown unbounded");
        }
        // A fault followed by a healthy executable must recover on the next
        // queued capture, with the same bounded supervisor instance.
        const auto directory = std::filesystem::temp_directory_path() / ("cyclo-recovery-" + std::to_string(getpid()));
        std::filesystem::create_directory(directory);
        const auto link = directory / "worker";
        std::filesystem::create_symlink(argv[2], link);
        {
            WorkerOptions recovery; recovery.executable = link.string();
            ShadowWorker recovering(recovery); recovering.set_enabled(true);
            CaptureContext context; context.band_ghz = 2; context.sample_rate_hz = 20e6; context.capture_center_hz = 2438.5e6;
            context.source_samples = iq.size(); context.submission_epoch = recovering.epoch();
            until([&] { return recovering.try_submit(iq, iq.size(), context); });
            until([&] { return recovering.stats().discarded > 0; });
            std::filesystem::remove(link); std::filesystem::create_symlink(argv[1], link);
            context.capture_sequence = 7;
            until([&] { return recovering.try_submit(iq, iq.size(), context); });
            until([&] { return bool(recovering.snapshot()); });
            check(recovering.snapshot()->capture.capture_sequence == 7 && recovering.stats().worker_restarts == 1 &&
                  recovering.snapshot()->tiles.front().spectral.quality == "measured", "worker did not recover after fault");
            // Eight valid requests cause routine recycling without using the
            // fault allowance. A ninth must still return a valid measurement.
            const auto launches = recovering.stats().worker_launches;
            for (int request = 0; request < 9; ++request) {
                const auto before = recovering.stats().measured; ++context.capture_sequence;
                until([&] { return recovering.try_submit(iq, iq.size(), context); });
                until([&] { return recovering.stats().measured > before; });
            }
            check(recovering.stats().worker_launches > launches && recovering.stats().worker_restarts == 1,
                  "routine recycle consumes fault allowance");
            recovering.set_enabled(false);
        }
        std::filesystem::remove_all(directory);
        WorkerOptions hung; hung.executable = argv[3]; hung.deadline_ms = 5000;
        ShadowWorker worker(hung); worker.set_enabled(true);
        CaptureContext c; c.band_ghz = 2; c.sample_rate_hz = 20e6; c.capture_center_hz = 2438.5e6;
        c.source_samples = iq.size(); c.submission_epoch = worker.epoch();
        until([&] { return worker.try_submit(iq, iq.size(), c); });
        until([&] { return worker.stats().worker_pid > 0; }); worker.invalidate();
        until([&] { return worker.stats().discarded > 0; });
        check(worker.stats().failures == 0 && worker.enabled(), "cancelled stale job consumed fault allowance");
        c.submission_epoch = worker.epoch();
        until([&] { return worker.try_submit(iq, iq.size(), c); });
        until([&] { return worker.stats().worker_pid > 0; });
        const auto stop_start = std::chrono::steady_clock::now(); worker.set_enabled(false);
        check(std::chrono::steady_clock::now() - stop_start < std::chrono::seconds(1), "stop waits for hung request deadline");
        std::cout << "process crash/hang/bad-reply/limits/descriptor/sampling checks passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
