#include "wifi_enrolment.hpp"
#include "security/wifi_mac_frame.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <set>
#include <fcntl.h>
#include <unistd.h>

namespace rfmon::wifi_master {
using json=nlohmann::json;
namespace {
constexpr size_t kMaxJobs=64, kMaxBytes=32*1024*1024, kMaxContexts=16, kWindow=64, kHistory=128;
struct Sample { int64_t ts=0; std::array<double,6> values{}; unsigned valid=0; std::string source; };
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Sample,ts,values,valid,source)
double median(std::vector<double> x) { if(x.empty())return 0;std::sort(x.begin(),x.end());return x.size()%2?x[x.size()/2]:(x[x.size()/2-1]+x[x.size()/2])/2; }
double angle(double x) {return std::remainder(x,360.0);}
bool finite(const wifi_fingerprint::WifiFingerprint& f, bool ofdm) {
    for(auto x:{f.cfo_ppm,f.dc_dbc,f.dc_ang_deg,f.snr_db,f.evm_pct,f.sync_corr})if(!std::isfinite(x))return false;
    return !ofdm || (std::isfinite(f.irr_db)&&std::isfinite(f.iq_eps)&&std::isfinite(f.iq_phi_deg));
}
json digital_fields(const wifi::BeaconInfo& b) {
    json fields=json::object();
    if(b.ssid_present&&!b.ssid.empty())fields["SSID"]=wifi::display_text(b.ssid);
    if(b.ies_complete){fields["Security"]=b.security;fields["Ciphers"]=b.ciphers;fields["PMF"]=b.pmf;fields["Capabilities"]=b.capability;fields["Beacon interval"]=b.beacon_interval_tu;fields["PHY capabilities"]=b.standards;if(b.channel)fields["Advertised channel"]=b.channel;
        if(b.wps_present){fields["WPS manufacturer"]=wifi::display_text(b.wps_manufacturer);fields["WPS model"]=wifi::display_text(b.wps_model_name);fields["WPS number"]=wifi::display_text(b.wps_model_number);fields["WPS name"]=wifi::display_text(b.wps_device_name);}}
    return fields;
}
std::string source_id(const wifi_security::FrameEvent& f) {return enrolment_observation_key(f.run_id,f.radio_session,f.capture_seq,f.sample_start,f.phy);}
std::string context_id(const wifi_security::CaptureRecord& c,const std::string& phy) {
    // CaptureRecord has no stable serial/calibration ID. Sessions deliberately
    // stay separate instead of assuming two radios named "X310" are identical.
    std::ostringstream s;s<<c.device<<" | "<<c.run_id<<"/"<<c.radio_session<<" | "<<phy<<" | "<<std::setprecision(17)
      <<c.channel_hz<<"/"<<c.sample_rate_hz<<" | "<<c.antenna<<" | "<<(c.gain_db?std::to_string(*c.gain_db):"AGC/unknown")
      <<" | RF="<<c.actual_rf_hz<<" DSP="<<c.actual_dsp_hz<<" | estimator-v1";return s.str();
}
// -1: not replaced, 0: replaced but directory durability uncertain, 1: durable.
int write_atomic(const std::string& path,const std::string& text) {
    const auto tmp=path+".tmp";
    int fd=::open(tmp.c_str(),O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0)return -1;
    size_t done=0;bool ok=true;
    while(done<text.size()){auto n=::write(fd,text.data()+done,text.size()-done);if(n<0&&errno==EINTR)continue;if(n<=0){ok=false;break;}done+=size_t(n);}
    if(ok)ok=::fsync(fd)==0;if(::close(fd)!=0)ok=false;
    if(!ok || ::rename(tmp.c_str(),path.c_str())!=0)return -1;
    int d=::open(std::filesystem::path(path).parent_path().c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    ok=d>=0&&::fsync(d)==0;if(d>=0)::close(d);
    return ok?1:0;
}
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(EnrolmentEvent,id,ts,kind,detail,source)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RfProfileSummary,context,phy,source,centre,spread,reference,deviation,valid,captures,first_ts,last_ts,has_reference,review,cfo_history)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WifiEnrolmentInfo,policy,alias,note,role,imported,digital_review,observations,accepted,rejected,history_total,reference_version,seen_24,seen_5,last_source,last_rejection,history,profiles,archived_profiles)
struct WifiEnrolment::State {
    WifiEnrolmentInfo info;
    bool corrupt=false;
    std::map<std::string,std::deque<Sample>> windows;
    json reference_versions=json::array();
    json digital=json::object();
    void event(int64_t ts,const std::string& kind,const std::string& detail,const std::string& source) {
        info.history.push_back({++info.history_total,ts,kind.substr(0,80),detail.substr(0,1024),source.substr(0,512)});
        if(info.history.size()>kHistory)info.history.erase(info.history.begin());
    }
    void summarise(const std::string& context,const std::string& phy) {
        auto& samples=windows.at(context);if(samples.empty())return;
        auto it=std::find_if(info.profiles.begin(),info.profiles.end(),[&](const auto& p){return p.context==context;});
        if(it==info.profiles.end()){info.profiles.emplace_back();it=std::prev(info.profiles.end());it->context=context;}
        auto& p=*it;p.phy=phy;p.source=samples.back().source;p.first_ts=samples.front().ts;p.last_ts=samples.back().ts;p.captures=samples.size();p.valid=samples.back().valid;
        p.cfo_history.clear();for(const auto& x:samples)p.cfo_history.push_back(float(x.values[0]));
        for(size_t m=0;m<6;++m){if(!(p.valid&(1u<<m)))continue;std::vector<double> vals;
            const bool circular=m==3||m==5;const double origin=samples.back().values[m];
            for(const auto& x:samples)if(x.valid&(1u<<m))vals.push_back(circular?angle(x.values[m]-origin):x.values[m]);
            double centre=median(vals);std::vector<double> residuals;for(double v:vals)residuals.push_back(std::abs(v-centre));
            p.centre[m]=circular?angle(origin+centre):centre;p.spread[m]=median(residuals);
            p.deviation[m]=p.has_reference?(circular?angle(p.centre[m]-p.reference[m]):p.centre[m]-p.reference[m]):0;
        }
        // Diagnostics only: thresholds are deliberately not an authentication policy.
        // Compare the latest independent capture with the immutable accepted spread,
        // never an inflated current spread that could learn away a change.
        if(p.has_reference && samples.size()>=8){
            const std::array<double,6> floor={0.5,2.0,0.005,1.0,3.0,10.0};
            const json* ref=nullptr;
            for(auto v=reference_versions.rbegin();v!=reference_versions.rend()&&!ref;++v)
                for(const auto& r:v->at("profiles"))if(r.at("context")==context){ref=&r;break;}
            unsigned changed=0;bool cfo=false;
            if(ref){auto spread=ref->at("spread").get<std::array<double,6>>();for(size_t m=0;m<6;++m)if(p.valid&(1u<<m)){
                double delta=samples.back().values[m]-p.reference[m];if(m==3||m==5)delta=angle(delta);
                if(std::abs(delta)>std::max(floor[m],6*1.4826*spread[m])){if(m==0)cfo=true;else if(m==1||m==2||m==3)changed|=1;else changed|=2;}
            }}
            const bool unusual=cfo||changed!=0;
            // Require three consecutive independent captures; latch until reviewed.
            auto& run=deviations[context];run=unusual?run+1:0;
            if(run>=3&&!p.review){p.review=true;event(samples.back().ts,"RF review","Repeated deviation from accepted reference (experimental; not spoofing proof)",p.source);}
        }
    }
    std::map<std::string,unsigned> deviations; // consecutive runs reset on restart
};

WifiEnrolment::WifiEnrolment(const std::string& dir):directory_(dir),master_(dir) {
    for(const auto& row:master_.snapshot())if(row.key_is_mac){
        auto state=std::make_unique<State>();state->info.imported=true;
        state->info.role=row.identity?"AP / network interface":"Observed interface";
        if(row.identity)state->digital=digital_fields(*row.identity);
        if(row.last_channel_hz>=2400e6&&row.last_channel_hz<2500e6)state->info.seen_24=row.last_seen_ts;
        if(row.last_channel_hz>=5000e6&&row.last_channel_hz<6000e6)state->info.seen_5=row.last_seen_ts;
        const auto path=directory_+"/"+row.device_key+".enrol.json";
        try {
            if(std::filesystem::exists(path)){
                if(std::filesystem::is_symlink(path)||std::filesystem::file_size(path)>2*1024*1024)throw std::runtime_error("size/path");
                json j;std::ifstream(path)>>j;
                if(j.at("schema")!=1||j.at("key")!=row.device_key)throw std::runtime_error("schema/key");
                auto candidate=std::make_unique<State>();if(!j.at("info").contains("archived_profiles"))j["info"]["archived_profiles"]=json::array();candidate->info=j.at("info").get<WifiEnrolmentInfo>();
                candidate->windows=j.at("windows").get<decltype(candidate->windows)>();candidate->reference_versions=j.at("references");candidate->digital=j.at("digital");
                const auto& i=candidate->info;
                if(i.policy!="unreviewed"&&i.policy!="acknowledged"&&i.policy!="approved"&&i.policy!="disallowed")throw std::runtime_error("policy");
                if(i.history.size()>kHistory||i.profiles.size()>kMaxContexts||i.archived_profiles.size()>64||candidate->windows.size()>kMaxContexts||candidate->reference_versions.size()>8)throw std::runtime_error("bounds");
                for(const auto& [key,w]:candidate->windows){if(w.size()>kWindow)throw std::runtime_error("window");for(const auto& x:w)for(double v:x.values)if(!std::isfinite(v))throw std::runtime_error("nonfinite");}
                if(i.alias.size()>128||i.note.size()>1024||!candidate->digital.is_object()||!candidate->reference_versions.is_array())throw std::runtime_error("shape/text");
                for(const auto& e:i.history)if(e.kind.size()>80||e.detail.size()>1024||e.source.size()>512)throw std::runtime_error("history bounds");
                std::set<std::string> contexts;
                for(const auto& p:i.profiles){
                    if(p.context.size()>4096||!contexts.insert(p.context).second||p.valid>63||p.captures>kWindow||p.cfo_history.size()>kWindow)throw std::runtime_error("profile bounds");
                    for(const auto& a:{p.centre,p.spread,p.reference,p.deviation})for(double v:a)if(!std::isfinite(v))throw std::runtime_error("nonfinite profile");
                }
                for(const auto& version:candidate->reference_versions){
                    if(!version.at("profiles").is_array()||version.at("profiles").size()>kMaxContexts)throw std::runtime_error("reference shape");
                    for(const auto& r:version.at("profiles")){auto p=r.get<RfProfileSummary>();for(double v:p.spread)if(!std::isfinite(v)||v<0)throw std::runtime_error("reference spread");}
                }
                state=std::move(candidate);
            }
        }catch(const std::exception& e){state->corrupt=true;error_="Enrolment record unreadable; approval withheld: "+row.device_key+" ("+e.what()+")";}
        states_[row.device_key]=std::move(state);
    }
    publish();worker_=std::thread([this]{run();});
}
WifiEnrolment::~WifiEnrolment(){ {std::lock_guard<std::mutex> l(mutex_);stopping_=true;}ready_.notify_all();if(worker_.joinable())worker_.join(); }
void WifiEnrolment::flush(){std::unique_lock<std::mutex> l(mutex_);drained_.wait(l,[&]{return jobs_.empty()&&!busy_;});}
bool WifiEnrolment::submit(EnrolmentBatch b){
    size_t bytes=sizeof(b);for(const auto& x:b.observations)bytes+=sizeof(x)+x.frame.mpdu.size()+x.frame.run_id.size();bytes+=b.anonymous.size()*sizeof(AnonymousReading);
    std::lock_guard<std::mutex> l(mutex_);
    if(stopping_||jobs_.size()>=kMaxJobs||bytes>kMaxBytes||queued_bytes_+bytes>kMaxBytes||b.observations.size()>4000||b.anonymous.size()>400){++dropped_;return false;}
    queued_bytes_+=bytes;jobs_.push_back({std::move(b),{},bytes});ready_.notify_one();return true;
}
bool WifiEnrolment::command(EnrolmentCommand c){
    if(c.key.size()>64||c.alias.size()>128||c.note.size()>1024)return false;
    if(c.action!="acknowledge"&&c.action!="approve"&&c.action!="disallow"&&c.action!="unapprove"&&c.action!="note"&&c.action!="review_digital"&&c.action!="reference"&&c.action!="review_rf")return false;
    std::lock_guard<std::mutex> l(mutex_);if(stopping_||jobs_.size()>=kMaxJobs+8)return false;
    Job j;j.command=std::move(c);jobs_.push_back(std::move(j));ready_.notify_one();return true;
}
std::vector<WifiMasterRow> WifiEnrolment::snapshot() const {std::lock_guard<std::mutex> l(mutex_);return rows_;}
std::map<std::string,std::string> WifiEnrolment::associations() const {std::lock_guard<std::mutex> l(mutex_);return associations_;}
void WifiEnrolment::associate(const std::string& source,const std::string& key){
    if(source.empty()||key.empty())return;std::lock_guard<std::mutex> l(mutex_);
    if(associations_.emplace(source,key).second)association_order_.push_back(source);
    while(association_order_.size()>2048){associations_.erase(association_order_.front());association_order_.pop_front();}
}
EnrolmentHealth WifiEnrolment::health() const {std::lock_guard<std::mutex> l(mutex_);return {dropped_,rejected_sources_,jobs_.size()+(busy_?1:0),published_error_,published_command_};}
void WifiEnrolment::publish(){
    auto rows=master_.snapshot();for(auto& r:rows){auto it=states_.find(r.device_key);if(it!=states_.end())r.enrolment=std::make_shared<const WifiEnrolmentInfo>(it->second->info);}
    auto err=master_.storage_error();if(!error_.empty()){if(!err.empty())err+="; ";err+=error_;}
    std::lock_guard<std::mutex> l(mutex_);rows_=std::move(rows);published_error_=err;published_command_=command_result_;
}
bool WifiEnrolment::save(const std::string& key,const State& s){
    last_save_replaced_=false;
    if(s.corrupt){error_="Corrupt enrolment record preserved; repair required: "+key;return false;}
    if(!master_.writable()){error_="Enrolment changes not saved: store is read-only";return false;}
    // Policy acknowledgement must not outlive its underlying identity on crash.
    int base=::open((directory_+"/"+key+".ndjson").c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    bool durable=base>=0 && ::fsync(base)==0;if(base>=0)::close(base);
    if(!durable){error_="Cannot flush underlying Wi-Fi identity: "+key;return false;}
    json j={{"schema",1},{"key",key},{"info",s.info},{"windows",s.windows},{"references",s.reference_versions},{"digital",s.digital}};
    const auto outcome=write_atomic(directory_+"/"+key+".enrol.json",j.dump()+"\n");
    last_save_replaced_=outcome>=0;
    if(outcome!=1){error_=outcome==0?"Enrolment applied but directory durability UNCONFIRMED: "+key:"Cannot save enrolment for "+key;return false;}return true;
}
bool WifiEnrolment::backup(const std::string& key){
    const auto path=directory_+"/"+key+".ndjson", bak=path+".pre-enrolment";
    std::error_code ec;
    if(!std::filesystem::exists(path,ec)||std::filesystem::exists(bak,ec))return !ec;
    const auto tmp=bak+".tmp";
    if(!std::filesystem::copy_file(path,tmp,std::filesystem::copy_options::overwrite_existing,ec)){
        error_="Cannot back up legacy Wi-Fi record: "+key;return false;
    }
    int fd=::open(tmp.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    bool ok=fd>=0&&::fsync(fd)==0;if(fd>=0)::close(fd);
    if(ok)ok=::rename(tmp.c_str(),bak.c_str())==0;
    if(ok){int d=::open(directory_.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);ok=d>=0&&::fsync(d)==0;if(d>=0)::close(d);}
    if(!ok)error_="Legacy backup could not be committed: "+key;
    return ok;
}
void WifiEnrolment::run(){
    for(;;){Job job;{std::unique_lock<std::mutex> l(mutex_);ready_.wait(l,[&]{return stopping_||!jobs_.empty();});if(jobs_.empty()&&stopping_)break;job=std::move(jobs_.front());jobs_.pop_front();queued_bytes_-=job.bytes;busy_=true;}
        try{if(job.command)apply(*job.command);else process(job.batch);}catch(const std::exception& e){error_="Enrolment processing error: "+std::string(e.what());if(job.command)command_result_="Command failed; inspect storage error";}
        publish();{std::lock_guard<std::mutex> l(mutex_);busy_=false;if(jobs_.empty())drained_.notify_all();}
    }
}
void WifiEnrolment::process(const EnrolmentBatch& batch){
    const auto& c=batch.capture;
    if(c.source!="live"){std::lock_guard<std::mutex> l(mutex_);++rejected_sources_;return;}
    if(!master_.writable())return;
    if(!std::isfinite(c.channel_hz)||!std::isfinite(c.sample_rate_hz)||c.sample_rate_hz<=0||c.run_id.empty()){
        error_="Enrolment rejected capture with invalid frequency/rate/run provenance";return;
    }
    const auto run=c.run_id;
    auto previous=capture_highwater_.find(run);
    if(previous!=capture_highwater_.end() && (c.radio_session<previous->second.first ||
       (c.radio_session==previous->second.first && c.capture_seq<=previous->second.second)))return;
    if(previous==capture_highwater_.end()&&capture_highwater_.size()>=64){error_="Enrolment run limit reached; restart required for further independent runs";return;}
    capture_highwater_[run]={c.radio_session,c.capture_seq};
    std::set<std::string> changed, sampled;
    std::set<wifi_security::IngestKey> ingested;
    for(const auto& obs:batch.observations){const auto& f=obs.frame;
        if(f.run_id!=c.run_id||f.radio_session!=c.radio_session||f.capture_seq!=c.capture_seq||(f.phy!="OFDM"&&f.phy!="DSSS")||f.channel_hz!=c.channel_hz||f.sample_rate_hz!=c.sample_rate_hz||!f.fcs_valid||f.mpdu_truncated||f.sample_start>=c.samples_received||f.sample_length>c.samples_received-f.sample_start||!ingested.insert(wifi_security::ingest_key(f)).second)continue;
        auto mac=wifi_security::parse_mac_frame(f.mpdu.data(),f.mpdu.size());
        if(mac.fcs!=wifi_security::FcsStatus::Valid||mac.header_state!=wifi_security::ParseState::Ok||!mac.transmitter||wifi_security::is_group_address(*mac.transmitter))continue;
        const auto address=wifi_security::format_mac(*mac.transmitter);
        // BSSID identity and RF transmitter must agree for a beacon association.
        if(mac.beacon_identity&&mac.beacon_identity->bssid!=address)continue;
        auto it=states_.find(address);if(it!=states_.end()&&it->second->info.imported&&!backup(address))continue;
        const int64_t ts=f.host_time_ns?*f.host_time_ns/1000000000LL:(c.host_before_ns?c.host_before_ns/1000000000LL:std::time(nullptr));
        std::string key=mac.beacon_identity?master_.record_identity(*mac.beacon_identity,c.channel_hz,ts,f.phy):master_.record_transmitter(address,c.channel_hz,ts,f.phy);
        if(key.empty())continue;
        associate(source_id(f),key);
        if(it==states_.end()){auto state=std::make_unique<State>();state->event(ts,"Discovered","New claimed transmitting interface",source_id(f));it=states_.emplace(key,std::move(state)).first;}
        auto& s=*it->second;auto& i=s.info;changed.insert(key);++i.observations;i.last_source=source_id(f);
        if(c.channel_hz>=2400e6&&c.channel_hz<2500e6)i.seen_24=std::max(i.seen_24,ts);
        if(c.channel_hz>=5000e6&&c.channel_hz<6000e6)i.seen_5=std::max(i.seen_5,ts);
        if(mac.beacon_identity){i.role="AP / network interface";const auto& b=*mac.beacon_identity;
            // Omitted/incomplete fields cannot fabricate a downgrade or erase names.
            const auto fields=digital_fields(b);
            for(auto p=fields.begin();p!=fields.end();++p){if(s.digital.contains(p.key())&&s.digital[p.key()]!=p.value()){
                    s.event(ts,"Digital change",p.key()+": "+s.digital[p.key()].dump()+" -> "+p.value().dump(),i.last_source);i.digital_review=true;
                }s.digital[p.key()]=p.value();}
        } else if(mac.fc.to_ds&&!mac.fc.from_ds&&i.role!="AP / network interface")i.role="Station transmitter (observed)";
        if(!obs.fingerprint)continue;const auto& fp=*obs.fingerprint;
        if(fp.gated_out||!finite(fp,f.phy=="OFDM")){++i.rejected;i.last_rejection=fp.gated_out?fp.gate_reason:"Non-finite RF estimate";continue;}
        ++i.accepted;master_.record_reading(key,f.phy,c.channel_hz,f.bandwidth_hz,fp,ts);
        if(!c.security_usable()||f.after_overflow){i.last_rejection="RF summary excluded: incomplete capture coverage";continue;}
        const auto context=context_id(c,f.phy);if(!sampled.insert(key+context).second)continue;
        if(!s.windows.count(context)&&s.windows.size()>=kMaxContexts){
            auto oldest=std::min_element(i.profiles.begin(),i.profiles.end(),[](const auto& a,const auto& b){return a.last_ts<b.last_ts;});
            if(oldest!=i.profiles.end()){
                auto archived=*oldest;archived.cfo_history.clear();i.archived_profiles.push_back(std::move(archived));
                if(i.archived_profiles.size()>64)i.archived_profiles.erase(i.archived_profiles.begin());
                s.windows.erase(oldest->context);s.deviations.erase(oldest->context);i.profiles.erase(oldest);
            }
        }
        Sample x;x.ts=ts;x.values={fp.cfo_ppm,fp.irr_db,fp.iq_eps,fp.iq_phi_deg,fp.dc_dbc,fp.dc_ang_deg};x.valid=f.phy=="OFDM"?63u:49u;x.source=i.last_source;
        const bool returning=s.windows.count(context)==0;
        auto& w=s.windows[context];w.push_back(x);if(w.size()>kWindow)w.pop_front();s.summarise(context,f.phy);
        if(returning){
            auto& p=i.profiles.back();
            for(auto a=i.archived_profiles.rbegin();a!=i.archived_profiles.rend();++a)if(a->context==context){p.review=a->review;break;}
            i.archived_profiles.erase(std::remove_if(i.archived_profiles.begin(),i.archived_profiles.end(),[&](const auto& a){return a.context==context;}),i.archived_profiles.end());
            for(auto version=s.reference_versions.rbegin();version!=s.reference_versions.rend();++version){
                auto found=std::find_if(version->at("profiles").begin(),version->at("profiles").end(),[&](const auto& r){return r.at("context")==context;});
                if(found!=version->at("profiles").end()){p.reference=found->at("reference").template get<std::array<double,6>>();p.has_reference=true;break;}
            }
            if(p.has_reference)for(size_t m=0;m<6;++m)if(p.valid&(1u<<m))
                p.deviation[m]=(m==3||m==5)?angle(p.centre[m]-p.reference[m]):p.centre[m]-p.reference[m];
        }
    }
    for(const auto& r:batch.anonymous)if(!r.fingerprint.gated_out&&finite(r.fingerprint,r.phy=="OFDM")){auto key=master_.record_reading({},r.phy,r.hz,r.bandwidth,r.fingerprint,r.ts);associate(r.source,key);}
    for(const auto& key:changed)save(key,*states_.at(key));
}
void WifiEnrolment::apply(const EnrolmentCommand& c){
    auto it=states_.find(c.key);if(it==states_.end()){command_result_="Identity no longer available";return;}
    if(!master_.writable()||!master_.storage_error().empty()){command_result_="Not saved: resolve Wi-Fi storage errors first";return;}
    State next=*it->second;auto& i=next.info;const auto ts=std::time(nullptr);
    if(c.action=="acknowledge"){if(i.policy=="unreviewed")i.policy="acknowledged";}
    else if(c.action=="approve")i.policy="approved";
    else if(c.action=="disallow")i.policy="disallowed";
    else if(c.action=="unapprove")i.policy="acknowledged";
    else if(c.action=="note"){i.alias=c.alias;i.note=c.note;}
    else if(c.action=="review_digital")i.digital_review=false;
    else if(c.action=="review_rf"){for(auto& p:i.profiles)p.review=false;for(auto& p:i.archived_profiles)p.review=false;next.deviations.clear();}
    else if(c.action=="reference"){
        if(i.policy!="approved"){command_result_="Approve the identity before accepting an RF reference";return;}
        json profiles=json::array();
        for(auto& p:i.profiles)if(p.captures>=8&&p.last_ts-p.first_ts>=10){p.reference=p.centre;p.has_reference=true;p.review=false;p.deviation={};profiles.push_back(p);}
        if(profiles.empty()){command_result_="Reference needs at least 8 usable captures spanning 10 seconds in one context";return;}
        if(next.reference_versions.size()>=8){command_result_="Reference version limit reached; existing references preserved";return;}
        next.reference_versions.push_back({{"version",++i.reference_version},{"ts",ts},{"profiles",profiles}});next.deviations.clear();
    }else{command_result_="Unsupported enrolment action";return;}
    next.event(ts,"Operator: "+c.action,c.note,"Local operator; not authenticated");
    const bool durable=save(c.key,next);
    if(durable||last_save_replaced_){*it->second=std::move(next);command_result_=(durable?"Saved: ":"Applied, durability UNCONFIRMED: ")+c.action+" — "+c.key;}
    else command_result_="NOT SAVED: "+c.action;
}
}
