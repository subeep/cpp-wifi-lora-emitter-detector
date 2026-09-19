#include "lora_capture.hpp"
#include "lora_observation.hpp"
#include "lora_phy.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
#include <limits>
using namespace rfmon;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> void rejected(F f) {
    bool threw = false; try { f(); } catch (const std::exception&) { threw = true; }
    check(threw, "Malformed capture was accepted");
}
int main() {
    char path[] = "/tmp/rfmon-lora-test-XXXXXX";
    const char* tmp = mkdtemp(path); if (!tmp) return 1;
    try {
        LoraPacketRow row;
        set_lora_integrity(row, true, false, false);
        check(row.crc == LoraPacketRow::Crc::Absent && !row.crc_valid, "Absent CRC reported as failure");
        set_lora_integrity(row, true, true, false);
        check(row.crc == LoraPacketRow::Crc::Failed && row.crc_valid == false, "Failed CRC missing");
        set_lora_integrity(row, false, true, false);
        check(row.header_valid && !row.payload_complete && row.crc == LoraPacketRow::Crc::NotChecked,
              "Partial packet lost header evidence");

        lora::std_phy::StdParams p; p.sf = 7;
        std::vector<uint8_t> payload(80, 0x42);
        auto wave = lora::std_phy::modulate(payload, p);
        auto good = analyze_lora_hypothesis(wave, 7, true);
        check(good && good->crc == LoraPacketRow::Crc::Valid && good->payload_complete, "Valid fixture failed");
        check(good->decoder.find("Legacy SX reference") == 0, "Decoder provenance missing");
        auto clipped = wave; clipped.resize(clipped.size() - 128 * 10);
        auto partial = analyze_lora_hypothesis(clipped, 7, true);
        check(partial && partial->header_valid && !partial->payload_complete && !partial->payload_repr,
              "Truncated payload incorrectly presented as complete");
        auto corrupted = wave;
        // Preserve preamble/header; destroy the tail carrying payload/CRC.
        std::fill(corrupted.end() - 128 * 10, corrupted.end(), std::complex<float>(0, 0));
        auto failed = analyze_lora_hypothesis(corrupted, 7, true);
        check(failed && failed->crc == LoraPacketRow::Crc::Failed, "Corrupt payload marked valid");
        p.crc_on = false;
        auto no_crc = analyze_lora_hypothesis(lora::std_phy::modulate(payload, p), 7, true);
        check(no_crc && no_crc->crc == LoraPacketRow::Crc::Absent, "CRC-off waveform marked failed");
        lora::LoRaParams internal_params;
        auto production = analyze_lora_hypothesis(lora::modulate(payload, internal_params), 7);
        check(!production || production->decoder.find("Legacy") == std::string::npos,
              "Production silently fell back to internal codec");
        auto internal = analyze_lora_hypothesis(lora::modulate(payload, internal_params), 7, true);
        check(internal && internal->decoder.find("Legacy internal codec") == 0, "Internal codec mislabeled standard");
        check(analyze_lora_capture(std::vector<std::complex<float>>(2048), 500000, 866900000).empty(),
              "Silence produced a packet");

        LoraCapture c;
        c.iq = wave; c.sample_rate_hz = c.requested_sample_rate_hz = 125000;
        c.requested_center_hz = 866900000; c.requested_duration_s = double(wave.size()) / 125000;
        c.host_start_unix_s = 123456; c.device_args = "synthetic fixture";
        c.antenna = "none"; c.source = "synthetic/internal encoder, not OTA"; c.overflow = true;
        auto endian_fixture = c;
        endian_fixture.iq = {{1.0f, -2.5f}};
        auto endian_dir = save_lora_capture(tmp, endian_fixture);
        unsigned char encoded[8]{};
        { std::ifstream f(std::filesystem::path(endian_dir)/"iq.cf32_le", std::ios::binary);
          f.read(reinterpret_cast<char*>(encoded), 8); }
        const unsigned char expected[8] = {0, 0, 0x80, 0x3f, 0, 0, 0x20, 0xc0};
        check(std::equal(encoded, encoded + 8, expected), "IQ is not interleaved little-endian float32");
        endian_fixture.iq[0] = {std::numeric_limits<float>::quiet_NaN(), 0};
        rejected([&]{ save_lora_capture(tmp, endian_fixture); });
        auto dir = save_lora_capture(tmp, c);
        auto loaded = load_lora_capture(dir);
        check(loaded.iq == wave && loaded.overflow && loaded.source == c.source, "Capture round trip lost evidence");
        auto rows = analyze_lora_capture(loaded.iq, loaded.sample_rate_hz, loaded.requested_center_hz, true);
        bool found = false;
        for (auto& r : rows) if (r.sf == 7 && r.crc == LoraPacketRow::Crc::Valid && r.payload_hex == good->payload_hex) found = true;
        check(found, "Replay differs from direct analysis");
        auto second = save_lora_capture(tmp, c);
        check(second != dir, "Save overwrote prior capture");
        auto manifest = std::filesystem::path(dir) / "manifest.json";
        nlohmann::json j; { std::ifstream f(manifest); f >> j; }
        auto write_manifest = [&](nlohmann::json v) { std::ofstream(manifest) << v.dump(); };
        auto invalid = j; invalid["sample_count"] = 16000001; write_manifest(invalid);
        rejected([&]{ load_lora_capture(dir); });
        invalid = j; invalid["sample_count"] = -1; write_manifest(invalid);
        rejected([&]{ load_lora_capture(dir); });
        invalid = j; invalid["sample_rate_hz"] = 125001; write_manifest(invalid);
        rejected([&]{ load_lora_capture(dir); });
        invalid = j; invalid["version"] = 99; write_manifest(invalid);
        rejected([&]{ load_lora_capture(dir); });
        write_manifest(j);
        { std::fstream f(std::filesystem::path(dir)/"iq.cf32_le", std::ios::in|std::ios::out|std::ios::binary);
          char b; f.read(&b, 1); b ^= 1; f.seekp(0); f.write(&b, 1); }
        rejected([&]{ load_lora_capture(dir); });
        std::filesystem::resize_file(std::filesystem::path(second)/"iq.cf32_le", 10);
        rejected([&]{ load_lora_capture(second); });
        c.iq.clear(); rejected([&]{ save_lora_capture(tmp, c); });
        std::filesystem::remove_all(tmp);
        std::cout << "PASS: integrity states, corrupt/truncated/CRC-absent waveforms, decoder provenance, capture/replay and malformed files\n";
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << " (fixtures retained at " << tmp << ")\n"; return 1;
    }
}
