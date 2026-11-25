// Link clock (crystal ppm from symbol timing): synthetic OFDM links with a known clock error,
// seen through the timing monitor's 24 blocks of one 0.512 s capture at 20 MSps.
#include "drones/symbol_timing_monitor.hpp"

#include <cmath>
#include <iostream>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using namespace rfmon;
using Sample = std::complex<float>;
namespace st = rfmon::symbol_timing;

static void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

namespace {
constexpr double pi = 3.14159265358979323846;

// An OFDM transmitter whose clock runs (1 + ppm) fast, sampled by the receiver at fs. Symbols are
// QPSK on `used` subcarriers either side of DC, generated on demand (seeded by index) at the
// link's base rate, and read at fractional positions by linear interpolation.
struct Link {
    int n_fft;
    std::vector<int> prefixes;  // cycled per symbol
    int used;
    double base_rate, ppm, carrier;
    std::vector<int> on_off{};   // optional burst pattern: symbol s is on when on_off[s % size]
    bool hop{};                  // each block at a random frequency offset (no steady grid)
    double start = 12345.6;      // transmitter sample position at the capture's first sample
    std::map<long, std::vector<std::complex<double>>> cache;

    long period() const { long p = 0; for (int c : prefixes) p += n_fft + c; return p; }
    const std::vector<std::complex<double>>& symbol(long index) {
        auto it = cache.find(index);
        if (it != cache.end()) return it->second;
        if (cache.size() > 64) cache.erase(cache.begin());
        const int cp = prefixes[index % prefixes.size()];
        std::mt19937 rng(static_cast<unsigned>(index * 2654435761u + 7));
        std::vector<std::complex<double>> freq(n_fft), out(n_fft + cp);
        const bool on = on_off.empty() || on_off[index % on_off.size()];
        for (int k = 1; k <= used && on; ++k)
            for (int bin : {k, n_fft - k})
                freq[bin] = {rng() & 1 ? 1. : -1., rng() & 1 ? 1. : -1.};
        // Direct inverse DFT would be slow; use a radix-2 FFT (n_fft is a power of two).
        std::vector<std::complex<double>> a(freq);
        for (int i = 1, j = 0; i < n_fft; ++i) {
            int bit = n_fft >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(a[i], a[j]);
        }
        for (int len = 2; len <= n_fft; len <<= 1) {
            const auto w = std::polar(1., 2 * pi / len);
            for (int i = 0; i < n_fft; i += len) {
                std::complex<double> v = 1;
                for (int j = 0; j < len / 2; ++j, v *= w) {
                    const auto u = a[i + j], t = a[i + j + len / 2] * v;
                    a[i + j] = u + t;
                    a[i + j + len / 2] = u - t;
                }
            }
        }
        for (int i = 0; i < n_fft + cp; ++i) out[i] = a[(i - cp + n_fft) % n_fft] / std::sqrt(double(n_fft));
        return cache.emplace(index, std::move(out)).first->second;
    }
    // Base-rate sample at (fractional) transmitter sample position pos.
    std::complex<double> at(double pos) {
        const long whole = static_cast<long>(std::floor(pos));
        const double frac = pos - whole;
        return (1 - frac) * sample(whole) + frac * sample(whole + 1);
    }
    std::complex<double> sample(long n) {
        const long p = period(), cycle = n / p;
        long rem = n % p, index = cycle * long(prefixes.size());
        for (int cp : prefixes) {
            if (rem < n_fft + cp) break;
            rem -= n_fft + cp;
            ++index;
        }
        return symbol(index)[rem];
    }
};

// The monitor's 24 blocks of one capture of `capture` samples, received with noise at snr_db.
std::vector<Sample> blocks_of(Link& link, std::size_t capture, double fs, double snr_db, unsigned seed,
                              std::array<std::size_t, st::max_blocks>& starts, std::size_t& count) {
    count = std::min(st::max_blocks, capture / st::block_size);
    std::vector<Sample> out(count * st::block_size);
    std::mt19937 rng(seed);
    std::normal_distribution<double> noise(0, std::sqrt(.5 * std::pow(10, -snr_db / 10)));
    const double offset = link.start;  // transmitter time at the capture's first sample
    std::uniform_real_distribution<double> shift(-1e6, 1e6);
    for (std::size_t b = 0; b < count; ++b) {
        starts[b] = st::block_start(capture, b, count);
        const double hop_hz = link.hop ? shift(rng) : 0;
        for (std::size_t k = 0; k < st::block_size; ++k) {
            const double n = double(starts[b] + k);
            const double pos = offset + n / fs * (1 + link.ppm * 1e-6) * link.base_rate;
            const auto carrier = std::polar(1., 2 * pi * (link.carrier * link.ppm * 1e-6 + hop_hz) * n / fs);
            const auto v = link.at(pos) * carrier + std::complex<double>(noise(rng), noise(rng));
            out[b * st::block_size + k] = Sample(float(v.real() * .1), float(v.imag() * .1));
        }
    }
    return out;
}

constexpr std::size_t capture = 10240000;  // 0.512 s at 20 MSps, the app's dwell
constexpr double fs = 20e6;

std::optional<link_clock::Estimate> run(Link link, link_clock::Link kind, double snr, unsigned seed) {
    std::array<std::size_t, st::max_blocks> starts{};
    std::size_t count{};
    const auto iq = blocks_of(link, capture, fs, snr, seed, starts, count);
    return link_clock::measure(kind, iq.data(), st::block_size, starts.data(), count, fs);
}

void expect(const std::string& name, const std::optional<link_clock::Estimate>& e, double ppm, double tolerance) {
    require(e.has_value(), name + ": no estimate");
    std::cout << name << ": true " << ppm << " ppm, measured " << e->ppm << " +- " << e->sigma_ppm << '\n';
    require(std::abs(e->ppm - ppm) <= tolerance, name + ": ppm off by " + std::to_string(e->ppm - ppm));
    require(link_clock::usable(*e), name + ": not usable");
    require(e->blocks == st::max_blocks, name + ": blocks");
}

// Lightbridge: 68 symbols per 7 ms (313600 samples at 44.8 MSps).
const std::vector<int> lightbridge_even = [] { std::vector<int> v(52, 516); v.insert(v.end(), 16, 515); return v; }();
// Same frame, but one long prefix per frame: symbols drift up to about 6 us off a uniform grid
// within the frame, as the real link does. The harmonic method gives 100+ ppm errors on this.
const std::vector<int> lightbridge_uneven = [] { std::vector<int> v(67, 512); v.push_back(768); return v; }();
const std::vector<int> lte_slot{160, 144, 144, 144, 144, 144, 144};
}

int main() try {
    using L = link_clock::Link;
    expect("Lightbridge +3.2 ppm, 15 dB", run({4096, lightbridge_even, 457, 44.8e6, 3.2, 2.44e9}, L::lightbridge, 15, 1), 3.2, .3);
    expect("Lightbridge -4.0 ppm, uneven frame, 15 dB", run({4096, lightbridge_uneven, 457, 44.8e6, -4.0, 2.44e9}, L::lightbridge, 15, 2), -4.0, .3);
    expect("Lightbridge -1.7 ppm, uneven frame, 0 dB", run({4096, lightbridge_uneven, 457, 44.8e6, -1.7, 5.8e9}, L::lightbridge, 0, 3), -1.7, .6);
    expect("OcuSync 15k +2.1 ppm, LTE slots, 15 dB", run({2048, lte_slot, 300, 30.72e6, 2.1, 5.76e9}, L::ocusync_15k, 15, 4), 2.1, .1);
    expect("OcuSync 15k -0.6 ppm, LTE slots, 3 dB", run({2048, lte_slot, 300, 30.72e6, -0.6, 2.44e9}, L::ocusync_15k, 3, 5), -0.6, .15);
    {
        Link bursty{2048, lte_slot, 300, 30.72e6, 1.3, 2.44e9};
        bursty.on_off = {1, 1, 1, 1, 1, 1, 1, 1, 0, 0};
        expect("OcuSync 15k +1.3 ppm, bursts", run(bursty, L::ocusync_15k, 15, 6), 1.3, .15);
    }
    const std::vector<int> o4_slot{80, 72, 72, 72, 72, 72, 72};
    expect("OcuSync 30k -1.4 ppm, 15 dB", run({1024, o4_slot, 300, 30.72e6, -1.4, 5.8e9}, L::ocusync_30k, 15, 21), -1.4, .15);
    expect("OcuSync 30k +0.9 ppm, 3 dB", run({1024, o4_slot, 300, 30.72e6, 0.9, 2.44e9}, L::ocusync_30k, 3, 22), 0.9, .2);
    {
        // Symbol phase: delaying the transmitter by d moves its symbol grid by d after the capture
        // start (modulo the link's nominal symbol period), on every link.
        struct Case { const char* name; Link link; L kind; double tolerance_s; };
        const std::vector<Case> cases{
            {"Lightbridge", {4096, lightbridge_even, 457, 44.8e6, 1.0, 2.44e9}, L::lightbridge, 3e-6},
            {"OcuSync 15k", {2048, lte_slot, 300, 30.72e6, 1.0, 2.44e9}, L::ocusync_15k, 0.3e-6},
            {"OcuSync 30k", {1024, o4_slot, 300, 30.72e6, 1.0, 5.8e9}, L::ocusync_30k, 0.3e-6}};
        for (const auto& c : cases) {
            const double period = link_clock::symbol_period_s(c.kind);
            double first = 0;
            for (const double delay_s : {0.0, 13.3e-6, 31.9e-6}) {
                Link link = c.link;
                link.start = c.link.start - delay_s * link.base_rate;
                const auto e = run(link, c.kind, 15, 23);
                require(e && e->has_symbol_phase, std::string(c.name) + ": no symbol phase");
                if (delay_s == 0) { first = e->symbol_phase_s; continue; }
                const double moved = std::remainder(e->symbol_phase_s - first - delay_s, period);
                std::cout << c.name << " symbol phase, transmitter " << delay_s * 1e6 << " us later: moved "
                          << std::remainder(e->symbol_phase_s - first, period) * 1e6 << " us (off by " << moved * 1e6 << " us)\n";
                require(std::abs(moved) <= c.tolerance_s, std::string(c.name) + ": symbol phase did not follow the delay");
            }
        }
    }
    {
        // Noise only: no estimate, or one too loose to use.
        Link silent{4096, lightbridge_even, 0, 44.8e6, 0, 2.44e9};
        for (auto kind : {L::lightbridge, L::ocusync_15k, L::ocusync_30k}) {
            const auto e = run(silent, kind, 15, 7);
            if (e) std::cout << "noise only: " << e->ppm << " +- " << e->sigma_ppm << '\n';
            require(!e || !link_clock::usable(*e), "noise only gave a usable reading");
        }
    }
    {
        // Rates outside the probe's range and too few blocks give nothing.
        std::vector<Sample> iq(4 * st::block_size, Sample(1, 0));
        std::array<std::size_t, 4> starts{0, st::block_size, 2 * st::block_size, 3 * st::block_size};
        require(!link_clock::measure(L::lightbridge, iq.data(), st::block_size, starts.data(), 4, 10e6), "10 MSps accepted");
        require(!link_clock::measure(L::lightbridge, iq.data(), st::block_size, starts.data(), 3, 20e6), "3 blocks accepted");
    }
    {
        // measure_blocks fills link_clock for a Lightbridge capture, and only when the blocks
        // come from one capture of known length.
        Link lb{4096, lightbridge_uneven, 457, 44.8e6, 2.5, 2.44e9};
        std::array<std::size_t, st::max_blocks> starts{};
        std::size_t count{};
        const auto iq = blocks_of(lb, capture, fs, 15, 8, starts, count);
        st::Probe probe;
        st::Result r;
        r.rate = fs; r.center = 2.44e9; r.blocks = count;
        require(st::measure_blocks(probe, iq.data(), r), "measure_blocks failed");
        require(r.timing_group && std::string(r.timing_group) == "32.8kHz", "not grouped as Lightbridge");
        require(!r.link_clock, "link clock without capture length");
        r.capture_samples = capture;
        require(st::measure_blocks(probe, iq.data(), r), "measure_blocks failed");
        require(r.link_clock && r.link_clock->link == L::lightbridge, "no Lightbridge link clock");
        require(std::abs(r.link_clock->ppm - 2.5) <= .3, "measure_blocks ppm " + std::to_string(r.link_clock->ppm));
    }
    {
        // A 30 kHz group needs a steady 30 kHz grid: one O4-like link keeps it, the same link
        // at a different frequency in every block (no single transmitter) loses it.
        for (const bool hop : {false, true}) {
            Link o4{1024, o4_slot, 300, 30.72e6, 0.8, 5.8e9};
            o4.hop = hop;
            std::array<std::size_t, st::max_blocks> starts{};
            std::size_t count{};
            const auto iq = blocks_of(o4, capture, fs, 15, 9, starts, count);
            st::Probe probe;
            st::Result r;
            r.rate = fs; r.center = 5.8e9; r.blocks = count; r.capture_samples = capture;
            require(st::measure_blocks(probe, iq.data(), r), "measure_blocks failed (30 kHz)");
            const auto& g = r.grid[st::lte_30k];
            std::cout << "30 kHz link" << (hop ? ", hopping per block" : "") << ": fraction "
                      << r.fractions[st::lte_30k] << ", grid coherence " << (g ? g->coherence : -1)
                      << ", group " << (r.timing_group ? r.timing_group : "none") << '\n';
            require(r.fractions[st::lte_30k] >= st::group_threshold, "30 kHz lag not flagged");
            const bool has_30k = r.timing_group && std::string(r.timing_group).find("30kHz") != std::string::npos;
            require(has_30k == !hop, hop ? "30 kHz group kept without a steady grid" : "steady 30 kHz link lost its group");
            if (!hop) {
                // A "30kHz" capture gets the 30 kHz link clock, with its symbol phase.
                require(std::string(r.timing_group) == "30kHz", "steady 30 kHz link not in the 30kHz group");
                require(r.link_clock && r.link_clock->link == L::ocusync_30k, "no 30 kHz link clock");
                require(std::abs(r.link_clock->ppm - 0.8) <= .15 && r.link_clock->has_symbol_phase,
                        "30 kHz link clock " + std::to_string(r.link_clock->ppm));
            }
        }
    }
    {
        // An OcuSync capture with Lightbridge timing in some of its blocks (a Phantom on the same
        // band) is measured on its other blocks; with Lightbridge in most blocks, no clock.
        for (const int lb_symbols : {0, 300, 1700}) {
            Link ocu{2048, lte_slot, 300, 30.72e6, -0.7, 2.44e9};
            std::array<std::size_t, st::max_blocks> starts{};
            std::size_t count{};
            auto iq = blocks_of(ocu, capture, fs, 15, 10, starts, count);
            if (lb_symbols) {
                Link lb{4096, lightbridge_uneven, 457, 44.8e6, -4.0, 2.44e9};
                lb.on_off.assign(2000, 0);
                std::fill(lb.on_off.begin(), lb.on_off.begin() + lb_symbols, 1);
                const auto lb_iq = blocks_of(lb, capture, fs, 30, 11, starts, count);
                for (std::size_t i = 0; i < iq.size(); ++i) iq[i] += lb_iq[i];
            }
            st::Probe probe;
            st::Result r;
            r.rate = fs; r.center = 2.44e9; r.blocks = count; r.capture_samples = capture;
            require(st::measure_blocks(probe, iq.data(), r), "measure_blocks failed (mixed)");
            const double lb_fraction = std::max(r.fractions[st::lightbridge], r.fractions[st::lightbridge_third]);
            const auto all_blocks = link_clock::measure(L::ocusync_15k, iq.data(), st::block_size, starts.data(), count, fs);
            std::cout << "OcuSync -0.7 ppm, Lightbridge in " << lb_fraction * 100 << "% of blocks: group "
                      << (r.timing_group ? r.timing_group : "none") << ", clock "
                      << (r.link_clock ? std::to_string(r.link_clock->ppm) : "none") << " (all blocks: "
                      << (all_blocks ? std::to_string(all_blocks->ppm) : "none") << ")\n";
            if (lb_symbols == 1700) {
                require(lb_fraction > 0.6 && (!r.timing_group || std::string(r.timing_group) != "15kHz" || !r.link_clock),
                        "OcuSync clock from a capture with Lightbridge in most blocks");
                continue;
            }
            require(r.timing_group && std::string(r.timing_group) == "15kHz", "not grouped as 15 kHz");
            require((lb_symbols ? lb_fraction > 0 : lb_fraction == 0) && r.link_clock && std::abs(r.link_clock->ppm + 0.7) < 0.15,
                    "OcuSync clock wrong or missing with Lightbridge in some blocks");
        }
    }
    std::cout << "link clock tests passed\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
}
