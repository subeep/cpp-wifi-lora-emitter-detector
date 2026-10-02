// Radio-free IQ -> observations -> capture events -> same live consumer state.
#include "capture_timing_json.hpp"
#include "security/lora_security.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace rfmon;
using namespace rfmon::lora_security;
int main(int argc,char** argv) {
    try {
        std::string record;std::vector<std::string> inputs;
        for(int i=1;i<argc;++i) {
            std::string arg=argv[i];if(arg=="--record") {if(++i>=argc)throw std::runtime_error("Missing --record path");record=argv[i];}
            else if(arg=="--help"){std::cout<<"lora_security_replay [--record NEW.ndjson] CAPTURE_DIRECTORY|EVENTS.ndjson...\n";return 0;}
            else inputs.push_back(arg);
        }
        if(inputs.empty())throw std::runtime_error("No inputs (see --help)");
        State state;std::ofstream out;
        if(!record.empty()){if(std::filesystem::exists(record))throw std::runtime_error("Output already exists");out.open(record);if(!out)throw std::runtime_error("Cannot write recording");}
        for(const auto& path:inputs) {
            if(std::filesystem::is_directory(path)) {
                auto c=load_lora_capture(path);
                if(c.run_id.empty()) {
                    nlohmann::json manifest;std::ifstream(std::filesystem::path(path)/"manifest.json")>>manifest;
                    c.run_id="legacy-iq:"+manifest.at("iq_fnv1a64").get<std::string>();c.capture_seq=1;
                }
                auto rows=analyze_lora_capture(c.iq,c.sample_rate_hz,c.requested_center_hz);
                auto batch=make_batch(c,c.iq.size(),rows);state.ingest(batch);if(out.is_open())out<<to_json(batch).dump()<<'\n';
            } else {
                std::ifstream in(path);if(!in)throw std::runtime_error("Cannot open "+path);std::string line;
                while(std::getline(in,line)) {
                    if(line.empty())continue;
                    try {
                        if(line.size()>4*1024*1024)throw std::runtime_error("LoRa event record too large");
                        auto j=nlohmann::json::parse(line);
                        if(j.at("schema")!="rfmon-lora-security"||j.at("version")!=1)throw std::runtime_error("Invalid record schema");
                        const std::string kind=j.at("kind");
                        if(kind=="capture_batch")state.ingest(from_json(j));
                        else if(kind=="loss")state.note_loss(capture_sample_count(j.at("captures")));
                        else if(kind=="rejected")state.note_rejected();
                        else throw std::runtime_error("Unknown record kind");
                        if(out.is_open())out<<j.dump()<<'\n';
                    } catch(const std::exception&){
                        state.note_rejected();
                        if(out.is_open())out<<"{\"schema\":\"rfmon-lora-security\",\"version\":1,\"kind\":\"rejected\"}\n";
                    }
                }
            }
        }
        if(out.is_open()){out.flush();if(!out)throw std::runtime_error("Recording write failed");}
        std::cout<<snapshot_json(state.snapshot()).dump(2)<<'\n';return state.snapshot().rejected_inputs?1:0;
    }catch(const std::exception& e){std::cerr<<"lora_security_replay: "<<e.what()<<'\n';return 1;}
}
