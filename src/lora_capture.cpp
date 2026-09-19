#include "lora_capture.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <cstring>
#include <chrono>
#include <stdexcept>
#include <limits>

namespace rfmon {
namespace {
using nlohmann::json;
namespace fs = std::filesystem;
// Raised from 16,000,000 (128MB) on 2026-09-19 for Milestone 2 fixture
// collection: a 60s coordinated real-transmission capture window at the
// project's standard 500ksps LoRa listen rate (DeviceProfile::
// lora_listen_capture_rate_hz) needs 30,000,000 samples. 32,000,000
// (256MB, ~64s at 500ksps) gives that margin without leaving the bound
// effectively unlimited. Purely permissive versus the old cap - every
// existing capture under the old limit is still valid, and the live
// GUI's own LORA_LISTEN_DURATION_S=2.0s captures stay far under either
// bound, so this does not change its behavior.
constexpr size_t max_samples = 32000000; // 256 MB payload maximum
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void validate(const LoraCapture& c, size_t n) {
    require(n > 0 && n <= max_samples, "IQ sample count outside 1..32000000");
    require(std::isfinite(c.sample_rate_hz) && c.sample_rate_hz >= 125000 && c.sample_rate_hz <= 16000000,
            "Unsupported actual sample rate");
    require(std::isfinite(c.requested_sample_rate_hz) && c.requested_sample_rate_hz > 0, "Invalid requested sample rate");
    require(std::isfinite(c.requested_center_hz) && c.requested_center_hz > 0 && c.requested_center_hz < 1e11,
            "Invalid center frequency");
    require(std::isfinite(c.requested_duration_s) && c.requested_duration_s > 0, "Invalid requested duration");
    require(std::isfinite(c.host_start_unix_s) && c.host_start_unix_s >= 0, "Invalid timestamp");
    require(!c.requested_gain_db || std::isfinite(*c.requested_gain_db), "Invalid gain");
    bool compatible = false;
    for (double bw : {125000., 250000., 500000.}) {
        double r = c.sample_rate_hz / bw;
        if (r >= 1 && std::abs(r - std::round(r)) < 1e-6) compatible = true;
    }
    require(compatible, "Sample rate is incompatible with current integer-decimation decoder");
}
uint64_t hash_bytes(uint64_t h, const unsigned char* bytes, size_t n) {
    for (size_t i = 0; i < n; ++i) { h ^= bytes[i]; h *= 1099511628211ULL; }
    return h;
}
void pack(float f, unsigned char* b) {
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
    uint32_t u; std::memcpy(&u, &f, 4);
    for (int i = 0; i < 4; ++i) b[i] = uint8_t(u >> (8 * i));
}
float unpack(const unsigned char* b) {
    uint32_t u = 0; for (int i = 0; i < 4; ++i) u |= uint32_t(b[i]) << (8 * i);
    float f; std::memcpy(&f, &u, 4); return f;
}
}
std::string save_lora_capture(const std::string& parent, const LoraCapture& c) {
    validate(c, c.iq.size());
    for (auto x : c.iq) require(std::isfinite(x.real()) && std::isfinite(x.imag()), "Non-finite IQ sample");
    fs::create_directories(parent);
    auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
    fs::path dir;
    for (unsigned i = 0; i < 1000; ++i) {
        dir = fs::path(parent) / ("capture-" + std::to_string(stamp) + "-" + std::to_string(i));
        if (fs::create_directory(dir)) break;
        require(i != 999, "Cannot allocate unique capture directory");
    }
    // A manifest is the commit marker. Partial writes have no manifest and cannot replay.
    try {
        std::ofstream iq(dir / "iq.cf32_le", std::ios::binary);
        iq.exceptions(std::ios::failbit | std::ios::badbit);
        uint64_t hash = 14695981039346656037ULL;
        for (auto x : c.iq) {
            unsigned char bytes[8]; pack(x.real(), bytes); pack(x.imag(), bytes + 4);
            hash = hash_bytes(hash, bytes, 8);
            iq.write(reinterpret_cast<char*>(bytes), 8);
        }
        iq.close();
        json j{{"schema", "rfmon-lora-iq"}, {"version", 1}, {"format", "cf32_le"},
               {"sample_count", c.iq.size()}, {"sample_rate_hz", c.sample_rate_hz},
               {"requested_sample_rate_hz", c.requested_sample_rate_hz},
               {"requested_center_hz", c.requested_center_hz}, {"requested_duration_s", c.requested_duration_s},
               {"host_start_unix_s", c.host_start_unix_s}, {"overflow", c.overflow},
               {"device_args", c.device_args}, {"antenna", c.antenna}, {"source", c.source},
               {"iq_fnv1a64", std::to_string(hash)}, {"decoder_revision", "lora-observation-v1"}};
        j["requested_gain_db"] = c.requested_gain_db ? json(*c.requested_gain_db) : json(nullptr);
        std::ofstream manifest(dir / "manifest.tmp");
        manifest.exceptions(std::ios::failbit | std::ios::badbit);
        manifest << j.dump(2) << '\n'; manifest.close();
        fs::rename(dir / "manifest.tmp", dir / "manifest.json");
    } catch (...) {
        // Only this call's newly allocated directory is eligible for cleanup.
        std::error_code ignored; fs::remove_all(dir, ignored); throw;
    }
    return fs::absolute(dir).string();
}
LoraCapture load_lora_capture(const std::string& directory) {
    fs::path dir(directory);
    require(fs::file_size(dir / "manifest.json") <= 65536, "Manifest too large");
    std::ifstream manifest(dir / "manifest.json"); json j; manifest >> j;
    require(j.at("schema") == "rfmon-lora-iq" && j.at("version") == 1 && j.at("format") == "cf32_le",
            "Unsupported capture schema/version/format");
    LoraCapture c;
    c.sample_rate_hz = j.at("sample_rate_hz"); c.requested_sample_rate_hz = j.at("requested_sample_rate_hz");
    c.requested_center_hz = j.at("requested_center_hz"); c.requested_duration_s = j.at("requested_duration_s");
    c.host_start_unix_s = j.at("host_start_unix_s"); c.overflow = j.at("overflow");
    c.device_args = j.at("device_args"); c.antenna = j.at("antenna"); c.source = j.at("source");
    if (!j.at("requested_gain_db").is_null()) c.requested_gain_db = j.at("requested_gain_db").get<double>();
    require(j.at("sample_count").is_number_unsigned(), "Invalid sample count");
    auto count = j.at("sample_count").get<uint64_t>();
    require(count <= max_samples, "IQ sample count too large");
    validate(c, size_t(count));
    require(fs::file_size(dir / "iq.cf32_le") == count * 8, "IQ length differs from manifest");
    c.iq.resize(size_t(count));
    std::ifstream iq(dir / "iq.cf32_le", std::ios::binary); iq.exceptions(std::ios::failbit | std::ios::badbit);
    uint64_t hash = 14695981039346656037ULL;
    for (auto& x : c.iq) {
        unsigned char bytes[8]; iq.read(reinterpret_cast<char*>(bytes), 8);
        hash = hash_bytes(hash, bytes, 8);
        x = {unpack(bytes), unpack(bytes + 4)};
        require(std::isfinite(x.real()) && std::isfinite(x.imag()), "Non-finite IQ sample");
    }
    require(j.at("iq_fnv1a64") == std::to_string(hash), "IQ checksum mismatch");
    return c;
}
} // namespace rfmon
