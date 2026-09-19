// See bench_tx.hpp for the why. This file is the how: two fixed, known
// waveforms built bit-by-bit to match exactly what wifi_dsss_rx.cpp and
// wifi_fingerprint.cpp's OFDM path expect to receive - not approximations.
//
// DSSS: a real 802.11b 1Mbps long-PLCP frame (128-bit SYNC, SFD 0xF3A0,
// 6-byte PLCP header with a real CRC-16, a minimal beacon MPDU with a
// real FCS-32), scrambled with the same self-synchronising x^7+x^4+1
// scrambler wifi_frame.cpp's descramble_bits() undoes, DBPSK-encoded,
// Barker-11 spread. Every bit-order/byte-order convention here is the
// MIRROR of decode_dsss_burst()'s own - see that file for the
// (hard-won, real-hardware-confirmed) reasoning behind each one; this
// retains none of that reasoning, only its inverse.
//
// OFDM: only the L-STF/L-LTF preamble is built to spec (L-LTF is a
// byte-for-byte reuse of wifi_fingerprint.cpp's own kLltfFreq table,
// duplicated per this project's established convention for validated
// constants - see that file's header). Nothing after the preamble is a
// decodable payload: this project's OFDM RX path never attempts a MAC
// decode (see wifi_fingerprint.hpp's scope note), so the "data" symbols
// here are just more full-bandwidth filler to give the burst a
// realistic duration and occupied bandwidth, not real content.
#include "bench_tx.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>

#include <uhd/types/tune_request.hpp>

#include "config.hpp"
#include "wifi_frame.hpp"

namespace rfmon::bench {

namespace {

// --- shared bit-level helpers ---------------------------------------

// The self-synchronising scrambler's CAUSAL (encoder) direction:
// wifi_frame.cpp's descramble_bits() computes x[i] = y[i]^y[i-4]^y[i-7]
// from an already-complete array y (the received/scrambled stream) -
// that only works in the decode direction, since it needs y's own
// future-relative-to-x values already in hand. Scrambling has to build
// y progressively instead: y[i] = x[i]^y[i-4]^y[i-7], using y's own
// PAST (already-computed) outputs. Same taps, same polynomial,
// necessarily a different (recursive) implementation.
std::vector<uint8_t> scramble_bits(const std::vector<uint8_t>& x) {
    std::vector<uint8_t> y(x.size());
    for (size_t i = 0; i < x.size(); ++i) {
        uint8_t t4 = (i >= 4) ? y[i - 4] : 0;
        uint8_t t7 = (i >= 7) ? y[i - 7] : 0;
        y[i] = uint8_t((x[i] ^ t4 ^ t7) & 1u);
    }
    return y;
}

// Inverse of wifi_frame.cpp's bits_to_bytes_lsb_first(): bit position b
// in the output array is bit b of the byte (LSB first), matching
// exactly what that function assumes when packing back the other way.
void push_byte_lsb_first(std::vector<uint8_t>& bits, uint8_t byte) {
    for (int b = 0; b < 8; ++b) bits.push_back(uint8_t((byte >> b) & 1u));
}

// Duplicated from wifi_frame.cpp's own anonymous-namespace helper of
// the same name (internal linkage there, so not reachable from here) -
// see parse_plcp_header()'s comment for why the CRC field's two bytes
// need this and no other field does.
uint8_t reverse_bits8(uint8_t b) {
    uint8_t r = 0;
    for (int i = 0; i < 8; ++i) {
        if (b & (1u << i)) r |= uint8_t(1u << (7 - i));
    }
    return r;
}

// 802.11b long-PLCP SFD - same value wifi_dsss_rx.cpp's kLongSfd
// searches for, duplicated (small, standard, checkable constant).
constexpr uint16_t kSfd = 0xF3A0;

// --- DSSS frame construction ----------------------------------------

// A minimal, valid 802.11 beacon MPDU: a real management-frame header,
// a fixed test BSSID (locally-administered, so it can never collide
// with a real vendor OUI), a fixed SSID, a DS Parameter Set IE, and a
// real FCS-32 - decode_dsss_burst()/parse_beacon() will accept this
// exactly like a real captured beacon, since the FCS is what makes
// either one "real" (see wifi_frame.hpp's own header on that point).
std::vector<uint8_t> build_test_beacon_mpdu() {
    std::vector<uint8_t> f;
    f.reserve(64);

    // MAC header (24 bytes): FC(2) Dur(2) A1(6) A2(6) A3(6) Seq(2).
    f.push_back(0x80);  // FC: type=Management(00), subtype=Beacon(1000)
    f.push_back(0x00);
    f.push_back(0x00);
    f.push_back(0x00);  // Duration
    for (int i = 0; i < 6; ++i) f.push_back(0xFF);  // Addr1: broadcast
    const uint8_t test_mac[6] = {0x02, 0x00, 0x00, 0x54, 0x45, 0x53};  // locally-administered
    for (int i = 0; i < 6; ++i) f.push_back(test_mac[i]);  // Addr2
    for (int i = 0; i < 6; ++i) f.push_back(test_mac[i]);  // Addr3 (BSSID)
    f.push_back(0x00);
    f.push_back(0x00);  // Seq/Frag

    // Fixed params (12 bytes): Timestamp(8) BeaconInterval(2) Capability(2).
    for (int i = 0; i < 8; ++i) f.push_back(0x00);
    f.push_back(0x64);
    f.push_back(0x00);  // Beacon interval = 100 TU
    f.push_back(0x01);
    f.push_back(0x00);  // Capability: ESS, no privacy bit -> "Open"

    // SSID IE.
    static const char kSsid[] = "BENCH-TEST";
    const size_t ssid_len = sizeof(kSsid) - 1;
    f.push_back(0x00);
    f.push_back(uint8_t(ssid_len));
    for (size_t i = 0; i < ssid_len; ++i) f.push_back(uint8_t(kSsid[i]));

    // DS Parameter Set IE - channel value here is purely descriptive
    // metadata inside the frame body, not tied to the actual TX
    // frequency (which the GUI's own frequency field controls).
    f.push_back(0x03);
    f.push_back(0x01);
    f.push_back(0x06);

    uint32_t fcs = wifi::fcs32(f.data(), f.size());
    f.push_back(uint8_t(fcs & 0xFF));
    f.push_back(uint8_t((fcs >> 8) & 0xFF));
    f.push_back(uint8_t((fcs >> 16) & 0xFF));
    f.push_back(uint8_t((fcs >> 24) & 0xFF));
    return f;
}

std::vector<uint8_t> build_plcp_header_bytes(uint16_t length_us) {
    std::vector<uint8_t> h(6, 0);
    h[0] = 0x0A;  // SIGNAL: 1 Mbps (10 * 100kbit/s)
    h[1] = 0x00;  // SERVICE
    h[2] = uint8_t(length_us & 0xFF);
    h[3] = uint8_t((length_us >> 8) & 0xFF);
    // plcp_crc16() computes the CRC exactly as a real receiver checks
    // it (wifi_frame.hpp's own already-spec-validated implementation -
    // reused directly, not reimplemented). The two CRC bytes then need
    // the SAME per-octet bit reversal parse_plcp_header() undoes on
    // receive - see reverse_bits8()'s comment above.
    uint16_t crc = wifi::plcp_crc16(h.data(), 4);
    h[4] = reverse_bits8(uint8_t((crc >> 8) & 0xFF));
    h[5] = reverse_bits8(uint8_t(crc & 0xFF));
    return h;
}

// The full pre-scramble bit stream: 128-bit SYNC (all ones - see
// wifi_dsss_rx.cpp's own comment on why that's what it descrambles to)
// + SFD (MSB-first, matching bits_to_u16_msb()'s read convention) +
// PLCP header (LSB-first per byte) + MPDU (LSB-first per byte).
std::vector<uint8_t> build_dsss_plaintext_bits() {
    std::vector<uint8_t> mpdu = build_test_beacon_mpdu();
    uint16_t length_us = uint16_t(mpdu.size() * 8);  // 1 bit == 1us at 1Mbps
    std::vector<uint8_t> hdr = build_plcp_header_bytes(length_us);

    std::vector<uint8_t> x;
    x.reserve(128 + 16 + hdr.size() * 8 + mpdu.size() * 8);
    x.insert(x.end(), 128, uint8_t(1));
    for (int i = 0; i < 16; ++i) x.push_back(uint8_t((kSfd >> (15 - i)) & 1u));
    for (uint8_t b : hdr) push_byte_lsb_first(x, b);
    for (uint8_t b : mpdu) push_byte_lsb_first(x, b);
    return x;
}

// Linear-interpolating resample - same algorithm as
// wifi_dsss_rx.cpp's own resample_to()/wifi_phy.cpp's resample_linear(),
// duplicated per this project's convention rather than exported for one
// new caller.
std::vector<std::complex<float>> resample_linear(const std::vector<std::complex<float>>& in,
                                                    double in_rate, double out_rate) {
    if (in.empty() || in_rate <= 0.0 || out_rate <= 0.0) return in;
    double ratio = in_rate / out_rate;
    size_t n_out = size_t(double(in.size()) / ratio);
    std::vector<std::complex<float>> out(n_out);
    for (size_t i = 0; i < n_out; ++i) {
        double pos = double(i) * ratio;
        size_t i0 = size_t(pos);
        if (i0 + 1 >= in.size()) {
            out[i] = in.back();
            continue;
        }
        float frac = float(pos - double(i0));
        out[i] = in[i0] * (1.0f - frac) + in[i0 + 1] * frac;
    }
    return out;
}

}  // namespace

std::vector<std::complex<float>> build_dsss_test_waveform(double sample_rate_hz) {
    std::vector<uint8_t> plaintext = build_dsss_plaintext_bits();
    std::vector<uint8_t> scrambled = scramble_bits(plaintext);

    // DBPSK: 802.11b encodes bit=1 as a 180-degree flip from the
    // previous symbol, bit=0 as no change (see decode_dsss_burst()'s
    // own comment on the differential decision this must invert).
    // sym[0] is an arbitrary reference phase - the differential
    // decoder needs no absolute phase, only the transition sequence.
    size_t n_symbols = scrambled.size() + 1;
    std::vector<int> sym(n_symbols);
    sym[0] = 1;
    for (size_t i = 0; i < scrambled.size(); ++i) sym[i + 1] = sym[i] * (scrambled[i] ? -1 : 1);

    constexpr int kBarker[11] = {+1, -1, +1, +1, -1, +1, +1, +1, -1, -1, -1};
    constexpr int kSamplesPerChip = 2;   // matches wifi_dsss_rx.cpp's own chip grid exactly
    constexpr double kChipGridRateHz = 11e6 * kSamplesPerChip;  // 22 Msps
    constexpr float kAmplitude = 0.7f;

    std::vector<std::complex<float>> wave22;
    wave22.reserve(n_symbols * 11 * size_t(kSamplesPerChip));
    for (int s : sym) {
        for (int c = 0; c < 11; ++c) {
            float v = kAmplitude * float(s * kBarker[c]);
            for (int r = 0; r < kSamplesPerChip; ++r) wave22.emplace_back(v, 0.0f);
        }
    }

    std::vector<std::complex<float>> wave =
        (std::abs(sample_rate_hz - kChipGridRateHz) < 1.0)
            ? wave22
            : resample_linear(wave22, kChipGridRateHz, sample_rate_hz);

    // Silence padding on both ends - detect_bursts() finds edges by
    // energy rising/falling against a noise floor, and a burst that
    // starts the very first sample of a capture never gets one.
    size_t pad = size_t(std::lround(sample_rate_hz * 20e-6));
    std::vector<std::complex<float>> out(pad * 2 + wave.size(), std::complex<float>(0.0f, 0.0f));
    std::copy(wave.begin(), wave.end(), out.begin() + long(pad));
    return out;
}

namespace {

// --- OFDM preamble construction --------------------------------------

// Byte-for-byte the same 802.11a/g L-LTF frequency-domain training
// sequence as wifi_fingerprint.cpp's kLltfFreq - duplicated, not
// shared, per this project's established convention (see that file's
// header) for validated math consumed by a new caller. This copy is
// load-bearing: wifi_fingerprint.cpp fits its widely-linear IQ-
// imbalance model against exactly this reference, so a mismatch here
// wouldn't just fail to decode, it would silently corrupt IRR/IQ eps/
// IQ phi/DC on every loopback packet without any obvious symptom.
constexpr int kLltfFreq[53] = {
    1,  1,  -1, -1, 1,  1,  -1, 1,  -1, 1,  1,  1,  1,  1,  1,  -1, -1, 1, 1,
    -1, 1,  -1, 1,  1,  1,  1,  0,  1,  -1, -1, 1,  1,  -1, 1,  -1, 1,  -1, -1,
    -1, -1, -1, 1,  1,  -1, -1, 1,  -1, 1,  -1, 1,  1,  1,  1,
};
constexpr double kSubcarrierSpacingHz = 20e6 / 64.0;  // 312.5kHz, fixed regardless of TX sample rate

std::complex<double> ltf_reference(double t_s) {
    std::complex<double> sum(0.0, 0.0);
    for (int p = 0; p < 53; ++p) {
        if (kLltfFreq[p] == 0) continue;
        double k = double(p - 26);
        double phase = 2.0 * M_PI * k * kSubcarrierSpacingHz * t_s;
        sum += double(kLltfFreq[p]) * std::complex<double>(std::cos(phase), std::sin(phase));
    }
    return sum / std::sqrt(52.0);
}

// The standard 802.11a/g L-STF frequency-domain sequence (12 active
// subcarriers, spaced 4 apart, scaled by sqrt(13/6) - IEEE 802.11-2016
// Eq 17-25). UNLIKE kLltfFreq above, nothing on the RX side fits
// against this table's specific values - classify_modulation()'s
// Schmidl-Cox correlator only needs SOME comb of nonzero tones spaced
// every 4th subcarrier (that spacing alone is what produces the 0.8us
// time-domain periodicity it locks onto), so an error in one tone's
// sign here would cost nothing measurable in this bench - reproduced
// faithfully anyway since it costs nothing extra to do so.
struct StfTone { int k; std::complex<double> v; };
const std::vector<StfTone>& stf_tones() {
    static const double scale = std::sqrt(13.0 / 6.0);
    static const std::complex<double> p(scale, scale), n(-scale, -scale);
    static const std::vector<StfTone> tones = {
        {-24, p}, {-20, n}, {-16, p}, {-12, n}, {-8, n}, {-4, p},
        {4, n},   {8, n},   {12, p},  {16, p},  {20, p}, {24, p},
    };
    return tones;
}

std::complex<double> stf_reference(double t_s) {
    std::complex<double> sum(0.0, 0.0);
    for (const auto& tone : stf_tones()) {
        double phase = 2.0 * M_PI * double(tone.k) * kSubcarrierSpacingHz * t_s;
        sum += tone.v * std::complex<double>(std::cos(phase), std::sin(phase));
    }
    return sum / std::sqrt(12.0);
}

// Appends one OFDM symbol span: `gi_s` seconds of cyclic prefix (the
// L-LTF reference's own tail, evaluated at negative time - exploiting
// that a sum of pure subcarrier tones is periodic, so this needs no
// separate "copy the tail" step) followed by `sym_s` seconds of the
// reference itself starting at t=0.
void append_symbol_span(std::vector<std::complex<float>>& out, double sample_rate_hz, double amp,
                         double gi_s, double sym_s) {
    size_t n_gi = size_t(std::lround(gi_s * sample_rate_hz));
    size_t n_sym = size_t(std::lround(sym_s * sample_rate_hz));
    for (size_t i = 0; i < n_gi; ++i) {
        double t = -gi_s + double(i) / sample_rate_hz;
        auto v = ltf_reference(t) * amp;
        out.emplace_back(float(v.real()), float(v.imag()));
    }
    for (size_t i = 0; i < n_sym; ++i) {
        double t = double(i) / sample_rate_hz;
        auto v = ltf_reference(t) * amp;
        out.emplace_back(float(v.real()), float(v.imag()));
    }
}

}  // namespace

std::vector<std::complex<float>> build_ofdm_test_waveform(double sample_rate_hz) {
    std::vector<std::complex<float>> wave;
    constexpr double kAmplitude = 0.5;

    // L-STF: 8us of the periodic short-symbol pattern, generated as one
    // continuous span (stf_reference() is periodic every 0.8us by
    // construction, so this IS the 10 repeats, not an approximation of
    // them).
    size_t n_stf = size_t(std::lround(8.0e-6 * sample_rate_hz));
    for (size_t i = 0; i < n_stf; ++i) {
        double t = double(i) / sample_rate_hz;
        auto v = stf_reference(t) * kAmplitude;
        wave.emplace_back(float(v.real()), float(v.imag()));
    }

    // L-LTF: 1.6us GI + 6.4us (both long symbols in one span) - exactly
    // the fixed timing classify_modulation() assumes follows the L-STF
    // (see wifi_phy.cpp's preamble_to_ltf_samples comment).
    append_symbol_span(wave, sample_rate_hz, kAmplitude, 1.6e-6, 6.4e-6);

    // "Data": 8 more 4us symbols (0.8us GI + 3.2us) of the same
    // full-bandwidth content, purely to give the burst a realistic
    // duration and occupied bandwidth - see this file's header for why
    // their content is unused by the RX side.
    for (int i = 0; i < 8; ++i) append_symbol_span(wave, sample_rate_hz, kAmplitude, 0.8e-6, 3.2e-6);

    size_t pad = size_t(std::lround(sample_rate_hz * 20e-6));
    std::vector<std::complex<float>> out(pad * 2 + wave.size(), std::complex<float>(0.0f, 0.0f));
    std::copy(wave.begin(), wave.end(), out.begin() + long(pad));
    return out;
}

// --- BenchTx -----------------------------------------------------------

BenchTx::BenchTx() = default;
BenchTx::~BenchTx() { stop(); }

void BenchTx::set_device_type(SdrDeviceType type) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    device_type_ = type;
}
SdrDeviceType BenchTx::device_type() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return device_type_;
}

void BenchTx::start(size_t usrp_channel) {
    if (running_.load()) return;
    usrp_channel_ = usrp_channel;
    stop_flag_.store(false);
    running_.store(true);
    thread_ = std::thread(&BenchTx::run, this);
}

void BenchTx::stop() {
    stop_flag_.store(true);
    if (thread_.joinable()) thread_.join();
    running_.store(false);
}

bool BenchTx::running() const { return running_.load(); }

void BenchTx::set_freq_hz(double hz) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    freq_hz_ = hz;
}
double BenchTx::freq_hz() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return freq_hz_;
}
void BenchTx::set_gain_db(double db) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    gain_db_ = db;
}
double BenchTx::gain_db() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return gain_db_;
}
void BenchTx::set_packet_type(TxPacketType t) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    packet_type_ = t;
}
TxPacketType BenchTx::packet_type() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return packet_type_;
}
void BenchTx::send_once() { send_requested_.store(true); }
void BenchTx::set_repeat(bool enabled, double period_s) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    repeat_enabled_ = enabled;
    repeat_period_s_ = period_s;
}
bool BenchTx::repeat_enabled() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return repeat_enabled_;
}
double BenchTx::repeat_period_s() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return repeat_period_s_;
}
BenchTxStatus BenchTx::status() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return status_;
}

// Same 6-attempt retry discipline as BenchCapture::connect_sdr() - the
// X310's Ethernet control channel in particular is documented
// (config.hpp) to fail to come up cleanly on a real fraction of
// attempts, independent of this app and of whether the session is RX
// or TX; harmless extra patience for the B210's USB3 link.
bool BenchTx::connect_sdr(SdrDeviceType type, size_t usrp_channel) {
    const DeviceProfile profile = device_profile(type);
    connected_device_type_ = type;
    std::exception_ptr last_error;
    for (int attempt = 0; attempt < 6 && !stop_flag_.load(); ++attempt) {
        try {
            streamer_.reset();
            usrp_.reset();
        } catch (...) {
        }
        try {
            usrp_ = uhd::usrp::multi_usrp::make(profile.device_args);
            // "TX/RX" is the one TX-capable antenna port name UHD uses
            // for both this project's devices (the X310's UBX-160
            // daughterboards and the B210's integrated AD9361 - see
            // config.hpp's DeviceProfile comment) - "RX2"
            // (DeviceProfile::antenna, what BenchCapture's RX side
            // uses) is receive-only on either.
            usrp_->set_tx_antenna("TX/RX", usrp_channel);
            last_error = nullptr;
            break;
        } catch (const std::exception&) {
            last_error = std::current_exception();
            std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        }
    }
    if (last_error) {
        std::string message;
        try {
            std::rethrow_exception(last_error);
        } catch (const std::exception& e) {
            message = e.what();
        }
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.connected = false;
        status_.error = message;
        return false;
    }
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.connected = true;
    status_.error.clear();
    return true;
}

void BenchTx::transmit_one() {
    double freq_hz, gain_db;
    TxPacketType type;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        freq_hz = freq_hz_;
        gain_db = gain_db_;
        type = packet_type_;
    }
    const DeviceProfile profile = device_profile(connected_device_type_);
    const double rate = profile.max_sample_rate_hz;
    try {
        // The TX streamer is built ONCE per connection, not per send -
        // its rate never changes across calls here, which sidesteps
        // the RFNoC streamer-rebuild hazard sdr_capture.cpp's
        // ensure_streamer() documents for the RX side entirely (that
        // hazard is specifically about tearing down and rebuilding a
        // streamer at a NEW rate on a live RFNoC graph; never doing so
        // means never hitting it).
        if (!streamer_ || current_rate_ != rate) {
            usrp_->set_tx_rate(rate, usrp_channel_);
            double actual = usrp_->get_tx_rate(usrp_channel_);
            uhd::stream_args_t args("fc32", "sc16");
            args.channels = {usrp_channel_};
            streamer_ = usrp_->get_tx_stream(args);
            current_rate_ = actual;
        }
        usrp_->set_tx_gain(gain_db, usrp_channel_);
        usrp_->set_tx_freq(uhd::tune_request_t(freq_hz), usrp_channel_);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));  // retune settle, mirrors RETUNE_SETTLE_S

        std::vector<std::complex<float>> wave = (type == TxPacketType::DSSS)
                                                      ? build_dsss_test_waveform(current_rate_)
                                                      : build_ofdm_test_waveform(current_rate_);

        // ONE send() call for the whole burst: per <uhd/stream.hpp>'s
        // own documented contract, send() fragments internally across
        // packets and places start_of_burst/end_of_burst correctly on
        // the first/last fragment on its own - no manual chunking loop
        // needed (a timeout return of fewer samples than requested is
        // treated as a real send failure below, not something to
        // cleverly resume).
        uhd::tx_metadata_t md;
        md.has_time_spec = false;
        md.start_of_burst = true;
        md.end_of_burst = true;
        size_t sent = streamer_->send(wave.data(), wave.size(), md, 3.0);

        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.send_count += 1;
        if (sent == wave.size()) {
            status_.error.clear();
            status_.last_action = std::string(type == TxPacketType::DSSS ? "Sent DSSS" : "Sent OFDM") +
                                   " test packet (" + std::to_string(wave.size()) + " samples @ " +
                                   std::to_string(int(current_rate_ / 1e6)) + "Msps)";
        } else {
            status_.error = "TX send() sent only " + std::to_string(sent) + "/" +
                             std::to_string(wave.size()) + " samples (timeout)";
        }
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.error = e.what();
    }
}

void BenchTx::run() {
    SdrDeviceType type;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        type = device_type_;
    }
    if (!connect_sdr(type, usrp_channel_)) {
        running_.store(false);
        return;
    }
    double last_repeat_time =
        std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();

    while (!stop_flag_.load()) {
        bool do_send = send_requested_.exchange(false);
        bool repeat_now = false;
        {
            std::lock_guard<std::mutex> lock(config_mutex_);
            if (repeat_enabled_) {
                double now = std::chrono::duration<double>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
                if (now - last_repeat_time >= repeat_period_s_) {
                    repeat_now = true;
                    last_repeat_time = now;
                }
            }
        }
        if (do_send || repeat_now) transmit_one();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    try {
        streamer_.reset();
        usrp_.reset();
    } catch (...) {
    }
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.connected = false;
    }
    running_.store(false);
}

}  // namespace rfmon::bench
