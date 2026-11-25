// Offline check of the app's link measurements on wideband drone recordings (public datasets and
// our own X310 sessions). No radio.
//
// A recording at 20-200 MSps (an integer multiple of 20 MSps) is cut into the 20 MSps views the
// scanner would capture: one every --view-step Hz across the recorded band, each a 20 MHz channel
// with captures of --capture-s seconds. Only the 24 blocks the timing monitor takes from a capture
// (block_start) are formed, by fast convolution: one FFT of the block's stretch of the recording,
// the view's bins tapered off between 9 and 10 MHz, and an inverse FFT, with a margin either side
// that is dropped. The mixing phase is kept continuous across blocks. Each view's capture then
// goes through the live measurement unchanged (symbol_timing::measure_blocks: timing group, grid
// offsets, link clock with its symbol phase).
//
// With --droneid, DJI DroneID is decoded over the whole recording at every documented channel in
// its band (the live worker's decoder, which resamples itself), and each packet's offset is
// given in Hz and ppm of its channel as the app does.
//
// Output: one JSON line per view capture ("link") and per packet ("droneid") in the live offset
// log's format (data/drone_offsets.jsonl), so drone_log_replay can replay a recording through the
// app's rows and the Drones page grouping. Times: device_ns and unix_ms count from the recording's
// start (plus --time0 for a recording kept in pieces, plus 1 s); session is 1 for one recording.
// Extra fields: t_s (capture start in the recording), view_offset_hz, fractions.
//
//   drone_corpus_check --rate R --centre F [--input FILE|-] [--format cf32|sc16] [--run ID]
//                      [--time0 S] [--capture-s 0.5] [--view-step 5e6] [--views F1,F2,...]
//                      [--droneid] [--seq0 N] [--threads N]
//                      [--symbol-grid] [--dense-s S] [--nominal-15k 14000] [--nominal-30k 28000]
// --traits-s S: per view capture with a timing group, the periodicity spectra (50 Hz bands to
// 30 kHz, and the strongest lines) of the group's cyclic-prefix metric and of the power envelope
// over S contiguous seconds (kind "traits"): the link's rhythms, set by its protocol.
// --dense-s adds a pass of S-second captures (kind "dense", views with a timing group only) with the
// symbol-grid prototype, for frame timing across captures (Phase 2).
// Input is read whole into memory (stdin by default).

#include "drones/symbol_timing_monitor.hpp"
#ifdef RFMON_CORPUS_CHECK_DRONEID
#include "drones/droneid_packet.hpp"
#include "droneid/dji.h"
#endif
#include <fftw3.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace rfmon;
using Json = nlohmann::json;
using cf = std::complex<float>;
namespace st = rfmon::symbol_timing;

namespace {
constexpr double out_rate = 20e6;
constexpr double pass_hz = 9e6;      // flat to here, cosine taper to the output Nyquist
constexpr std::size_t margin = 8192;  // output samples either side of a block, dropped
constexpr double pi = 3.14159265358979323846;

std::vector<cf> read_input(const std::string& path, const std::string& format) {
    std::vector<char> bytes;
    std::istream* in = &std::cin;
    std::ifstream file;
    if (path != "-") {
        file.open(path, std::ios::binary);
        if (!file) throw std::runtime_error("cannot open " + path);
        in = &file;
    }
    std::vector<char> chunk(1 << 24);
    while (*in) {
        in->read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        bytes.insert(bytes.end(), chunk.data(), chunk.data() + in->gcount());
    }
    std::vector<cf> iq;
    if (format == "cf32") {
        iq.resize(bytes.size() / sizeof(cf));
        std::memcpy(iq.data(), bytes.data(), iq.size() * sizeof(cf));
    } else if (format == "sc16") {
        const std::size_t n = bytes.size() / 4;
        iq.resize(n);
        const auto* s = reinterpret_cast<const int16_t*>(bytes.data());
        for (std::size_t i = 0; i < n; ++i) iq[i] = {s[2 * i] / 32768.f, s[2 * i + 1] / 32768.f};
    } else {
        throw std::runtime_error("unknown format " + format);
    }
    return iq;
}

// One block's stretch of the recording in the frequency domain, shared by all views.
struct Channelizer {
    std::size_t decim, n_in, n_out;
    double rate;
    fftwf_plan forward{}, inverse{};
    std::vector<float> taper;  // per output bin (FFT order)
    Channelizer(std::size_t d, double r) : decim(d), n_in((st::block_size + 2 * margin) * d),
        n_out(st::block_size + 2 * margin), rate(r), taper(n_out) {
        auto* a = fftwf_alloc_complex(n_in);
        auto* b = fftwf_alloc_complex(n_out);
        forward = fftwf_plan_dft_1d(int(n_in), a, a, FFTW_FORWARD, FFTW_ESTIMATE);
        inverse = fftwf_plan_dft_1d(int(n_out), b, b, FFTW_BACKWARD, FFTW_ESTIMATE);
        fftwf_free(a); fftwf_free(b);
        for (std::size_t k = 0; k < n_out; ++k) {
            const double f = (k < n_out / 2 ? double(k) : double(k) - double(n_out)) * out_rate / double(n_out);
            const double af = std::abs(f);
            taper[k] = af <= pass_hz ? 1.f : af >= out_rate / 2 ? 0.f
                     : float(0.5 * (1 + std::cos(pi * (af - pass_hz) / (out_rate / 2 - pass_hz))));
        }
    }
    double bin_hz() const { return rate / double(n_in); }
    // spectrum: forward FFT (n_in) of the block's input stretch, starting at input sample `first`.
    // Writes the view's block_size output samples to `out`.
    void view(const fftwf_complex* spectrum, long shift_bins, double first, fftwf_complex* work, cf* out) const {
        for (std::size_t k = 0; k < n_out; ++k) {
            const long ko = k < n_out / 2 ? long(k) : long(k) - long(n_out);
            long ki = shift_bins + ko;
            ki = ((ki % long(n_in)) + long(n_in)) % long(n_in);
            work[k][0] = spectrum[ki][0] * taper[k];
            work[k][1] = spectrum[ki][1] * taper[k];
        }
        fftwf_execute_dft(inverse, work, work);
        // Continuous mixing: the bin shift mixes from this stretch's first sample.
        const double shift_hz = double(shift_bins) * bin_hz();
        const std::complex<double> rot = std::polar(1.0 / double(n_in), -2 * pi * shift_hz * first / rate);
        for (std::size_t i = 0; i < st::block_size; ++i) {
            const std::complex<double> v(work[margin + i][0], work[margin + i][1]);
            out[i] = cf(v * rot);
        }
    }
};

const char* link_name(link_clock::Link l) { return link_clock::name(l); }

// Prototype (Phase 2, not app code): a CP-OFDM link's symbol rate and symbol-grid position in one
// capture, for any subcarrier spacing. The cyclic-prefix metric (lag = one useful symbol, window
// 0.9 of a prefix of 144/2048 of it, as link_clock uses for 15 kHz OcuSync) is periodic at the
// symbol rate. The rate is measured, not assumed: the strongest line of the metric's spectrum
// (summed over blocks) between 0.8 and 1.0 of the spacing, then refined by the coherent sum over
// all blocks at their capture times (resolution about 1 / capture length). The grid position is
// the phase of that sum at a given nominal rate (as link_clock's Lightbridge symbol phase).
struct GridRate { double rate_hz{}, coherence{}, phase_at_rate_s{}, phase_at_nominal_s{}, nominal_hz{}; };
std::optional<GridRate> symbol_grid(const std::vector<cf>& blocks, const std::vector<std::size_t>& starts, double fs,
                                    double spacing, double nominal) {
    constexpr std::size_t decim = 10;
    const std::size_t count = starts.size();
    const auto lag = std::size_t(std::lround(fs / spacing));
    const auto window = std::max<std::size_t>(8, std::size_t(std::lround(0.9 * 144.0 / 2048.0 * double(lag))));
    std::vector<std::vector<double>> m(count);
    double total = 0;
    for (std::size_t b = 0; b < count; ++b) {
        const cf* x = blocks.data() + b * st::block_size;
        const std::size_t products = st::block_size - lag;
        std::vector<std::complex<double>> c(products + 1);
        for (std::size_t k = 0; k < products; ++k)
            c[k + 1] = c[k] + std::complex<double>(x[k]) * std::conj(std::complex<double>(x[k + lag]));
        const std::size_t n = (products + 1 - window) / decim;
        m[b].resize(n);
        double mean = 0;
        for (std::size_t i = 0; i < n; ++i) {
            double sum = 0;
            for (std::size_t j = 0; j < decim; ++j) sum += std::abs(c[i * decim + j + window] - c[i * decim + j]);
            m[b][i] = sum / decim; mean += m[b][i];
        }
        mean /= double(n);
        for (auto& v : m[b]) { v -= mean; total += std::abs(v); }
    }
    const double fm = fs / decim;
    const auto coherent = [&](double r) {
        std::complex<double> sum{};
        for (std::size_t b = 0; b < count; ++b) {
            const double t0 = (double(starts[b]) + decim / 2.0) / fs;
            std::complex<double> ph = std::polar(1.0, -2 * pi * r * t0), step = std::polar(1.0, -2 * pi * r / fm);
            for (double v : m[b]) { sum += v * ph; ph *= step; }
        }
        return sum;
    };
    // Coarse: per-block power at candidate rates 0.8-1.0 x spacing in steps of fm / n (incoherent sum).
    const std::size_t n = m[0].size();
    const double coarse_step = fm / double(n) / 2;
    double best_r = 0, best_p = -1;
    for (double r = 0.8 * spacing; r <= spacing; r += coarse_step) {
        double p = 0;
        for (std::size_t b = 0; b < count; ++b) {
            std::complex<double> sum{}, ph{1, 0};
            const std::complex<double> step = std::polar(1.0, -2 * pi * r / fm);
            for (double v : m[b]) { sum += v * ph; ph *= step; }
            p += std::norm(sum);
        }
        if (p > best_p) { best_p = p; best_r = r; }
    }
    // Fine: coherent over the capture, +-coarse_step in steps of a tenth of 1 / span, twice.
    const double span = double(starts.back() + st::block_size - starts.front()) / fs;
    for (double width : {coarse_step, 2.0 / span}) {
        const double step = std::max(0.1 / span, width / 200);
        double r0 = best_r, bp = -1;
        for (double r = r0 - width; r <= r0 + width; r += step) {
            const double p = std::abs(coherent(r));
            if (p > bp) { bp = p; best_r = r; }
        }
    }
    if (!(total > 0)) return std::nullopt;
    GridRate g;
    g.rate_hz = best_r;
    const auto z = coherent(best_r);
    g.coherence = std::abs(z) / total;
    const auto wrap = [](double v, double p) { v = std::fmod(v, p); return v < 0 ? v + p : v; };
    g.phase_at_rate_s = wrap(-std::arg(z) / (2 * pi) / best_r, 1 / best_r);
    if (nominal > 0) {
        g.nominal_hz = nominal;
        g.phase_at_nominal_s = wrap(-std::arg(coherent(nominal)) / (2 * pi) / nominal, 1 / nominal);
    }
    return g;
}
}

int main(int argc, char** argv) try {
    std::string input = "-", format = "cf32", run = "rec";
    double rate = 0, centre = 0, time0 = 0, capture_s = 0.5, view_step = 5e6;
    std::vector<double> view_list;
    bool droneid = false, grid_proto = false;
    double nominal15 = 14000, nominal30 = 28000, dense_s = 0, trait_s = 0;
    std::uint64_t seq0 = 0;
    unsigned threads = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&]() -> std::string { if (i + 1 >= argc) throw std::runtime_error("missing value for " + a); return argv[++i]; };
        if (a == "--input") input = next();
        else if (a == "--format") format = next();
        else if (a == "--run") run = next();
        else if (a == "--rate") rate = std::stod(next());
        else if (a == "--centre") centre = std::stod(next());
        else if (a == "--time0") time0 = std::stod(next());
        else if (a == "--capture-s") capture_s = std::stod(next());
        else if (a == "--view-step") view_step = std::stod(next());
        else if (a == "--views") { std::stringstream ss(next()); std::string f; while (std::getline(ss, f, ',')) view_list.push_back(std::stod(f)); }
        else if (a == "--droneid") droneid = true;
        else if (a == "--symbol-grid") grid_proto = true;
        else if (a == "--dense-s") dense_s = std::stod(next());
        else if (a == "--traits-s") trait_s = std::stod(next());
        else if (a == "--nominal-15k") nominal15 = std::stod(next());
        else if (a == "--nominal-30k") nominal30 = std::stod(next());
        else if (a == "--seq0") seq0 = std::stoull(next());
        else if (a == "--threads") threads = unsigned(std::stoul(next()));
        else throw std::runtime_error("unknown option " + a);
    }
    const double ratio = rate / out_rate;
    const std::size_t decim = std::size_t(std::llround(ratio));
    if (!(rate > 0) || decim < 1 || std::abs(ratio - double(decim)) > 1e-9 || !(centre > 0))
        throw std::runtime_error("--rate must be a multiple of 20 MSps and --centre given");
    const auto iq = read_input(input, format);
    const std::size_t n = iq.size();
    const std::size_t n_out_total = n / decim;
    if (std::min<std::size_t>(std::size_t(capture_s * out_rate), n_out_total) < 4 * st::block_size)
        throw std::runtime_error("recording too short for 4 blocks");

    // Views: offsets from the recording centre, so the 20 MHz view stays inside the recorded band.
    std::vector<double> offsets;
    if (decim == 1) offsets.push_back(0);
    else if (!view_list.empty()) for (double f : view_list) offsets.push_back(f - centre);
    else {
        const double edge = rate / 2 - out_rate / 2;
        for (double o = -std::floor(edge / view_step) * view_step; o <= edge + 1; o += view_step) offsets.push_back(o);
    }
    Channelizer ch(decim, rate);
    std::vector<long> shifts;
    for (double o : offsets) shifts.push_back(std::lround(o / ch.bin_hz()));

    struct Line { double t; Json j; };
    std::vector<Line> lines;
    std::mutex lines_mutex;
    std::uint64_t seq = seq0;
    std::size_t captures_done = 0;
    // Views where the live-length pass found a timing group: the dense pass looks only there.
    std::vector<char> active(offsets.size(), 1), found(offsets.size(), 0);
    // Views with a timing group in the live-length pass, for the periodicity traits.
    struct TraitJob { std::size_t cs, v; std::string group; double t; };
    std::vector<TraitJob> trait_jobs;
    // One pass of captures of `cap_s` seconds back to back. kind "link": the live capture length,
    // lines for the replay; kind "dense": shorter captures for frame timing (Phase 2), with the
    // symbol-grid prototype, only views with a timing group.
    const auto pass = [&](double cap_s, const char* kind, bool proto) {
    const std::size_t capture_out = std::min<std::size_t>(std::size_t(cap_s * out_rate), n_out_total);
    if (capture_out < 4 * st::block_size) return;
    std::vector<std::size_t> capture_starts;  // in output samples
    for (std::size_t s = 0; s + capture_out <= n_out_total; s += capture_out) capture_starts.push_back(s);
    const std::size_t count = std::min(st::max_blocks, capture_out / st::block_size);
    if (std::string(kind) == "link") captures_done = capture_starts.size();
    for (std::size_t c = 0; c < capture_starts.size(); ++c) {
        const std::size_t cs = capture_starts[c];
        std::vector<std::size_t> starts(count);
        for (std::size_t b = 0; b < count; ++b) starts[b] = st::block_start(capture_out, b, count);
        // Blocks per view, end to end.
        std::vector<std::vector<cf>> views(offsets.size(), std::vector<cf>(count * st::block_size));
        std::atomic<std::size_t> next_block{0};
        const auto work_blocks = [&]() {
            auto* spectrum = fftwf_alloc_complex(ch.n_in);
            auto* work = fftwf_alloc_complex(ch.n_out);
            for (std::size_t b; (b = next_block++) < count;) {
                if (decim == 1) {  // already a 20 MSps capture: the samples as the live app gets them
                    std::copy_n(iq.data() + cs + starts[b], st::block_size, views[0].data() + b * st::block_size);
                    continue;
                }
                // Input stretch: block (in output samples cs + starts[b]) with the margin, times decim.
                const long first_out = long(cs + starts[b]) - long(margin);
                for (std::size_t k = 0; k < ch.n_in; ++k) {
                    const long src = first_out * long(decim) + long(k);
                    const cf v = src >= 0 && std::size_t(src) < n ? iq[std::size_t(src)] : cf{};
                    spectrum[k][0] = v.real(); spectrum[k][1] = v.imag();
                }
                fftwf_execute_dft(ch.forward, spectrum, spectrum);
                for (std::size_t v = 0; v < offsets.size(); ++v)
                    if (active[v])
                        ch.view(spectrum, shifts[v], double(first_out) * double(decim), work, views[v].data() + b * st::block_size);
            }
            fftwf_free(spectrum); fftwf_free(work);
        };
        {
            std::vector<std::thread> pool;
            for (unsigned t = 0; t < threads; ++t) pool.emplace_back(work_blocks);
            for (auto& t : pool) t.join();
        }
        std::atomic<std::size_t> next_view{0};
        const auto work_views = [&]() {
            st::Probe probe;
            for (std::size_t v; (v = next_view++) < offsets.size();) {
                if (!active[v]) continue;
                st::Result r;
                r.rate = out_rate;
                r.center = centre + double(shifts[v]) * ch.bin_hz();
                r.blocks = count;
                r.capture_samples = capture_out;
                if (!st::measure_blocks(probe, views[v].data(), r)) continue;
                if (std::string(kind) == "dense" && !r.timing_group) continue;
                if (r.timing_group) found[v] = 1;
                if (trait_s > 0 && r.timing_group && std::string(kind) == "link") {
                    std::lock_guard<std::mutex> lock(lines_mutex);
                    trait_jobs.push_back({cs, v, r.timing_group, time0 + double(cs) / out_rate});
                }
                const double t = time0 + double(cs) / out_rate;
                Json grids = Json::array();
                for (std::size_t i = 0; i < r.grid.size(); ++i)
                    if (const auto& g = r.grid[i])
                        grids.push_back({{"hypothesis", st::hypotheses[i].name}, {"spacing_hz", g->spacing_hz},
                                         {"hz", g->hz}, {"coherence", g->coherence}, {"blocks", g->blocks}});
                Json fr = Json::object();
                for (std::size_t i = 0; i < r.fractions.size(); ++i)
                    if (r.fractions[i] > 0) fr[st::hypotheses[i].name] = r.fractions[i];
                Json j{{"kind", kind}, {"schema", "rfmon.drone_offset.v1"}, {"run", run}, {"session", 1},
                       {"capture_s", cap_s}, {"center_hz", r.center}, {"sample_rate_hz", r.rate}, {"blocks", r.blocks},
                       {"group", r.timing_group ? Json(r.timing_group) : Json(nullptr)}, {"grid", grids},
                       {"device_ns", std::llround((t + 1) * 1e9)}, {"unix_ms", (t + 1) * 1e3}, {"t_s", t},
                       {"view_offset_hz", double(shifts[v]) * ch.bin_hz()}, {"fractions", fr}};
                if (const auto& lc = r.link_clock) {
                    j["link_clock"] = {{"link", link_name(lc->link)}, {"ppm", lc->ppm}, {"sigma_ppm", lc->sigma_ppm},
                                       {"blocks", lc->blocks}, {"usable", link_clock::usable(*lc)}};
                    if (lc->has_symbol_phase) j["link_clock"]["symbol_phase_s"] = lc->symbol_phase_s;
                }
                if (proto && r.timing_group) {
                    const std::string g = r.timing_group;
                    Json protos = Json::array();
                    for (double spacing : {15000.0, 30000.0}) {
                        if (g.find(spacing == 15000 ? "15kHz" : "30kHz") == std::string::npos) continue;
                        if (const auto gr = symbol_grid(views[v], starts, out_rate, spacing, spacing == 15000 ? nominal15 : nominal30))
                            protos.push_back({{"spacing_hz", spacing}, {"rate_hz", gr->rate_hz}, {"coherence", gr->coherence},
                                              {"phase_at_rate_s", gr->phase_at_rate_s}, {"nominal_hz", gr->nominal_hz},
                                              {"phase_at_nominal_s", gr->phase_at_nominal_s}});
                    }
                    if (!protos.empty()) j["symbol_grid"] = std::move(protos);
                }
                std::lock_guard<std::mutex> lock(lines_mutex);
                lines.push_back({t + 1e-9 * double(v), std::move(j)});
            }
        };
        {
            std::vector<std::thread> pool;
            for (unsigned t = 0; t < threads; ++t) pool.emplace_back(work_views);
            for (auto& t : pool) t.join();
        }
    }
    };
    pass(capture_s, "link", grid_proto);
    active = found;
    if (dense_s > 0) pass(dense_s, "dense", true);
    if (trait_s > 0 && !trait_jobs.empty()) {
        // One contiguous stretch of trait_s seconds per view capture with a timing group (the app's
        // own 0.5 s capture is contiguous, so this is what it could compute live too).
        const std::size_t len = std::min<std::size_t>(std::size_t(trait_s * out_rate), n_out_total);
        const std::size_t nfft = 1u << 18;  // periodicity spectrum of the 2 MHz metric: 7.6 Hz bins
        auto* tmp_a = fftwf_alloc_complex(nfft);
        const fftwf_plan pplan = fftwf_plan_dft_1d(int(nfft), tmp_a, tmp_a, FFTW_FORWARD, FFTW_ESTIMATE);
        fftwf_free(tmp_a);
        std::atomic<std::size_t> next{0};
        std::vector<std::thread> pool;
        for (unsigned th = 0; th < threads; ++th) pool.emplace_back([&]() {
            auto* spectrum = fftwf_alloc_complex(ch.n_in);
            auto* work = fftwf_alloc_complex(ch.n_out);
            auto* pbuf = fftwf_alloc_complex(nfft);
            std::vector<cf> out(st::block_size), x(len);
            for (std::size_t j; (j = next++) < trait_jobs.size();) {
                const auto& job = trait_jobs[j];
                if (job.cs + len > n_out_total) continue;
                // The view as one contiguous 20 MSps stretch.
                for (std::size_t b = 0; b * st::block_size < len; ++b) {
                    const std::size_t o = job.cs + b * st::block_size;
                    const std::size_t take = std::min(st::block_size, len - b * st::block_size);
                    if (decim == 1) { std::copy_n(iq.data() + o, take, x.data() + b * st::block_size); continue; }
                    const long first_out = long(o) - long(margin);
                    for (std::size_t k = 0; k < ch.n_in; ++k) {
                        const long src = first_out * long(decim) + long(k);
                        const cf val = src >= 0 && std::size_t(src) < n ? iq[std::size_t(src)] : cf{};
                        spectrum[k][0] = val.real(); spectrum[k][1] = val.imag();
                    }
                    fftwf_execute_dft(ch.forward, spectrum, spectrum);
                    ch.view(spectrum, shifts[job.v], double(first_out) * double(decim), work, out.data());
                    std::copy_n(out.data(), take, x.data() + b * st::block_size);
                }
                Json traits{{"kind", "traits"}, {"run", run}, {"t_s", job.t}, {"center_hz", centre + double(shifts[job.v]) * ch.bin_hz()},
                            {"group", job.group}, {"seconds", double(len) / out_rate}};
                // Lag of the group's useful symbol, window 0.9 of its prefix (link_clock's proportions).
                const bool lb = job.group == "32.8kHz";
                const double useful = lb ? st::hypotheses[st::lightbridge].seconds
                                    : job.group == "30kHz" ? st::hypotheses[st::lte_30k].seconds : st::hypotheses[st::lte_15k].seconds;
                const auto lag = std::size_t(std::lround(useful * out_rate));
                const auto window = std::max<std::size_t>(8, std::size_t(std::lround(0.9 * (lb ? 516.0 / 4096 : 144.0 / 2048) * double(lag))));
                constexpr std::size_t dec = 10;
                const auto series = [&](bool cp) {
                    std::vector<double> m;
                    if (cp) {
                        std::vector<std::complex<double>> c(len - lag + 1);
                        for (std::size_t k = 0; k + lag < len; ++k)
                            c[k + 1] = c[k] + std::complex<double>(x[k]) * std::conj(std::complex<double>(x[k + lag]));
                        const std::size_t cnt = (len - lag + 1 - window) / dec;
                        m.resize(cnt);
                        for (std::size_t i = 0; i < cnt; ++i) {
                            double sum = 0;
                            for (std::size_t q = 0; q < dec; ++q) sum += std::abs(c[i * dec + q + window] - c[i * dec + q]);
                            m[i] = sum / dec;
                        }
                    } else {
                        m.resize(len / dec);
                        for (std::size_t i = 0; i < m.size(); ++i) {
                            double sum = 0;
                            for (std::size_t q = 0; q < dec; ++q) sum += std::norm(x[i * dec + q]);
                            m[i] = sum / dec;
                        }
                    }
                    return m;
                };
                const double fm = out_rate / dec;
                for (const bool cp : {true, false}) {
                    auto m = series(cp);
                    double mean = 0;
                    for (double v : m) mean += v;
                    mean /= double(std::max<std::size_t>(m.size(), 1));
                    for (std::size_t k = 0; k < nfft; ++k) {
                        pbuf[k][0] = k < m.size() ? float(m[k] - mean) : 0.f;
                        pbuf[k][1] = 0.f;
                    }
                    fftwf_execute_dft(pplan, pbuf, pbuf);
                    std::vector<double> pw(nfft / 2);
                    for (std::size_t k = 0; k < nfft / 2; ++k) pw[k] = double(pbuf[k][0]) * pbuf[k][0] + double(pbuf[k][1]) * pbuf[k][1];
                    const double bin = fm / double(nfft);
                    // 50 Hz bands from 50 Hz to 30 kHz: log10 mean power, median of the bands removed.
                    std::vector<double> bands;
                    for (double f = 50; f < 30000; f += 50) {
                        const auto lo = std::size_t(f / bin), hi = std::size_t((f + 50) / bin);
                        double sum = 0;
                        for (std::size_t k = lo; k < hi && k < pw.size(); ++k) sum += pw[k];
                        bands.push_back(std::log10(sum / double(std::max<std::size_t>(hi - lo, 1)) + 1e-30));
                    }
                    auto sorted = bands;
                    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
                    const double med = sorted[sorted.size() / 2];
                    Json jb = Json::array();
                    for (double v : bands) jb.push_back(std::round((v - med) * 1000) / 1000);
                    // Strongest lines (local maxima over +-3 bins), in dB over the median band.
                    std::vector<std::pair<double, double>> peaks;
                    const auto lo_k = std::size_t(40 / bin), hi_k = std::min(pw.size() - 4, std::size_t(30000 / bin));
                    for (std::size_t k = lo_k; k < hi_k; ++k) {
                        bool top = true;
                        for (int d = -3; d <= 3 && top; ++d) if (d && pw[k + d] > pw[k]) top = false;
                        if (top) peaks.push_back({10 * (std::log10(pw[k] + 1e-30) - med), double(k) * bin});
                    }
                    std::sort(peaks.begin(), peaks.end(), [](auto& a, auto& b) { return a.first > b.first; });
                    Json jp = Json::array();
                    for (std::size_t k = 0; k < std::min<std::size_t>(16, peaks.size()); ++k)
                        jp.push_back({std::round(peaks[k].second * 10) / 10, std::round(peaks[k].first * 10) / 10});
                    traits[cp ? "cp_bands" : "env_bands"] = std::move(jb);
                    traits[cp ? "cp_peaks" : "env_peaks"] = std::move(jp);
                }
                std::lock_guard<std::mutex> lock(lines_mutex);
                lines.push_back({job.t + 2e-9, std::move(traits)});
            }
            fftwf_free(spectrum); fftwf_free(work); fftwf_free(pbuf);
        });
        for (auto& t : pool) t.join();
    }

#ifdef RFMON_CORPUS_CHECK_DRONEID
    if (droneid) {
        std::vector<double> channels;
        // Every documented channel whose 10 MHz lies inside the recorded band (less 0.5 MHz).
        const auto inside = [&](double f) { return std::abs(f - centre) <= rate / 2 - 5.5e6; };
        for (double f : drone::droneid::channels_24) if (inside(f)) channels.push_back(f);
        for (double f : drone::droneid::channels_58) if (inside(f)) channels.push_back(f);
        // Each channel as a continuous 20 MSps stream centred on it (as the live worker decodes a
        // 20 MSps capture), cut block by block by the same channelizer; a 20 MSps recording is
        // decoded as it is, as live.
        std::vector<std::vector<droneid::cf32>> streams(channels.size());
        std::vector<long> ch_shift(channels.size());
        if (decim > 1) {
            for (std::size_t c = 0; c < channels.size(); ++c) {
                ch_shift[c] = std::lround((channels[c] - centre) / ch.bin_hz());
                streams[c].assign(n_out_total, {});
            }
            const std::size_t nblocks = (n_out_total + st::block_size - 1) / st::block_size;
            std::atomic<std::size_t> next{0};
            std::vector<std::thread> pool;
            for (unsigned t = 0; t < threads; ++t) pool.emplace_back([&]() {
                auto* spectrum = fftwf_alloc_complex(ch.n_in);
                auto* work = fftwf_alloc_complex(ch.n_out);
                std::vector<cf> out(st::block_size);
                for (std::size_t b; (b = next++) < nblocks;) {
                    const long first_out = long(b * st::block_size) - long(margin);
                    for (std::size_t k = 0; k < ch.n_in; ++k) {
                        const long src = first_out * long(decim) + long(k);
                        const cf v = src >= 0 && std::size_t(src) < n ? iq[std::size_t(src)] : cf{};
                        spectrum[k][0] = v.real(); spectrum[k][1] = v.imag();
                    }
                    fftwf_execute_dft(ch.forward, spectrum, spectrum);
                    for (std::size_t c = 0; c < channels.size(); ++c) {
                        ch.view(spectrum, ch_shift[c], double(first_out) * double(decim), work, out.data());
                        const std::size_t len = std::min(st::block_size, n_out_total - b * st::block_size);
                        std::copy_n(out.data(), len, streams[c].data() + b * st::block_size);
                    }
                }
                fftwf_free(spectrum); fftwf_free(work);
            });
            for (auto& t : pool) t.join();
        }
        std::vector<std::thread> pool;
        for (std::size_t c = 0; c < channels.size(); ++c) pool.emplace_back([&, c]() {
            const double f = channels[c];
            droneid::dji::DecodeOptions o;
            o.sample_rate = decim > 1 ? out_rate : rate;
            // The stream's centre sits on the channel to within a bin; a 20 MSps recording is shifted.
            o.freq_offset = decim > 1 ? -(f - centre - double(ch_shift[c]) * ch.bin_hz()) : centre - f;
            const std::vector<droneid::cf32>& samples = decim > 1 ? streams[c] : iq;
            const double fs_in = o.sample_rate;
            for (const auto& burst : droneid::dji::decode_samples(samples, o)) {
                if (!burst.crc_ok) continue;
                droneid::dji::Telemetry tel;
                const bool parsed = droneid::dji::parse_frame(burst.payload, tel);
                const auto info = drone::droneid::classify(burst.payload, droneid::dji::inspect_packet(burst.payload).format);
                const double t = time0 + double(burst.sample_index) / fs_in;
                // The view nearest the channel stands for the capture it was decoded from.
                std::size_t best = 0;
                for (std::size_t v = 1; v < offsets.size(); ++v)
                    if (std::abs(centre + offsets[v] - f) < std::abs(centre + offsets[best] - f)) best = v;
                Json j{{"kind", "droneid"}, {"schema", "rfmon.drone_offset.v1"}, {"run", run}, {"session", 1},
                       {"capture", 0}, {"center_hz", centre + double(shifts[best]) * ch.bin_hz()},
                       {"serial", info.identity_readable ? info.serial : (parsed ? tel.serial : std::string())},
                       {"model", info.model}, {"readable", info.identity_readable}, {"encrypted", info.identity_encrypted},
                       {"offset_hz", burst.cfo_hz}, {"channel_hz", f}, {"offset_ppm", burst.cfo_hz / f * 1e6},
                       {"unix_ms", (t + 1) * 1e3}, {"t_s", t}, {"correlation", burst.corr}};
                std::lock_guard<std::mutex> lock(lines_mutex);
                lines.push_back({t, std::move(j)});
            }
        });
        for (auto& t : pool) t.join();
    }
#else
    if (droneid) throw std::runtime_error("built without the DJI decoder");
#endif
    std::stable_sort(lines.begin(), lines.end(), [](const Line& a, const Line& b) { return a.t < b.t; });
    for (auto& l : lines) {
        if (l.j["kind"] == "link") l.j["capture"] = ++seq;
        std::cout << l.j.dump(-1, ' ', false, Json::error_handler_t::replace) << '\n';
    }
    std::fprintf(stderr, "%s: %zu samples, %zu captures x %zu views, %zu lines\n", run.c_str(), n,
                 captures_done, offsets.size(), lines.size());
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "drone_corpus_check: %s\n", e.what());
    return 1;
}
