// Offline evaluation of whole-frame RF features for PHY-level sender
// verification (RF fingerprint plan, phase A). Receive-only: reads IQ
// captures, never transmits.
//
// Every FCS-verified frame with whole-frame features (wifi_rf_features.hpp)
// is recorded by its transmitter address. Then:
//   1. Precision: per transmitter, each frame's deviation from that
//      transmitter's robust centre, by method / length / SNR, and how well
//      the fit's own sigma predicts it.
//   2. Verification trials. A frame is compared with its claimed sender's
//      OTHER frames within +/-window seconds (a contemporaneous reference,
//      so receiver and device drift cancel). Genuine: the frame's own
//      transmitter. Impostor: a frame from a different radio relabelled as
//      the claimed sender, which is what a forged frame looks like. MACs
//      differing only in the first octet (virtual BSSIDs of one radio) are
//      never paired.
//   3. Detection at fixed false-alarm rates, per feature and combined.
//
//   wifi_rf_auth_eval [--window S] [--csv FILE] CAPTURE.cf32 CENTRE_HZ [CHANNEL_HZ] [...]
//   Captures are 20 MSps complex float; CHANNEL_HZ defaults to CENTRE_HZ.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "config.hpp"
#include "security/wifi_mac_frame.hpp"
#include "wifi_burst_pipeline.hpp"

using namespace rfmon;

namespace {

struct Frame {
    int capture = 0;
    double t = 0, channel_hz = 0;
    std::string ta, kind, method;
    int symbols = 0;
    double cfo_preamble_ppm = NAN, cfo_ppm = 0, cfo_sigma_ppm = 0, cfo_early_ppm = NAN, settling_ppm = NAN, sfo_ppm = NAN, jitter = NAN, lo_leak = NAN, iq_image = NAN, snr = NAN, power = 0;
};

double median(std::vector<double> v) {
    if (v.empty()) return NAN;
    std::nth_element(v.begin(), v.begin() + long(v.size() / 2), v.end());
    return v[v.size() / 2];
}
double mad(const std::vector<double>& v) {
    const double m = median(v);
    std::vector<double> d;
    for (double x : v) d.push_back(std::abs(x - m));
    return 1.4826 * median(d);
}
double pct(std::vector<double> v, double p) {
    if (v.empty()) return NAN;
    std::sort(v.begin(), v.end());
    return v[size_t(p * double(v.size() - 1))];
}
bool same_radio(const std::string& a, const std::string& b) { return a.size() == 17 && b.size() == 17 && a.substr(3) == b.substr(3); }

std::string kind_of(const wifi_security::MacFrame& f) {
    using wifi_security::FrameType;
    if (f.fc.type == FrameType::Management) {
        switch (f.fc.subtype) {
            case 8: return "beacon";
            case 5: return "probe response";
            case 12: return "deauthentication";
            case 10: return "disassociation";
            case 13: return "action";
            default: return "management";
        }
    }
    return f.fc.type == FrameType::Data ? "data" : "control";
}

void collect(const std::string& path, double centre, double channel, int index, std::vector<Frame>& out) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return; }
    std::vector<std::complex<float>> x(size_t(in.tellg()) / 8);
    in.seekg(0);
    in.read(reinterpret_cast<char*>(x.data()), std::streamsize(x.size() * 8));
    const size_t chunk = 20000000;  // 1 s: the detector keeps at most a few thousand bursts per call
    for (size_t off = 0; off < x.size(); off += chunk) {
        const size_t n = std::min(chunk, x.size() - off);
        auto bursts = wifi::detect_bursts(x.data() + off, n, 20e6, DEFAULT_DETECTION_THRESHOLD_DB, WIFI_SECURITY_MAX_BURSTS_PER_CAPTURE);
        auto work = wifi::process_bursts(x.data() + off, n, bursts, 20e6, centre, channel, channel < 3e9);
        for (size_t i = 0; i < work.size(); ++i) {
            const auto& w = work[i];
            const std::vector<uint8_t>* mpdu = nullptr;
            const wifi::FrameRfFeatures* rf = nullptr;
            double preamble_hz = NAN;
            if (w.ofdm && w.ofdm->fcs_valid && w.ofdm->rf) { mpdu = &w.ofdm->mpdu; rf = &*w.ofdm->rf; preamble_hz = w.ofdm->cfo_hz; }
            if (w.dsss.result && w.dsss.result->fcs_valid && w.dsss.result->rf) { mpdu = &w.dsss.result->mpdu; rf = &*w.dsss.result->rf; }
            if (!mpdu) continue;
            const auto f = wifi_security::parse_mac_frame(mpdu->data(), mpdu->size());
            if (!f.transmitter) continue;
            Frame r;
            r.capture = index;
            r.t = double(off + bursts[i].start) / 20e6;
            r.channel_hz = channel;
            r.ta = wifi_security::format_mac(*f.transmitter);
            r.kind = kind_of(f);
            r.method = rf->method;
            r.symbols = rf->symbols;
            r.cfo_preamble_ppm = preamble_hz / channel * 1e6;  // the older preamble-only estimate (OFDM)
            r.cfo_ppm = rf->cfo_hz / channel * 1e6;
            r.cfo_sigma_ppm = rf->cfo_sigma_hz / channel * 1e6;
            r.cfo_early_ppm = std::isfinite(rf->cfo_early_hz) ? rf->cfo_early_hz / channel * 1e6 : r.cfo_ppm;  // DSSS: whole frame
            r.settling_ppm = rf->settling_hz / channel * 1e6;
            r.sfo_ppm = rf->sfo_ppm;
            r.jitter = rf->phase_jitter_deg;
            r.lo_leak = rf->lo_leak_dbc;
            r.iq_image = rf->iq_image_db;
            r.snr = rf->snr_db;
            r.power = w.cls.power_db;
            out.push_back(r);
        }
    }
}

struct Feature { const char* name; double Frame::*field; };
constexpr Feature kFeatures[] = {{"CFO, early (ppm)", &Frame::cfo_early_ppm}, {"CFO, preamble only (ppm)", &Frame::cfo_preamble_ppm}, {"CFO, whole frame (ppm)", &Frame::cfo_ppm}, {"SFO (ppm)", &Frame::sfo_ppm}, {"phase jitter (deg)", &Frame::jitter},
                                 {"LO leakage (dBc)", &Frame::lo_leak}, {"I/Q image (dB)", &Frame::iq_image}, {"power (dB)", &Frame::power}, {"frequency settling (ppm)", &Frame::settling_ppm}};

// Distance of one frame from a reference set of the claimed sender's frames,
// in units of the reference's own spread (robust), per feature.
std::optional<double> score(const Frame& f, const std::vector<const Frame*>& ref, double Frame::*field, double floor) {
    std::vector<double> v;
    for (const auto* r : ref) if (std::isfinite(r->*field)) v.push_back(r->*field);
    if (v.size() < 3 || !std::isfinite(f.*field)) return std::nullopt;
    const double centre = median(v), spread = std::max(mad(v), floor);
    return std::abs(f.*field - centre) / spread;
}

}  // namespace

int main(int argc, char** argv) {
    double window = 5;
    std::string csv;
    std::vector<Frame> frames;
    int index = 0;
    for (int a = 1; a < argc; ++a) {
        const std::string s = argv[a];
        if (s == "--window" && a + 1 < argc) { window = std::atof(argv[++a]); continue; }
        if (s == "--csv" && a + 1 < argc) { csv = argv[++a]; continue; }
        if (a + 1 >= argc) { std::fprintf(stderr, "usage: %s [--window S] [--csv FILE] CAPTURE CENTRE_HZ [CHANNEL_HZ] ...\n", argv[0]); return 2; }
        const double centre = std::atof(argv[++a]);
        double channel = centre;
        if (a + 1 < argc && std::atof(argv[a + 1]) > 1e9) channel = std::atof(argv[++a]);
        collect(s, centre, channel, index++, frames);
    }
    std::printf("Verified frames with whole-frame features: %zu from %d capture(s)\n", frames.size(), index);
    if (!csv.empty()) {
        std::ofstream out(csv);
        out << "capture,t,channel_hz,ta,kind,method,symbols,cfo_ppm,cfo_sigma_ppm,cfo_early_ppm,settling_ppm,sfo_ppm,jitter_deg,lo_leak_dbc,iq_image_db,snr_db,power_db\n";
        for (const auto& f : frames)
            out << f.capture << ',' << f.t << ',' << f.channel_hz << ',' << f.ta << ',' << f.kind << ',' << f.method << ',' << f.symbols << ','
                << f.cfo_ppm << ',' << f.cfo_sigma_ppm << ',' << f.cfo_early_ppm << ',' << f.settling_ppm << ',' << f.sfo_ppm << ',' << f.jitter << ',' << f.lo_leak << ',' << f.iq_image << ','
                << f.snr << ',' << f.power << '\n';
    }
    // Group by (capture, transmitter).
    std::map<std::pair<int, std::string>, std::vector<const Frame*>> by;
    for (const auto& f : frames) by[{f.capture, f.ta}].push_back(&f);

    // 1. Precision: deviation of each frame's CFO from its transmitter's median.
    std::printf("\n1. Per-frame early-CFO precision (deviation from the transmitter's median in the same capture)\n");
    struct Bucket { const char* name; std::vector<double> dev, z; };
    std::vector<Bucket> buckets = {{"DSSS", {}, {}}, {"OFDM < 20 symbols", {}, {}}, {"OFDM 20-99 symbols", {}, {}}, {"OFDM >= 100 symbols", {}, {}},
                                   {"OFDM >= 20 symbols, SNR >= 15 dB", {}, {}}};
    std::map<std::string, std::vector<double>> by_kind;
    for (const auto& [key, v] : by) {
        if (v.size() < 5) continue;
        std::vector<double> c;
        for (const auto* f : v) c.push_back(f->cfo_early_ppm);
        const double m = median(c);
        for (const auto* f : v) {
            const double d = f->cfo_early_ppm - m, z = f->cfo_sigma_ppm > 0 ? d / f->cfo_sigma_ppm : NAN;
            auto add = [&](size_t b) { buckets[b].dev.push_back(std::abs(d)); if (std::isfinite(z)) buckets[b].z.push_back(std::abs(z)); };
            if (f->method.rfind("DSSS", 0) == 0) add(0);
            else { add(f->symbols < 20 ? 1 : f->symbols < 100 ? 2 : 3); if (f->symbols >= 20 && f->snr >= 15) add(4); }
            by_kind[f->kind].push_back(std::abs(d));
        }
    }
    std::printf("   %-34s %7s %12s %12s %12s %16s\n", "frames", "count", "|dev| p50", "|dev| p90", "|dev| p99", "|dev|/sigma p90");
    for (const auto& b : buckets)
        std::printf("   %-34s %7zu %9.4f ppm %9.4f ppm %9.4f ppm %16.2f\n", b.name, b.dev.size(), pct(b.dev, .5), pct(b.dev, .9), pct(b.dev, .99), pct(b.z, .9));
    for (const auto& [k, d] : by_kind) std::printf("   by frame type: %-18s %7zu  |dev| p50 %.4f ppm  p90 %.4f ppm\n", k.c_str(), d.size(), pct(d, .5), pct(d, .9));

    // 2. Verification trials against contemporaneous references.
    std::printf("\n2. Verification trials (reference: claimed sender's other frames within +/-%.0f s, at least 3)\n", window);
    const size_t nf = std::size(kFeatures);
    std::vector<std::vector<double>> genuine(nf + 2), impostor(nf + 2);  // + early CFO + power, + class-matched early CFO
    // Floors keep a perfectly stable transmitter from making every tiny
    // difference look huge; they are the smallest spreads worth resolving.
    const double floors[] = {0.01, 0.01, 0.01, 0.05, 0.2, 0.5, 0.5, 1.0, 0.01};
    // Short (< 20 symbols) and long frames carry a small device-specific CFO
    // offset (see the doc), so the class-matched variant compares like with like.
    auto is_short = [](const Frame* f) { return f->method.rfind("OFDM", 0) == 0 && f->symbols < 20; };
    auto reference = [&](int capture, const std::string& ta, double t, const Frame* exclude, const Frame* match = nullptr) {
        std::vector<const Frame*> ref;
        const auto it = by.find({capture, ta});
        if (it == by.end()) return ref;
        for (const auto* r : it->second)
            if (r != exclude && std::abs(r->t - t) <= window && (!match || is_short(r) == is_short(match))) ref.push_back(r);
        return ref;
    };
    std::vector<std::pair<int, std::string>> keys;
    for (const auto& [k, v] : by) if (v.size() >= 4) keys.push_back(k);
    for (const auto& [key, v] : by) {
        for (const auto* f : v) {
            // Genuine: against its own transmitter.
            const auto own = reference(key.first, key.second, f->t, f);
            // Impostor: the same frame claiming every other radio heard in this capture.
            std::vector<std::vector<const Frame*>> others;
            for (const auto& k : keys)
                if (k.first == key.first && k.second != key.second && !same_radio(k.second, key.second)) {
                    auto r = reference(k.first, k.second, f->t, nullptr);
                    if (r.size() >= 3) others.push_back(std::move(r));
                }
            auto trial = [&](const std::vector<const Frame*>& ref, std::vector<std::vector<double>>& out) {
                double combined = 0;
                bool any = false;
                for (size_t i = 0; i < nf; ++i) {
                    const auto s = score(*f, ref, kFeatures[i].field, floors[i]);
                    if (!s) continue;
                    out[i].push_back(*s);
                    if (i == 0 || i == 7) { combined += (*s) * (*s); any = true; }  // combined: early CFO and power
                }
                if (any) out[nf].push_back(std::sqrt(combined));
            };
            if (own.size() >= 3) trial(own, genuine);
            for (const auto& r : others) trial(r, impostor);
            // Class-matched early CFO: references of the same length class only.
            const auto own_m = reference(key.first, key.second, f->t, f, f);
            if (const auto sc = score(*f, own_m, &Frame::cfo_early_ppm, floors[0])) genuine[nf + 1].push_back(*sc);
            for (const auto& k : keys)
                if (k.first == key.first && k.second != key.second && !same_radio(k.second, key.second))
                    if (const auto sc = score(*f, reference(k.first, k.second, f->t, nullptr, f), &Frame::cfo_early_ppm, floors[0]))
                        impostor[nf + 1].push_back(*sc);
        }
    }
    // 3. Detection at fixed false-alarm rates: the threshold is the genuine
    // score quantile; detection is the share of impostors above it.
    std::printf("   %-28s %9s %9s %14s %14s %14s\n", "feature", "genuine", "impostor", "det @ FA 10%", "det @ FA 1%", "det @ FA 0.1%");
    auto row_name = [&](size_t i) { return i < nf ? kFeatures[i].name : i == nf ? "early CFO + power" : "early CFO, same length class"; };
    for (size_t i = 0; i <= nf + 1; ++i) {
        const auto& g = genuine[i];
        const auto& im = impostor[i];
        if (g.size() < 20 || im.empty()) { std::printf("   %-28s %9zu %9zu   (too few trials)\n", row_name(i), g.size(), im.size()); continue; }
        auto det = [&](double fa) {
            const double thr = pct(g, 1 - fa);
            return 100.0 * double(std::count_if(im.begin(), im.end(), [&](double s) { return s > thr; })) / double(im.size());
        };
        std::printf("   %-28s %9zu %9zu %13.1f%% %13.1f%% %13.1f%%\n", row_name(i), g.size(), im.size(),
                    det(.1), det(.01), det(.001));
    }
    std::printf("\nScores are |frame - reference median| / reference spread (robust). FA = false-alarm rate on genuine frames.\n");
    return 0;
}
