#include "lora_capture.hpp"
#include "lorawan_inspect.hpp"
#include "lora_observation.hpp"
#include "lora_phy.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
#include <limits>
#include <cstdio>
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
        std::vector<uint8_t> uplink={0x40,0x04,0x03,0x02,0x01,0x80,0x34,0x12,1,0xab,0xde,0xad,0xbe,0xef};
        auto mac=inspect_lorawan(uplink);
        check(mac && mac->detail.find("DevAddr=01020304")!=std::string::npos &&
              mac->detail.find("FCnt16=4660")!=std::string::npos && mac->detail.find("MIC not verified")!=std::string::npos,
              "LoRaWAN structure/endian/authentication label wrong");
        auto badmac=uplink;badmac[5]=0x8f;
        check(!inspect_lorawan(badmac),"Truncated FOpts accepted");
        badmac=uplink;badmac[0]|=1;check(!inspect_lorawan(badmac),"Unsupported major accepted");
        check(!inspect_lorawan({'A','u','t','o'}),"ASCII mislabeled LoRaWAN");
        auto join=std::vector<uint8_t>(23,0);check(inspect_lorawan(join).has_value(),"Join request rejected");
        join.pop_back();check(!inspect_lorawan(join),"Truncated join accepted");
        auto accept=std::vector<uint8_t>(17,0);accept[0]=0x20;
        check(inspect_lorawan(accept)->detail.find("Encrypted")!=std::string::npos,"Join accept exposed encrypted fields");
        auto down=std::vector<uint8_t>(12,0);down[0]=0x60;
        check(inspect_lorawan(down).has_value(),"Empty downlink rejected");
        down[5]=0x40;check(!inspect_lorawan(down),"Downlink RFU accepted");
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

        // Grouping uses one capture and aligned data anchors; raw rows stay intact.
        LoraPacketRow anchored;
        anchored.decoder="LoRa explicit PHY";anchored.sf=7;anchored.bandwidth_khz=125;
        anchored.cr=1;anchored.payload_len=3;anchored.payload_hex="01 02 03";
        anchored.header_valid=anchored.payload_complete=true;anchored.crc=LoraPacketRow::Crc::Valid;
        anchored.inverted_iq=false;anchored.decoder_sample_start=200;anchored.decoder_data_start=1200;
        anchored.decoder_sample_end=2000;anchored.start_uncertainty_samples=128;anchored.fec_disagreements=2;
        auto duplicate=anchored;duplicate.decoder_sample_start=204;duplicate.decoder_data_start=1224;
        duplicate.decoder_sample_end=2004;duplicate.fec_disagreements=0;
        std::vector<LoraPacketRow> evidence={anchored,duplicate};
        auto grouped=group_lora_receptions(evidence,500000);
        check(grouped.groups.size()==1&&grouped.groups[0].hypotheses.size()==2&&grouped.groups[0].representative==1,
              "Aligned duplicate hypotheses not grouped with stable representative");
        annotate_lora_receptions(evidence,500000,"run-A/session-1/capture-1");
        check(evidence.size()==2&&evidence[0].decoder_sample_start==200&&evidence[1].fec_disagreements==0&&
              evidence[0].reception_id==evidence[1].reception_id&&evidence[0].reception_hypotheses==2,
              "Grouping removed or rewrote raw evidence");
        const auto first_id=evidence[0].reception_id;
        annotate_lora_receptions(evidence,500000,"run-A/session-2/capture-1");
        check(evidence[0].reception_id!=first_id,"Capture/session-local grouping ID reused across scopes");
        auto separate=[&](LoraPacketRow other,const char* why) {
            auto g=group_lora_receptions({anchored,other},500000);check(g.groups.size()==2,why);
        };
        auto variant=anchored;variant.decoder_sample_start=3000;variant.decoder_data_start=4000;variant.decoder_sample_end=4800;
        separate(variant,"Repeated identical payload at later samples collapsed");
        variant=anchored;variant.payload_hex="04 05 06";separate(variant,"Conflicting CRC-valid bytes collapsed");
        variant=anchored;variant.sf=8;separate(variant,"Different SF collapsed");
        variant=anchored;variant.bandwidth_khz=250;separate(variant,"Different bandwidth collapsed");
        variant=anchored;variant.cr=2;separate(variant,"Different header coding rate collapsed");
        variant=anchored;variant.crc=LoraPacketRow::Crc::Failed;separate(variant,"Failed CRC promoted into valid group");
        variant=anchored;variant.crc=LoraPacketRow::Crc::Absent;separate(variant,"Absent CRC promoted into valid group");
        variant=anchored;variant.payload_complete=false;variant.crc=LoraPacketRow::Crc::NotChecked;variant.payload_hex.clear();
        separate(variant,"Partial evidence promoted into complete group");
        variant=anchored;variant.decoder_data_start=1264;separate(variant,"Overlap alone collapsed distinct anchors");
        variant=anchored;variant.decoder_data_start.reset();separate(variant,"Unknown anchor guessed from overlap");
        variant=anchored;variant.freq_mhz=867.1;separate(variant,"Different receive centres collapsed");
        auto chain=duplicate;chain.decoder_data_start=1248;chain.fec_disagreements=0;
        grouped=group_lora_receptions({anchored,duplicate,chain},500000);
        check(grouped.groups.size()==2,"Transitive grouping chained nearby receptions");
        variant=anchored;variant.inverted_iq=true;
        check(group_lora_receptions({anchored,variant},500000).groups.size()==1,"Matching CRC-checked polarity hypotheses not grouped");
        auto uncertain=anchored;uncertain.crc=LoraPacketRow::Crc::Absent;variant=uncertain;variant.inverted_iq=true;
        check(group_lora_receptions({uncertain,variant},500000).groups.size()==2,"Unverified opposite polarity collapsed");
        variant=anchored;variant.decoder_data_start=*variant.decoder_sample_end;
        grouped=group_lora_receptions({anchored,variant},500000);
        check(grouped.groups.size()==2&&grouped.unlocated==1,"Malformed data anchor grouped");
        check(group_lora_receptions({anchored},std::numeric_limits<double>::quiet_NaN()).unlocated==1,
              "Invalid actual rate used for grouping");
        // No aligned anchor in old rows: only exact context/span duplication.
        auto legacy=anchored;legacy.decoder_data_start.reset();auto legacy_duplicate=legacy;
        check(group_lora_receptions({legacy,legacy_duplicate},500000).groups.size()==1,"Legacy exact-span grouping regressed");
        legacy_duplicate.decoder_sample_start=201;
        check(group_lora_receptions({legacy,legacy_duplicate},500000).groups.size()==2,"Legacy approximate span guessed");

        // Adversarial alternatives are kept, with bounded comparison work.
        std::vector<LoraPacketRow> crowded(1500,anchored);
        for(size_t i=0;i<crowded.size();++i){char hex[12];std::snprintf(hex,sizeof(hex),"%02x %02x 00",unsigned(i&255),unsigned(i>>8));crowded[i].payload_hex=hex;}
        grouped=group_lora_receptions(crowded,500000);
        check(grouped.comparison_limited&&grouped.groups.size()==crowded.size(),
              "Grouping work cap removed ambiguous evidence or was not reported");

        // Legacy-only test generator padding must also be safe at partial
        // FEC blocks. This is not a production interoperability oracle.
        for(bool crc:{false,true})for(size_t n:{size_t(0),size_t(1),size_t(2),size_t(79),size_t(80),size_t(81)}) {
            lora::std_phy::StdParams params;params.sf=7;params.crc_on=crc;
            auto generated=lora::std_phy::modulate(std::vector<uint8_t>(n,0x42),params);
            check(!generated.empty(),"Legacy padding fixture generation failed");
        }

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
