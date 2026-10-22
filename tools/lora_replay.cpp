// Offline only: never constructs a radio or writes emitter identity records.
#include "lora_capture.hpp"
#include "lora_observation.hpp"
#include <iostream>
#include <set>
#include <nlohmann/json.hpp>
int main(int argc, char** argv) {
    bool legacy=false,json_output=false;
    if(argc<2 || argc>4){std::cerr<<"Usage: lora_replay CAPTURE_DIRECTORY [--legacy-lab] [--json]\n";return 2;}
    for(int i=2;i<argc;++i){const std::string option=argv[i];
        if(option=="--legacy-lab"&&!legacy)legacy=true;
        else if(option=="--json"&&!json_output)json_output=true;
        else {std::cerr<<"Unknown or repeated option\n";return 2;}}
    try {
        auto c = rfmon::load_lora_capture(argv[1]);
        auto rows = rfmon::analyze_lora_capture(c.iq, c.sample_rate_hz, c.requested_center_hz, legacy);
        if(!legacy)rfmon::annotate_lora_receptions(rows,c.sample_rate_hz,"offline:"+std::string(argv[1]));
        if(json_output) {
            nlohmann::json result={{"schema","rfmon.lora.replay.v1"},{"source",c.source},
                {"samples",c.iq.size()},{"sample_rate_hz",c.sample_rate_hz},{"center_hz",c.requested_center_hz},
                {"overflow",c.overflow},{"legacy_lab",legacy},{"rows",nlohmann::json::array()}};
            for(const auto& r:rows)result["rows"].push_back({{"sf",r.sf},{"bandwidth_khz",r.bandwidth_khz},
                {"decoder",r.decoder},{"status",r.status},{"crc",rfmon::lora_crc_label(r.crc)},
                {"payload_hex",r.payload_hex},{"reception_id",r.reception_id},{"header_valid",r.header_valid}});
            std::cout<<result.dump(2)<<'\n';return 0;
        }
        std::cout << "Source: " << c.source << "\nSamples: " << c.iq.size()
                  << "\nActual rate: " << c.sample_rate_hz << "\nRequested center: " << c.requested_center_hz
                  << "\nOverflow: " << (c.overflow ? "yes (discontinuous IQ)" : "no") << '\n';
        std::set<std::string> reception_groups;
        for (const auto& r : rows) {
            if(!r.reception_id.empty()) {
                reception_groups.insert(r.reception_id);
                std::cout<<"Reception group: "<<r.reception_id<<" ("<<r.reception_hypotheses<<" source hypotheses)\n";
            }
            if (r.lorawan_candidate) std::cout << *r.lorawan_candidate << ": " << *r.lorawan_detail << '\n';
            if (r.inverted_iq) std::cout << "IQ=" << (*r.inverted_iq ? "inverted" : "normal")
                << " preamble=" << r.preamble_peak_ratio.value_or(0) << " SFD=" << r.sfd_peak_ratio.value_or(0)
                << " CFO_Hz=" << r.cfo_hz.value_or(0) << " drift_Hz/symbol=" << r.drift_hz_per_symbol.value_or(0)
                << " offset_s=" << r.capture_offset_s.value_or(0) << " FEC_mismatches=" << r.fec_disagreements.value_or(0) << '\n';
            std::cout << "SF" << r.sf << " BW=" << r.bandwidth_khz << " kHz | " << r.decoder
                      << " | " << r.status << " | CRC=" << rfmon::lora_crc_label(r.crc)
                      << "\n" << r.detail << "\nHEX: " << r.payload_hex << '\n';
        }
        std::cout << rows.size() << " hypothesis results (not a unique packet count)\n";
        std::cout << reception_groups.size() << " conservative capture-local reception groups (not unique devices or delivery rate)\n";
    } catch (const std::exception& e) { std::cerr << "Replay failed: " << e.what() << '\n'; return 1; }
}
