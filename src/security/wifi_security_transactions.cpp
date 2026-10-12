#include "security/wifi_security_transactions.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace rfmon::wifi_security {
TransactionReplayRule::TransactionReplayRule(TransactionConfig c):cfg_(c) {
    for(double t:{c.min_delay_s,c.retention_s,c.max_gap_s})
        if(!std::isfinite(t)||t<=0)throw std::invalid_argument("transaction timing limit");
    if(c.retention_s<c.min_delay_s||!c.max_pending||!c.max_frames||!c.max_profiles||!c.max_links||c.history<2 ||
       c.max_pending>64||c.max_frames>512||c.max_profiles>16||c.max_links>256||c.history>16)
        throw std::invalid_argument("transaction capacity limit");
}
nlohmann::json TransactionReplayRule::diagnostics() const {
    return {{"enabled",cfg_.enabled},{"evaluated",evaluated_},{"excluded_captures",excluded_},
        {"history_evictions",forgotten_},{"unsupported",unsupported_},{"policy","historical_answered_transaction_v1"},
        {"min_delay_s",cfg_.min_delay_s},{"retention_s",cfg_.retention_s},{"max_gap_s",cfg_.max_gap_s},
        {"max_pending",cfg_.max_pending},{"max_frames",cfg_.max_frames},{"max_profiles",cfg_.max_profiles},
        {"max_links",cfg_.max_links},{"history",cfg_.history},{"max_frame_bytes",2048},{"cryptographic_validation",false}};
}
void TransactionReplayRule::interrupt(){pending_.clear();profiles_.clear();lost_=true;}
void TransactionReplayRule::note_frame(const FrameEvent& e,const MacFrame& f) {
    if(!cfg_.enabled)return;
    auto key=std::make_pair(e.run_id,e.capture_seq);
    if(!pending_.count(key)&&pending_.size()>=cfg_.max_pending)interrupt();
    auto& p=pending_[key];++p.received;
    bool relevant=f.is_management(mgmt::Authentication)||f.is_management(mgmt::AssocRequest)||
        f.is_management(mgmt::AssocResponse)||f.is_management(mgmt::ReassocRequest)||f.is_management(mgmt::ReassocResponse)||
        f.is_management(mgmt::Deauthentication)||f.is_management(mgmt::Disassociation)||
        (f.data && f.data->ethertype==0x888e);
    if(!relevant)return;
    if(!f.fcs_valid()||f.header_state!=ParseState::Ok||f.body_state!=BodyState::Readable||f.fc.protected_frame||
       f.fc.more_fragments||f.fragment_number.value_or(0)||!f.bssid||!f.transmitter||!f.receiver||
       is_group_address(*f.transmitter)||is_group_address(*f.receiver)||e.mpdu.size()>2048){++unsupported_;p.degraded=true;return;}
    Frame v;v.ap=format_mac(*f.bssid);v.retry=f.fc.retry;
    const bool from_ap=*f.transmitter==*f.bssid;
    if(!from_ap && *f.receiver!=*f.bssid){++unsupported_;p.degraded=true;return;}
    v.client=format_mac(from_ap?*f.receiver:*f.transmitter);
    if(v.client==v.ap){++unsupported_;p.degraded=true;return;}
    v.link=v.ap+"/"+v.client;
    v.event=e;
    v.bytes.assign(e.mpdu.begin(),e.mpdu.end()-4);v.bytes[1]&=~uint8_t(8);
    if(f.management) {
        if(f.fc.to_ds||f.fc.from_ds){++unsupported_;p.degraded=true;return;}
        v.reset=f.is_management(mgmt::Deauthentication)||f.is_management(mgmt::Disassociation);
        if(!v.reset) {
            v.kind=f.is_management(mgmt::Authentication)?"authentication":
                (f.is_management(mgmt::ReassocRequest)||f.is_management(mgmt::ReassocResponse)?"reassociation":"association");
            v.request=!from_ap;v.counter=f.sequence_number.value_or(0);
            if(v.kind=="authentication") {
                if(f.management->auth_algorithm!=0||f.management->auth_transaction!=(v.request?1:2)||
                   f.management->status_code!=0){++unsupported_;p.degraded=true;return;}
            } else if((v.request && !(f.is_management(mgmt::AssocRequest)||f.is_management(mgmt::ReassocRequest)))||
                      (!v.request && (!(f.is_management(mgmt::AssocResponse)||f.is_management(mgmt::ReassocResponse))||
                                     f.management->status_code!=0))) {++unsupported_;p.degraded=true;return;}
        }
    } else {
        if(!f.data||!f.data->eapol_key||f.data->amsdu){++unsupported_;p.degraded=true;return;}
        const auto& k=*f.data->eapol_key;
        // Supported RSN 16-byte-MIC layouts only. Do not turn a truncated
        // advertised EAPOL body or ambiguous flag inference into evidence.
        const size_t off=f.header_len+8;
        if(off+4>e.mpdu.size()-4){++unsupported_;p.degraded=true;return;}
        const size_t len=(size_t(e.mpdu[off+2])<<8)|e.mpdu[off+3];
        if(!k.key_data_length||len!=95+*k.key_data_length||off+4+len!=e.mpdu.size()-4||
           k.descriptor_type!=2||k.descriptor_version<1||k.descriptor_version>3||k.protocol_version<1||k.protocol_version>3||
           !k.pairwise||k.error||k.request||(k.key_info&0xe030)||f.fc.to_ds==f.fc.from_ds||
           from_ap!=f.fc.from_ds){++unsupported_;p.degraded=true;return;}
        if(k.message==EapolKeyMessage::M1 && !k.install&&!k.secure&&!k.encrypted_key_data&&!k.nonce_zero) v.stage=1;
        if(k.message==EapolKeyMessage::M2 && !k.install&&!k.encrypted_key_data&&!k.nonce_zero) v.stage=2;
        if(k.message==EapolKeyMessage::M3 && k.secure&&!k.nonce_zero) v.stage=3;
        if(k.message==EapolKeyMessage::M4 && !k.install&&!k.encrypted_key_data&&k.nonce_zero) v.stage=4;
        if(!v.stage || from_ap!=(v.stage==1||v.stage==3)){++unsupported_;p.degraded=true;return;}
        v.kind="eapol";v.eapol=true;v.request=from_ap;v.counter=k.replay_counter;v.nonce=k.nonce;
        v.bytes.assign(e.mpdu.begin()+off,e.mpdu.end()-4);
    }
    if(e.clock!=ClockDomain::UsrpDevice||!e.device_time_ns||e.after_overflow||p.frames.size()>=cfg_.max_frames){p.degraded=true;return;}
    v.evidence.frame=ingest_key(e);v.evidence.host_ns=e.host_time_ns.value_or(0);
    v.evidence.summary=describe(f);v.evidence.mpdu.assign(e.mpdu.begin(),e.mpdu.begin()+std::min(e.mpdu.size(),size_t(512)));
    p.frames.push_back(std::move(v));
}
TransactionResult TransactionReplayRule::finish_capture(const CaptureRecord& c,IncidentStore& incidents) {
    TransactionResult out;if(!cfg_.enabled)return out;
    Pending p;auto it=pending_.find({c.run_id,c.capture_seq});if(it!=pending_.end()){p=std::move(it->second);pending_.erase(it);}
    const auto profile_key=BaselineStore::key_for(c);
    out.usable=c.security_usable()&&c.clock==ClockDomain::UsrpDevice&&!lost_&&!p.degraded&&p.received==c.events_submitted;
    lost_=false;
    const long double end=(long double)c.device_time_ns+c.observed_seconds()*1e9L;
    if(!std::isfinite(c.sample_rate_hz)||c.sample_rate_hz<=0||end>INT64_MAX||end<INT64_MIN)out.usable=false;
    int64_t previous=c.device_time_ns;
    for(const auto& v:p.frames) {
        const auto& e=v.event;
        const long double anchor=(long double)c.device_time_ns+(long double)e.sample_start*1e9L/c.sample_rate_hz;
        if(!e.device_time_ns||*e.device_time_ns<previous||*e.device_time_ns>=end||e.sample_start>=c.analysed_samples||
           !e.sample_length||e.sample_length>c.analysed_samples-e.sample_start||e.radio_session!=c.radio_session||e.band!=c.band||
           e.channel!=c.channel||e.sample_rate_hz!=c.sample_rate_hz||std::abs(anchor-*e.device_time_ns)>1.0L)out.usable=false;
        previous=e.device_time_ns.value_or(previous);
    }
    if(!out.usable){++excluded_;profiles_.erase(profile_key);return out;}
    if(!profiles_.count(profile_key)&&profiles_.size()>=cfg_.max_profiles){profiles_.erase(profiles_.begin());++forgotten_;}
    auto& profile=profiles_[profile_key];
    const bool same=profile.initialized&&profile.run==c.run_id&&profile.session==c.radio_session;
    if(same&&c.device_time_ns<profile.end){++excluded_;out.usable=false;profile={};return out;}
    if(!same||((long double)c.device_time_ns-profile.end)/1e9L>cfg_.max_gap_s)profile={};
    profile.initialized=true;profile.run=c.run_id;profile.session=c.radio_session;profile.end=int64_t(end);
    for(auto& v:p.frames) {
        ++evaluated_;
        if(v.reset) {for(auto k=profile.links.begin();k!=profile.links.end();) {
            if(k->first.rfind(v.link+"/",0)==0)k=profile.links.erase(k);else ++k;}continue;}
        const auto key=v.link+"/"+v.kind;
        if(!profile.links.count(key)&&profile.links.size()>=cfg_.max_links){profile.links.erase(profile.links.begin());++forgotten_;}
        auto& link=profile.links[key];auto& history=link.history;
        const auto now=*v.event.device_time_ns;
        while(!history.empty()&&((long double)now-*history.front().request.event.device_time_ns)/1e9L>cfg_.retention_s){history.pop_front();++forgotten_;}
        if(history.empty())link.nonce_known=false;
        if(v.eapol && v.request) {
            if(v.stage==1 && (!link.nonce_known||link.nonce!=v.nonce)) {history.clear();link.nonce=v.nonce;link.nonce_known=true;}
            if(!link.nonce_known||link.nonce!=v.nonce) {history.clear();link.nonce_known=false;continue;}
            if(v.stage==3 && (history.empty()||history.front().request.stage!=1||!history.front().response))continue;
        }
        if(!v.request) {
            if(history.empty())continue;
            auto& last=history.back();
            if(v.eapol && (v.counter!=last.request.counter||v.stage!=last.request.stage+1))continue;
            if(!last.response)last.response=v;
            continue;
        }
        auto old=std::find_if(history.begin(),history.end(),[&](const Exchange& x){return x.request.bytes==v.bytes;});
        if(old!=history.end()) {
            const auto& latest=history.back();
            const double delay=double(((long double)now-*old->request.event.device_time_ns)/1e9L);
            if(&*old!=&latest && old->response && latest.response && !v.retry && delay>=cfg_.min_delay_s &&
               now>*latest.response->event.device_time_ns && (!v.eapol || (latest.request.counter>v.counter && latest.request.stage>=v.stage))) {
                IncidentObservation o;o.rule=v.eapol?"eapol_handshake_replay_anomaly":"management_transaction_replay_anomaly";
                o.rule_version="1";
                auto config=diagnostics();
                for(const char* key:{"evaluated","excluded_captures","history_evictions","unsupported"})config.erase(key);
                o.config_version=config.dump();
                o.severity=IncidentSeverity::Medium;o.confidence=IncidentConfidence::Low;
                o.band=c.band;o.channel=c.channel;o.claimed_bssid=v.ap;
                o.claimed_source=v.eapol?v.ap:v.client;
                o.target=v.client;o.baseline_key=profile_key;o.host_ns=v.evidence.host_ns;o.device_ns=now;
                o.mode="observation";o.tier="suspected";o.path=v.kind+"_historical_request_after_answered_exchange";
                o.context={{"transaction",v.kind},{"original_counter",std::to_string(old->request.counter)},
                           {"newer_counter",std::to_string(latest.request.counter)},{"session_authentication","unverified"}};
                o.measures={{"request_age_s",delay},{"minimum_age_s",cfg_.min_delay_s}};
                o.confidence_basis={"Exact historical request match", "Responses observed for original and newer exchange", "Ordered device-timed healthy capture context"};
                o.coverage_note="Replay-like observation only. Responses are not authenticated acceptance; no MIC verification, key installation or client impact established.";
                o.benign_alternatives={"Delayed retransmission or reordered traffic", "Reconnect or firmware restart reusing state", "Missed session transition or spoofed response"};
                auto evidence=[&](const Frame& f,const char* role){auto e=f.evidence;e.role=role;return e;};
                o.evidence=evidence(old->request,"original_request");
                o.extra_evidence={evidence(*old->response,"original_response"),evidence(latest.request,"newer_request"),
                    evidence(*latest.response,"newer_response"),evidence(v,"historical_request_reobserved")};
                out.observed_incidents.insert(incidents.observe(o));
            }
            continue; // ordinary retries do not refresh original timing or advance state
        }
        if(v.retry)continue;
        if(!history.empty()) {
            const auto last=history.back().request.counter;
            if(v.eapol ? v.counter<=last : ((v.counter-last)&4095)==0 || ((v.counter-last)&4095)>=2048)history.clear();
        }
        // A fresh management connection attempt invalidates visible handshake
        // context for this pair; no inference across observed reconnects.
        if(!v.eapol) {
            for(const char* other:{"eapol","authentication","association","reassociation"})
                if(v.kind!=other)profile.links.erase(v.link+"/"+other);
        }
        if(history.size()>=cfg_.history){history.pop_front();++forgotten_;}
        history.push_back({std::move(v),std::nullopt});
    }
    return out;
}
}
