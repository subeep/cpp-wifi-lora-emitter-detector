#include "security/lora_security.hpp"
#include "capture_timing_json.hpp"
#include "lorawan_inspect.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <tuple>
namespace rfmon::lora_security {
namespace {
using nlohmann::json;
void require(bool b,const char* msg){if(!b)throw std::runtime_error(msg);}
std::vector<uint8_t> payload_bytes(const std::string& text) {
    std::vector<uint8_t> bytes;int high=-1;
    for(unsigned char ch:text) {
        if(ch==' ')continue;
        int n=ch>='0'&&ch<='9'?ch-'0':ch>='a'&&ch<='f'?ch-'a'+10:ch>='A'&&ch<='F'?ch-'A'+10:-1;
        require(n>=0,"Invalid LoRa payload hex");
        if(high<0)high=n;else {bytes.push_back(uint8_t(high*16+n));high=-1;}
        require(bytes.size()<=255,"LoRa payload too large");
    }
    require(high<0,"Odd LoRa payload hex");return bytes;
}
bool supported_rate(double rate) {
    for(double bw:{125000.,250000.,500000.}) {
        double f=rate/bw;
        if(std::isfinite(f)&&f>=1&&f<=1024&&std::abs(f-std::round(f))<1e-6)return true;
    }
    return false;
}
size_t analysis_span(double rate,size_t received) {
    size_t factor=1;
    for(double bw:{125000.,250000.,500000.}) {
        double f=rate/bw;
        if(std::isfinite(f)&&f>=1&&f<=1024&&std::abs(f-std::round(f))<1e-6)factor=std::max(factor,size_t(std::llround(f)));
    }
    return received/factor*factor; // common span fully processed by all supported BW hypotheses
}
void validate(const Batch& b) {
    const auto& c=b.capture;
    require(!c.run_id.empty()&&c.run_id.size()<=256&&c.source.size()<=2048&&c.device.size()<=2048&&c.antenna.size()<=256,"Invalid LoRa capture identity/context");
    require(std::isfinite(c.rate_hz)&&c.rate_hz>0&&c.rate_hz<=16e6&&std::isfinite(c.center_hz)&&c.center_hz>0&&c.center_hz<1e11&&
        (!c.gain_db||std::isfinite(*c.gain_db)),"Invalid LoRa receiver settings");
    require(c.received<=32000000&&c.analysed<=c.received&&b.events.size()<=kMaxEvents&&c.hypotheses<=100000,
        "LoRa coverage/event limits exceeded");
    require(c.supported==supported_rate(c.rate_hz)&&(!c.processed?c.analysed==0:c.received>0&&c.analysed>0),
        "Inconsistent LoRa analysis/coverage flags");
    if(c.timing) {
        validate_capture_timing(*c.timing,c.received,c.rate_hz);
        require(c.overflow==!c.timing->overflows.empty(),"LoRa overflow metadata disagreement");
    }
    for(const auto& e:b.events) {
        require(e.sample_start<e.sample_end&&e.sample_end<=c.received&&e.start_uncertainty_samples<=16000000&&
            e.sf>=7&&e.sf<=12&&(e.bandwidth_hz==125000||e.bandwidth_hz==250000||e.bandwidth_hz==500000)&&
            (!e.cr||(*e.cr>=1&&*e.cr<=4))&&e.payload.size()<=255&&e.decoder.size()<=256&&e.lorawan_candidate.size()<=256,
            "Invalid LoRa event span/PHY/payload");
        require(e.header_valid&&((e.payload_complete&&e.crc!=LoraPacketRow::Crc::NotChecked)||
            (!e.payload_complete&&e.crc==LoraPacketRow::Crc::NotChecked&&e.payload.empty())),"Inconsistent LoRa integrity flags");
        const auto expected=c.timing?capture_sample_time(*c.timing,c.rate_hz,c.received,e.sample_start):std::nullopt;
        require(e.device_time_ns==expected,"LoRa event time disagrees with capture provenance");
    }
}
}
bool Capture::usable() const {
    return processed&&supported&&!laboratory&&!overflow&&!analysis_limited&&received>0&&analysed>0&&analysed<=received&&
        std::isfinite(rate_hz)&&rate_hz>0&&(!timing||(!timing->timed_out&&!timing->exception&&timing->overflows.empty()));
}
Batch make_batch(const LoraCapture& input,size_t received,const std::vector<LoraPacketRow>& rows,bool laboratory) {
    Batch b;auto& c=b.capture;
    c.run_id=input.run_id;c.capture_seq=input.capture_seq;c.radio_session=input.radio_session;
    c.source=input.source;c.device=input.device_args;c.antenna=input.antenna;
    c.center_hz=input.requested_center_hz;c.rate_hz=input.sample_rate_hz;c.gain_db=input.requested_gain_db;
    c.received=received;c.timing=input.timing;c.overflow=input.overflow;c.laboratory=laboratory;c.hypotheses=rows.size();
    // Legacy fixtures have no live ingest identity. The checked IQ content is
    // given a deterministic offline identity by the replay tool.
    c.supported=supported_rate(c.rate_hz);
    c.analysed=c.supported?analysis_span(c.rate_hz,received):0;c.processed=c.analysed>0;
    std::set<std::tuple<size_t,size_t,int,double,std::vector<uint8_t>,int>> seen;
    for(const auto& row:rows) {
        c.analysis_limited=c.analysis_limited||row.analysis_limited;
        if(!row.header_valid||!row.decoder_sample_start||!row.decoder_sample_end) {++c.unlocated;continue;}
        double f=c.rate_hz/(row.bandwidth_khz*1000);
        if(!std::isfinite(f)||f<1||f>1024||std::abs(f-std::round(f))>1e-6) {++c.unlocated;continue;}
        size_t factor=size_t(std::llround(f));Event e;
        if(*row.decoder_sample_start>received/factor || *row.decoder_sample_end>received/factor) {c.analysis_limited=true;continue;}
        e.sample_start=*row.decoder_sample_start*factor;e.sample_end=*row.decoder_sample_end*factor;
        e.start_uncertainty_samples=row.start_uncertainty_samples*factor;
        e.run_id=c.run_id;e.capture_seq=c.capture_seq;e.radio_session=c.radio_session;
        e.sf=row.sf;e.bandwidth_hz=row.bandwidth_khz*1000;e.cr=row.cr;e.inverted_iq=row.inverted_iq;
        e.header_valid=row.header_valid;e.payload_complete=row.payload_complete;e.crc=row.crc;e.decoder=row.decoder;
        e.payload=payload_bytes(row.payload_hex);
        // Only collapse identical hypotheses at exactly the same sample span
        // and PHY context. Different samples remain actual repeated receptions.
        auto key=std::make_tuple(e.sample_start,e.sample_end,e.sf,e.bandwidth_hz,e.payload,int(e.crc));
        if(!seen.insert(key).second) {++c.deduplicated;continue;}
        if(b.events.size()>=kMaxEvents) {c.analysis_limited=true;break;}
        if(c.timing)e.device_time_ns=capture_sample_time(*c.timing,c.rate_hz,received,e.sample_start);
        if(e.protocol_eligible())if(auto p=inspect_lorawan(e.payload))e.lorawan_candidate=p->type;
        b.events.push_back(std::move(e));
    }
    return b;
}
json to_json(const Batch& b) {
    const auto& c=b.capture;json events=json::array();
    for(const auto& e:b.events)events.push_back({{"sample_start",e.sample_start},{"sample_end",e.sample_end},
        {"start_uncertainty_samples",e.start_uncertainty_samples},{"sf",e.sf},{"bandwidth_hz",e.bandwidth_hz},
        {"cr",e.cr?json(*e.cr):json(nullptr)},{"inverted_iq",e.inverted_iq?json(*e.inverted_iq):json(nullptr)},
        {"header_valid",e.header_valid},{"payload_complete",e.payload_complete},{"crc",lora_crc_label(e.crc)},
        {"payload",e.payload},{"device_time_ns",e.device_time_ns?json(*e.device_time_ns):json(nullptr)},
        {"decoder",e.decoder},{"lorawan_candidate",e.lorawan_candidate}});
    return {{"schema","rfmon-lora-security"},{"version",1},{"kind","capture_batch"},{"run_id",c.run_id},
        {"capture_seq",c.capture_seq},{"radio_session",c.radio_session},{"source",c.source},{"device",c.device},
        {"antenna",c.antenna},{"center_hz",c.center_hz},{"rate_hz",c.rate_hz},{"gain_db",c.gain_db?json(*c.gain_db):json(nullptr)},
        {"received",c.received},{"analysed",c.analysed},{"hypotheses",c.hypotheses},{"deduplicated",c.deduplicated},
        {"unlocated",c.unlocated},{"timing",c.timing?capture_timing_json(*c.timing):json(nullptr)},
        {"overflow",c.overflow},{"processed",c.processed},{"analysis_limited",c.analysis_limited},
        {"laboratory",c.laboratory},{"supported",c.supported},{"events",events}};
}
Batch from_json(const json& j) {
    require(j.at("schema")=="rfmon-lora-security"&&j.at("version")==1&&j.at("kind")=="capture_batch","Unsupported LoRa event schema");
    Batch b;auto& c=b.capture;c.run_id=j.at("run_id");c.capture_seq=capture_sample_count(j.at("capture_seq"));
    c.radio_session=capture_sample_count(j.at("radio_session"));c.source=j.at("source");c.device=j.at("device");c.antenna=j.at("antenna");
    c.center_hz=j.at("center_hz");c.rate_hz=j.at("rate_hz");if(!j.at("gain_db").is_null())c.gain_db=j.at("gain_db").get<double>();
    c.received=capture_sample_count(j.at("received"));c.analysed=capture_sample_count(j.at("analysed"));
    c.hypotheses=capture_sample_count(j.at("hypotheses"));c.deduplicated=capture_sample_count(j.at("deduplicated"));c.unlocated=capture_sample_count(j.at("unlocated"));
    if(!j.at("timing").is_null())c.timing=capture_timing_from_json(j.at("timing"),c.received,c.rate_hz);
    c.overflow=j.at("overflow");c.processed=j.at("processed");c.analysis_limited=j.at("analysis_limited");c.laboratory=j.at("laboratory");c.supported=j.at("supported");
    const auto& events=j.at("events");require(events.is_array()&&events.size()<=kMaxEvents,"LoRa event capacity exceeded");
    for(const auto& item:events) {
        Event e;e.run_id=c.run_id;e.capture_seq=c.capture_seq;e.radio_session=c.radio_session;e.sample_start=capture_sample_count(item.at("sample_start"));e.sample_end=capture_sample_count(item.at("sample_end"));
        e.start_uncertainty_samples=capture_sample_count(item.at("start_uncertainty_samples"));e.sf=item.at("sf");e.bandwidth_hz=item.at("bandwidth_hz");
        if(!item.at("cr").is_null())e.cr=item.at("cr").get<int>();if(!item.at("inverted_iq").is_null())e.inverted_iq=item.at("inverted_iq").get<bool>();
        e.header_valid=item.at("header_valid");e.payload_complete=item.at("payload_complete");
        const std::string crc=item.at("crc");
        require(crc=="Valid"||crc=="Failed"||crc=="Absent"||crc=="Not checked","Unknown LoRa integrity state");
        e.crc=crc=="Valid"?LoraPacketRow::Crc::Valid:crc=="Failed"?LoraPacketRow::Crc::Failed:crc=="Absent"?LoraPacketRow::Crc::Absent:LoraPacketRow::Crc::NotChecked;
        require(item.at("payload").is_array()&&item.at("payload").size()<=255,"LoRa payload too large");
        for(const auto& byte:item.at("payload")) {auto v=capture_sample_count(byte);require(v<=255,"Invalid payload byte");e.payload.push_back(uint8_t(v));}
        if(!item.at("device_time_ns").is_null())e.device_time_ns=item.at("device_time_ns").get<int64_t>();
        e.decoder=item.at("decoder");e.lorawan_candidate=item.at("lorawan_candidate");b.events.push_back(std::move(e));
    }
    validate(b);return b;
}
void State::ingest(const Batch& b) {
    validate(b);const auto& c=b.capture;
    auto it=last_seq_.find(c.run_id);
    if(it!=last_seq_.end()&&c.capture_seq<=it->second) {
        if(c.capture_seq==it->second)++snapshot_.duplicates;
        else {++snapshot_.out_of_order;snapshot_.health="Out-of-order capture excluded";session_ends_.clear();}
        return;
    }
    if(it==last_seq_.end()&&last_seq_.size()>=32)last_seq_.erase(last_seq_.begin());
    last_seq_[c.run_id]=c.capture_seq;
    ++snapshot_.captures;snapshot_.latest=c;snapshot_.hypotheses_deduplicated+=c.deduplicated;
    snapshot_.sampled_s+=double(c.received)/c.rate_hz;snapshot_.analysed_s+=double(c.analysed)/c.rate_hz;
    bool ordered=true;
    const auto session=std::make_pair(c.run_id,c.radio_session);
    if(c.timing&&c.timing->device_time_valid) {
        if(!session_ends_.count(session)&&session_ends_.size()>=32)session_ends_.erase(session_ends_.begin());
        auto it=session_ends_.find(session);
        if(it!=session_ends_.end()) {
            if(!it->second)++snapshot_.unknown_gaps;
            else if(c.timing->device_time_ns<*it->second){ordered=false;++snapshot_.time_regressions;}
            else snapshot_.dead_s+=double((long double)c.timing->device_time_ns-*it->second)/1e9;
        }
        const long double end=(long double)c.timing->device_time_ns+double(c.received)/c.rate_hz*1e9L;
        if(c.overflow||c.timing->timed_out||c.timing->exception||end>INT64_MAX||end<INT64_MIN)session_ends_[session]=std::nullopt;
        else session_ends_[session]=int64_t(end);
    } else session_ends_.erase(session);
    const bool usable=c.usable()&&ordered;
    if(usable)snapshot_.usable_s+=double(c.analysed)/c.rate_hz;else ++snapshot_.excluded;
    const auto profile=json{{"center_hz",c.center_hz},{"rate_hz",c.rate_hz},{"gain_db",c.gain_db?json(*c.gain_db):json(nullptr)},
        {"antenna",c.antenna},{"device",c.device}}.dump();
    if(!snapshot_.coverage.count(profile)&&snapshot_.coverage.size()>=32){snapshot_.coverage.erase(snapshot_.coverage.begin());++snapshot_.profiles_evicted;}
    auto& cov=snapshot_.coverage[profile];cov.center_hz=c.center_hz;cov.rate_hz=c.rate_hz;cov.gain_db=c.gain_db;
    cov.antenna=c.antenna;cov.device=c.device;++cov.captures;cov.excluded+=!usable;
    cov.sampled_s+=double(c.received)/c.rate_hz;if(usable)cov.usable_s+=double(c.analysed)/c.rate_hz;
    if(c.timing&&c.timing->device_time_valid)++snapshot_.device_timed;
    snapshot_.health=!c.processed?"Capture failed or not analysed":!c.supported?"Unsupported sample rate":
        c.laboratory?"Laboratory decoder excluded":!ordered?"Device time regression; security input excluded":!c.usable()?"Incomplete coverage; security input excluded":
        !c.timing||!c.timing->device_time_valid?"Supported-PHY coverage; hardware timing unavailable":"Usable supported-PHY coverage";
    std::set<std::tuple<size_t,size_t,int,double,std::vector<uint8_t>,int>> seen;
    for(const auto& e:b.events) {
        if(!seen.insert({e.sample_start,e.sample_end,e.sf,e.bandwidth_hz,e.payload,int(e.crc)}).second) {++snapshot_.hypotheses_deduplicated;continue;}
        ++snapshot_.events;snapshot_.eligible+=e.protocol_eligible()&&usable;
        snapshot_.crc_failed+=e.crc==LoraPacketRow::Crc::Failed;snapshot_.crc_absent+=e.crc==LoraPacketRow::Crc::Absent;snapshot_.partial+=!e.payload_complete;
        snapshot_.recent.push_back(e);if(snapshot_.recent.size()>256)snapshot_.recent.pop_front();
    }
}
void State::note_loss(uint64_t count){session_ends_.clear();++snapshot_.unknown_gaps;snapshot_.input_loss+=count;snapshot_.health="Capture queue loss; observation gap";}
void State::note_rejected(){session_ends_.clear();++snapshot_.unknown_gaps;++snapshot_.rejected_inputs;snapshot_.health="Rejected input; observation gap";}
json snapshot_json(const Snapshot& s) {
    json recent=json::array();for(const auto& e:s.recent)recent.push_back({{"run_id",e.run_id},{"capture_seq",e.capture_seq},{"radio_session",e.radio_session},{"sample_start",e.sample_start},{"sf",e.sf},
        {"bandwidth_hz",e.bandwidth_hz},{"crc",lora_crc_label(e.crc)},{"payload",e.payload},
        {"device_time_ns",e.device_time_ns?json(*e.device_time_ns):json(nullptr)},{"start_uncertainty_samples",e.start_uncertainty_samples}});
    json coverage=json::array();for(const auto& [key,c]:s.coverage)coverage.push_back({{"key",key},{"center_hz",c.center_hz},
        {"rate_hz",c.rate_hz},{"gain_db",c.gain_db?json(*c.gain_db):json(nullptr)},{"antenna",c.antenna},{"device",c.device},
        {"captures",c.captures},{"excluded",c.excluded},{"sampled_s",c.sampled_s},{"usable_s",c.usable_s}});
    return {{"schema","rfmon-lora-security-snapshot"},{"version",1},{"attack_rules_enabled",0},
        {"capability","Capture evidence only; LoRa replay/flood classification not implemented"},
        {"captures",s.captures},{"duplicates",s.duplicates},{"out_of_order",s.out_of_order},{"excluded",s.excluded},
        {"device_timed",s.device_timed},{"events",s.events},{"eligible",s.eligible},{"crc_failed",s.crc_failed},
        {"crc_absent",s.crc_absent},{"partial",s.partial},{"hypotheses_deduplicated",s.hypotheses_deduplicated},
        {"input_loss",s.input_loss},{"rejected_inputs",s.rejected_inputs},{"sampled_s",s.sampled_s},
        {"analysed_s",s.analysed_s},{"usable_s",s.usable_s},{"dead_s",s.dead_s},{"unknown_gaps",s.unknown_gaps},{"time_regressions",s.time_regressions},
        {"profiles_evicted",s.profiles_evicted},{"coverage",coverage},{"health",s.health},{"recording_error",s.recording_error},{"recent",recent}};
}
Monitor::Monitor(std::string path,size_t capacity,uint64_t max_bytes):published_(std::make_shared<Snapshot>()),queue_capacity_(capacity),record_max_bytes_(max_bytes),recording_path_(std::move(path)) {
    require(capacity>0&&capacity<=64&&max_bytes>=8192,"Invalid LoRa monitor limits");
}
Monitor::~Monitor(){stop();}
void Monitor::start(){std::lock_guard<std::mutex> l(mutex_);if(running_)return;stopping_=false;running_=true;thread_=std::thread(&Monitor::run,this);}
void Monitor::stop(){ {std::lock_guard<std::mutex> l(mutex_);if(!running_)return;stopping_=true;}ready_.notify_one();if(thread_.joinable())thread_.join();std::lock_guard<std::mutex> l(mutex_);running_=false;}
bool Monitor::submit(Batch b){std::lock_guard<std::mutex> l(mutex_);if(!running_||stopping_||queue_.size()>=queue_capacity_||b.events.size()>kMaxEvents){++loss_;ready_.notify_one();return false;}queue_.push_back(std::move(b));ready_.notify_one();return true;}
std::shared_ptr<const Snapshot> Monitor::snapshot() const {std::lock_guard<std::mutex> l(mutex_);return published_;}
void Monitor::run() {
    State& state=state_;std::ofstream out;uint64_t bytes=0;std::string error;
    auto open=[&] {
        if(recording_path_.empty())return;
        std::filesystem::path path(recording_path_);if(!path.parent_path().empty())std::filesystem::create_directories(path.parent_path());
        bytes=std::filesystem::exists(path)?std::filesystem::file_size(path):0;
        out.open(path,std::ios::app);if(!out)throw std::runtime_error("Cannot open LoRa security recording");
    };
    try{open();}catch(const std::exception& e){error=e.what();}
    for(;;) {
        Batch batch;bool have=false,done=false;uint64_t loss=0;
        {std::unique_lock<std::mutex> l(mutex_);ready_.wait(l,[&]{return stopping_||!queue_.empty()||loss_;});
            loss=loss_;loss_=0;if(!queue_.empty()){batch=std::move(queue_.front());queue_.pop_front();have=true;}done=stopping_&&queue_.empty();}
        auto record=[&](const json& j) {
            if(!out.is_open())return;const std::string line=j.dump()+"\n";
            try {
                if(line.size()>record_max_bytes_/2)throw std::runtime_error("LoRa security record exceeds rotation limit");
                if(bytes+line.size()>record_max_bytes_/2) {
                    out.close();std::filesystem::path old=recording_path_+".1";
                    std::filesystem::remove(old);std::filesystem::rename(recording_path_,old);bytes=0;out.open(recording_path_,std::ios::trunc);
                }
                out<<line;out.flush();if(!out)throw std::runtime_error("LoRa security recording write failed");bytes+=line.size();
            }catch(const std::exception& e){error=e.what();out.close();}
        };
        if(loss){state.note_loss(loss);record({{"schema","rfmon-lora-security"},{"version",1},{"kind","loss"},{"captures",loss}});}
        if(have)try{state.ingest(batch);record(to_json(batch));}catch(const std::exception&){state.note_rejected();record({{"schema","rfmon-lora-security"},{"version",1},{"kind","rejected"}});}
        auto snapshot=std::make_shared<Snapshot>(state.snapshot());snapshot->recording_error=error;
        {std::lock_guard<std::mutex> l(mutex_);published_=std::move(snapshot);}
        if(done)break;
    }
}
} // namespace rfmon::lora_security
