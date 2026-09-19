// Finite, receive-only LoRa IQ capture tool for Milestone 2 fixture
// collection (EXECUTE_NEXT.md Section 5.3 point 5: "a dedicated
// finite receive-only capture tool using the same capture format").
//
// The GUI's "Save next completed capture" button needs a human to
// click it and isn't scriptable from here; this gives the same result
// (a save_lora_capture() directory, the exact format lora_replay and
// the interop tests already load) from one command, with a
// user-controlled listen duration rather than the live scanner's fixed
// LORA_LISTEN_DURATION_S=2.0s. It does not touch Scanner, the live GUI,
// or any persistent identity list - a single UsrpCapture connect,
// capture, save, disconnect, matching this project's existing
// tools/band_smoke_test.cpp convention for a one-shot hardware tool.
#include <chrono>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#include "config.hpp"
#include "lora_capture.hpp"
#include "sdr_capture.hpp"

using namespace rfmon;

namespace {

void print_usage(const char* argv0) {
    std::fprintf(stderr,
        "Usage: %s [options]\n"
        "  --device b210|x310   (default x310)\n"
        "  --channel N          USRP RX channel, 0 or 1 (default 0)\n"
        "  --freq HZ            center frequency in Hz (default 865100000, the\n"
        "                       TarangMini's readback frequency - see\n"
        "                       data/lora_m2/2026-09-18-tx-recovery/transmitter.txt)\n"
        "  --rate HZ            requested sample rate in Hz (default 500000,\n"
        "                       matching DeviceProfile::lora_listen_capture_rate_hz)\n"
        "  --duration SECONDS   capture length (default 8.0; capped at 32,000,000\n"
        "                       samples by the capture format - e.g. max 64s at\n"
        "                       500000 Hz, checked up front before connecting)\n"
        "  --gain DB            manual RX gain in dB (default 5.0 - deliberately low;\n"
        "                       raise it only if the signal turns out too weak, per\n"
        "                       EXECUTE_NEXT.md Section 5.1's saturation warning for a\n"
        "                       physically close transmitter/receiver pair)\n"
        "  --agc                use AGC instead of --gain (B210 only; X310's UBX has none)\n"
        "  --out DIR            parent directory for the saved capture\n"
        "                       (default data/lora_m2/2026-09-18-tx-recovery/captures)\n"
        "Prints the saved capture's directory path on stdout on success.\n",
        argv0);
}

}  // namespace

int main(int argc, char** argv) {
    SdrDeviceType device_type = SdrDeviceType::X310;
    size_t channel = 0;
    double freq_hz = 865.1e6;
    double rate_hz = 500e3;
    double duration_s = 8.0;
    std::optional<double> gain_db = 5.0;
    std::string out_dir = "data/lora_m2/2026-09-18-tx-recovery/captures";

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "Missing value for %s\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--device") {
            std::string v = next();
            device_type = (v == "b210") ? SdrDeviceType::B210 : SdrDeviceType::X310;
        } else if (a == "--channel") {
            channel = size_t(std::stoul(next()));
        } else if (a == "--freq") {
            freq_hz = std::stod(next());
        } else if (a == "--rate") {
            rate_hz = std::stod(next());
        } else if (a == "--duration") {
            duration_s = std::stod(next());
        } else if (a == "--gain") {
            gain_db = std::stod(next());
        } else if (a == "--agc") {
            gain_db = std::nullopt;
        } else if (a == "--out") {
            out_dir = next();
        } else if (a == "--help" || a == "-h") {
            print_usage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "Unknown argument: %s\n", a.c_str());
            print_usage(argv[0]);
            return 2;
        }
    }

    // save_lora_capture() (lora_capture.cpp) hard-caps IQ at 32,000,000
    // samples (256MB) and REJECTS anything over that at save time, after
    // the capture has already happened - there is no partial-save
    // fallback, so a too-long request silently discards real captured
    // RF data rather than trimming it. Refuse before ever calling
    // capture() so a long --duration fails fast, not after burning a
    // live transmission window.
    const double max_duration_s = 32000000.0 / rate_hz;
    if (duration_s > max_duration_s) {
        std::fprintf(stderr,
                     "FAIL: --duration %.2fs at %.0f Hz would need %.0f samples, over the "
                     "32,000,000-sample capture-format cap (max duration at this rate: %.2fs).\n",
                     duration_s, rate_hz, duration_s * rate_hz, max_duration_s);
        return 2;
    }

    DeviceProfile profile = device_profile(device_type);
    std::string gain_desc = gain_db.has_value() ? (std::to_string(*gain_db) + " dB") : "AGC";
    std::fprintf(stderr,
                 "Connecting: device_args=%s antenna=%s channel=%zu gain=%s\n",
                 profile.device_args.c_str(), profile.antenna.c_str(), channel, gain_desc.c_str());

    // Same 6-attempt/1200ms retry discipline as Scanner::connect_sdr()
    // and the Wi-Fi bench's BenchCapture/BenchTx::connect_sdr() (see
    // their own comments) - this X310's Ethernet control channel is
    // documented (config.hpp) to fail "Failure to create rfnoc_graph" /
    // management-transaction-timeout on a real, measured fraction of
    // connection attempts, independent of this tool. A one-shot connect
    // here would otherwise burn a coordinated real-transmission window
    // on a purely transport-level coin flip.
    std::unique_ptr<UsrpCapture> sdr;
    std::exception_ptr last_error;
    for (int attempt = 0; attempt < 6; ++attempt) {
        try {
            sdr = std::make_unique<UsrpCapture>(profile.antenna, gain_db, channel,
                                                 profile.device_args);
            last_error = nullptr;
            break;
        } catch (const std::exception&) {
            last_error = std::current_exception();
            std::fprintf(stderr, "Connect attempt %d/6 failed, retrying...\n", attempt + 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        }
    }
    if (last_error) {
        try {
            std::rethrow_exception(last_error);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "FAIL: could not connect after 6 attempts: %s\n", e.what());
        }
        return 1;
    }

    try {
        double host_start_unix_s =
            std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        std::fprintf(stderr,
                     "Capturing %.2fs at %.6f MHz, requested %.3f ksps ... (transmit now)\n",
                     duration_s, freq_hz / 1e6, rate_hz / 1e3);

        auto [iq, actual_rate, overflow] = sdr->capture(freq_hz, rate_hz, duration_s);

        std::fprintf(stderr, "Capture done: %zu samples, actual_rate=%.3f ksps, overflow=%s\n",
                     iq.size(), actual_rate / 1e3, overflow ? "YES" : "no");
        if (iq.empty()) {
            std::fprintf(stderr, "FAIL: zero samples returned - dropped capture, not saved.\n");
            return 1;
        }

        LoraCapture cap;
        cap.iq = std::move(iq);
        cap.sample_rate_hz = actual_rate;
        cap.requested_sample_rate_hz = rate_hz;
        cap.requested_center_hz = freq_hz;
        cap.requested_duration_s = duration_s;
        cap.host_start_unix_s = host_start_unix_s;
        cap.requested_gain_db = gain_db;
        cap.overflow = overflow;
        cap.device_args = profile.device_args;
        cap.antenna = profile.antenna;
        cap.source = "live";

        std::string dir = save_lora_capture(out_dir, cap);
        std::printf("%s\n", dir.c_str());
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
