#include "remote_id.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <sstream>
#include <iomanip>

namespace drone {
namespace {
using J=RidJson;
using Bytes=std::vector<uint8_t>;
const char* names[]={"Basic ID","Location","Authentication","Self ID","System","Operator ID"};
uint16_t u16(const uint8_t* p){return uint16_t(p[0])|(uint16_t(p[1])<<8);}
uint32_t u32(const uint8_t* p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
int32_t i32(const uint8_t* p){uint32_t u=u32(p);int32_t v;std::memcpy(&v,&u,4);return v;}
std::string hex(const uint8_t* p,size_t n){static const char d[]="0123456789abcdef";std::string s;for(size_t i=0;i<n;++i){s+=d[p[i]>>4];s+=d[p[i]&15];}return s;}
J text(const uint8_t* p,size_t n){std::string s;for(size_t i=0;i<n&&p[i];++i){if(p[i]<32||p[i]>=127)return nullptr;s+=char(p[i]);}return s;}
J alt(const uint8_t* p){auto v=u16(p);return v?J(v*.5-1000):J(nullptr);}
void position(J& m,const uint8_t* p){double lat=i32(p)/1e7,lon=i32(p+4)/1e7;bool ok=lat>=-90&&lat<=90&&lon>=-180&&lon<=180&&(lat!=0||lon!=0);m["latitude"]=ok?J(lat):J(nullptr);m["longitude"]=ok?J(lon):J(nullptr);m["position_status"]=ok?"Reported":"Unavailable / invalid";m["latitude_raw"]=i32(p);m["longitude_raw"]=i32(p+4);}
std::string category(unsigned n,std::initializer_list<const char*> labels){return n<labels.size()?*(labels.begin()+n):"Reserved / unknown";}
J accuracy(unsigned n,std::initializer_list<double> bounds){return n>0&&n<bounds.size()?J(*(bounds.begin()+n)):J(nullptr);}
J message(const uint8_t* r){
    unsigned kind=r[0]>>4,v=r[0]&15;if(v>2||kind>5)return nullptr;
    J m={{"type",names[kind]},{"protocol_version",v},{"raw_hex",hex(r,25)}};
    if(kind==0){
        unsigned id=r[1]>>4,ua=r[1]&15;if(id<1||id>(v==0?3:4))return nullptr;
        m.update(J{{"id_type",id},{"id_type_name",category(id,{"Unspecified","Serial number","CAA registration","UTM UUID","Session ID"})},{"ua_type",ua},{"ua_type_name",category(ua,{"Unspecified","Aeroplane","Helicopter / multirotor","Gyroplane","Hybrid lift","Ornithopter","Glider","Kite","Free balloon","Captive balloon","Airship","Free fall parachute","Rocket","Tethered powered aircraft","Ground obstacle","Other"})},{"reported_id_hex",hex(r+2,20)}});
        if(id<=2){auto t=text(r+2,20);if(t.is_null()||t=="")return nullptr;m["reported_id"]=t;}
    }else if(kind==1){
        position(m,r+5);unsigned direction=r[2]+((r[1]&2)?180:0),stamp=u16(r+21);double sh=r[3]*((r[1]&1)?.75:.25)+((r[1]&1)?63.75:0);double sv=(r[4]<128?int(r[4]):int(r[4])-256)*.5;
        m.update(J{{"status",r[1]>>4},{"direction_deg",direction<=360?J(direction):J(nullptr)},{"speed_horizontal_m_s",sh<=254.25?J(sh):J(nullptr)},{"speed_vertical_m_s",sv>=-62&&sv<=62?J(sv):J(nullptr)},{"altitude_baro_m",alt(r+13)},{"altitude_geo_m",alt(r+15)},{"height_m",alt(r+17)},{"height_reference",(r[1]&4)?"Ground":"Takeoff"},{"horizontal_accuracy_code",r[19]&15},{"vertical_accuracy_code",r[19]>>4},{"speed_accuracy_code",r[20]&15},{"baro_accuracy_code",r[20]>>4},{"timestamp_seconds_past_hour",stamp<=36000?J(stamp/10.):J(nullptr)},{"timestamp_accuracy_code",r[23]&15}});
        unsigned status=r[1]>>4;
        m["status_name"]=v==2?category(status,{"Undeclared","On ground","Airborne","Emergency","Remote ID system failure"}):category(status,{"Undeclared","On ground","Airborne","Emergency"});
        m["horizontal_accuracy_upper_bound_m"]=accuracy(r[19]&15,{0,18520,7408,3704,1852,926,555.6,185.2,92.6,30,10,3,1});
        m["vertical_accuracy_upper_bound_m"]=accuracy(r[19]>>4,{0,150,45,25,10,3,1});m["baro_accuracy_upper_bound_m"]=accuracy(r[20]>>4,{0,150,45,25,10,3,1});m["speed_accuracy_upper_bound_m_s"]=accuracy(r[20]&15,{0,10,3,1,.3});m["timestamp_accuracy_s"]=(r[23]&15)?J((r[23]&15)/10.):J(nullptr);
    }else if(kind==2){
        unsigned page=r[1]&15;if(v==0&&page>=5)return nullptr;
        m.update(J{{"auth_type",r[1]>>4},{"data_page",page},{"signature_verified",false}});
        if(page==0){
            if(v==0){if(r[2]>5||r[3]>109)return nullptr;m["page_count"]=r[2];m["last_page_index"]=nullptr;m["page_metadata_status"]="Legacy PageCount; not a last-page index";}
            else{if(r[2]>15||r[3]>17+23*r[2])return nullptr;m["last_page_index"]=r[2];}
            m.update(J{{"length",r[3]},{"timestamp_2019_s",u32(r+4)},{"auth_data_hex",hex(r+8,17)}});
        }else m["auth_data_hex"]=hex(r+2,23);
    }else if(kind==3){
        m.update(J{{"description_type",r[1]},{"description",text(r+2,23)},{"description_hex",hex(r+2,23)}});
        m["description_type_name"]=v==2?category(r[1],{"General text","Emergency details","Extended status"}):category(r[1],{"General text"});
    }else if(kind==4){
        position(m,r+2);unsigned loc=r[1]&3,cls=(r[1]>>2)&7,cat=r[17]>>4;
        m.update(J{{"operator_location_type",loc},{"classification_type",cls},{"area_count",u16(r+10)},{"area_radius_m",r[12]*10},{"area_ceiling_m",alt(r+13)},{"area_floor_m",alt(r+15)},{"category_eu",cat},{"class_eu",r[17]&15},{"operator_altitude_geo_m",v>=1?alt(r+18):J(nullptr)},{"timestamp_2019_s",v==2?J(u32(r+20)):J(nullptr)}});
        m["operator_altitude_geo_status"]=v==0?"Not carried by protocol v0":m["operator_altitude_geo_m"].is_null()?"Unavailable":"Reported";
        m["timestamp_status"]=v==2?"Reported":"Not carried by protocol v"+std::to_string(v);
        unsigned at=v==0?18:v==1?20:24;m["version_reserved_raw_hex"]=hex(r+at,25-at);
        m["operator_location_type_name"]=category(loc,{"Takeoff location","Live GNSS location","Fixed location"});m["classification_type_name"]=category(cls,{"Undeclared","EU classification"});m["category_eu_name"]=cls==1?category(cat,{"Undeclared","Open","Specific","Certified"}):"Not applicable / unknown";
    }else m.update(J{{"operator_id_type",r[1]},{"operator_id",text(r+2,20)},{"operator_id_hex",hex(r+2,20)}});
    return m;
}
J pack(const uint8_t* p,size_t n,const char* transport,unsigned counter){
    if(n<3||(p[0]>>4)!=15||(p[0]&15)>2||p[1]!=25)return nullptr;
    unsigned v=p[0]&15,count=p[2];if(count<1||count>(v==0?10:9)||n!=3+count*25)return nullptr;
    J messages=J::array(),ids=J::array();unsigned counts[6]={};std::set<std::pair<int,int>> pages;std::map<int,std::string> identities;
    for(unsigned i=0;i<count;++i){const auto* r=p+3+25*i;J m=message(r);if(m.is_null()||(r[0]&15)!=v)return nullptr;unsigned k=r[0]>>4,limit=k==0?(v==0?1:2):k==2?(v==0?5:16):1;if(++counts[k]>limit)return nullptr;
        if(k==2&&!pages.emplace(r[1]>>4,r[1]&15).second)return nullptr;
        if(k==0){int id=r[1]>>4;auto value=m["reported_id_hex"].get<std::string>();if(identities.count(id)&&identities[id]!=value)return nullptr;identities[id]=value;ids.push_back(m);}messages.push_back(m);
    }
    return J{{"transport",transport},{"protocol_version",v},{"message_counter",counter},{"message_count",count},{"messages",messages},{"basic_ids",ids},{"authenticated",false}};
}
bool frame(const Bytes& b,uint8_t subtype){return b.size()>=28&&(u16(b.data())&255)==subtype&&!(u16(b.data())&0xc700)&&!(b[22]&15);}
bool match(const uint8_t* p,std::initializer_list<uint8_t> expected){return std::equal(expected.begin(),expected.end(),p);}
std::string address(const Bytes& b){std::string s;for(int i=10;i<16;++i){if(i>10)s+=':';s+=hex(&b[i],1);}return s;}
}
RidJson parse_wifi_remote_id(const Bytes& b){
    J found=J::array();if(b.size()<28||b.size()>8192)return found;
    uint32_t crc=0xffffffff;for(size_t i=0;i<b.size()-4;++i){crc^=b[i];for(int bit=0;bit<8;++bit)crc=(crc>>1)^((crc&1)?0xedb88320:0);}
    if((crc^0xffffffff)!=u32(b.data()+b.size()-4))return found;
    const size_t end=b.size()-4;
    if(frame(b,0x80)&&end>=36){
        for(size_t at=36;at<end;){if(at+2>end)return J::array();unsigned kind=b[at],n=b[at+1];at+=2;if(at+n>end)return J::array();const auto* p=b.data()+at;at+=n;
            if(kind==221&&n>=8&&match(p,{0xfa,0x0b,0xbc,0x0d})){auto v=pack(p+5,n-5,"Wi-Fi Beacon",p[4]);if(!v.is_null())found.push_back(v);}
        }
    }else if(frame(b,0xd0)&&end>=30&&match(b.data()+4,{0x51,0x6f,0x9a,1,0,0})&&match(b.data()+24,{4,9,0x50,0x6f,0x9a,0x13})){
        for(size_t at=30;at<end;){if(at+3>end)return J::array();unsigned kind=b[at],n=u16(b.data()+at+1);at+=3;if(at+n>end)return J::array();const auto* p=b.data()+at;at+=n;
            if(kind==3&&n>=14&&match(p,{0x88,0x69,0x19,0x9d,0x92,9})&&p[8]==0x10&&p[9]==n-10){auto v=pack(p+11,n-11,"Wi-Fi NAN",p[10]);if(!v.is_null())found.push_back(v);}
        }
    }
    for(auto& p:found)p["transmitter"]=address(b);
    return found;
}
RemoteIdMonitor::~RemoteIdMonitor(){enable(false);}
void RemoteIdMonitor::enable(bool value){
    enabled_=false;wake_.notify_all();if(thread_.joinable())thread_.join();
    {std::lock_guard<std::mutex> lock(mutex_);queue_.clear();if(value){tracks_.clear();examined_=accepted_=rejected_=errors_=out_of_order_=0;dropped_=0;}}
    if(value){enabled_=true;thread_=std::thread(&RemoteIdMonitor::run,this);}
}
void RemoteIdMonitor::submit(const rfmon::wifi_security::FrameEvent& f) noexcept{
    if(!enabled_||f.mpdu.size()<28||f.mpdu.size()>8192)return;
    // Only management Beacon/Action frames need examination; packet capture itself is unchanged.
    if((f.mpdu[0]&0xfc)!=0x80&&(f.mpdu[0]&0xfc)!=0xd0)return;
    try{std::unique_lock<std::mutex> lock(mutex_,std::try_to_lock);if(!lock||queue_.size()>=128){++dropped_;return;}queue_.push_back(f);wake_.notify_one();}catch(...){++dropped_;}
}
void RemoteIdMonitor::run(){
    while(enabled_){rfmon::wifi_security::FrameEvent f;{std::unique_lock<std::mutex> lock(mutex_);wake_.wait(lock,[&]{return !enabled_||!queue_.empty();});if(!enabled_)break;f=std::move(queue_.front());queue_.pop_front();}
        try{consume(f);}catch(...){std::lock_guard<std::mutex> lock(mutex_);++errors_;}
    }
}
void RemoteIdMonitor::consume(const rfmon::wifi_security::FrameEvent& f){
    auto packs=f.fcs_valid&&!f.mpdu_truncated?parse_wifi_remote_id(f.mpdu):J::array();
    const auto now=std::chrono::steady_clock::now();
    const auto wall=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const double age=f.host_time_ns?std::max(0.,(wall-*f.host_time_ns)*1e-9):0.;
    std::lock_guard<std::mutex> lock(mutex_);++examined_;if(packs.empty()){++rejected_;return;}
    for(auto& p:packs){
        const std::string key=f.run_id+"/"+std::to_string(f.radio_session)+"/"+p["transport"].get<std::string>()+"/"+p["transmitter"].get<std::string>();
        for(auto it=tracks_.begin();it!=tracks_.end();)if(std::chrono::duration<double>(now-it->second.seen).count()>300)it=tracks_.erase(it);else ++it;
        if(!tracks_.count(key)&&tracks_.size()>=64){auto oldest=std::min_element(tracks_.begin(),tracks_.end(),[](const auto& a,const auto& b){return a.second.seen<b.second.seen;});tracks_.erase(oldest);}
        Track t;auto previous=tracks_.find(key);if(previous!=tracks_.end())t=previous->second;
        if(f.host_time_ns&&t.host_ns>*f.host_time_ns){++out_of_order_;continue;}
        bool reset=!t.info.empty()&&t.info["protocol_version"]!=p["protocol_version"];
        for(const auto& m:p["basic_ids"]){auto id="Basic ID/"+std::to_string(m["id_type"].get<int>());auto old=t.messages.find(id);if(old!=t.messages.end()&&old->second.value["reported_id_hex"]!=m["reported_id_hex"])reset=true;}
        if(reset)t=Track{};
        t.seen=now;t.host_ns=f.host_time_ns.value_or(0);uint64_t count=(t.info.is_object()?t.info.value("packs",uint64_t(0)):0)+1;
        t.info={{"key",key},{"transport",p["transport"]},{"transmitter",p["transmitter"]},{"protocol_version",p["protocol_version"]},{"packs",count},{"message_counter",p["message_counter"]},{"authenticated",false},{"aircraft_confirmed",false}};
        J provenance={{"run_id",f.run_id},{"radio_session",f.radio_session},{"capture_seq",f.capture_seq},{"sample_start",f.sample_start},{"sample_length",f.sample_length},{"channel",f.channel},{"channel_hz",f.channel_hz},{"capture_center_hz",f.capture_center_hz},{"sample_rate_hz",f.sample_rate_hz},{"phy",f.phy},{"after_overflow",f.after_overflow},{"host_uncertainty_ns",f.host_uncertainty_ns},{"mpdu_hex",hex(f.mpdu.data(),f.mpdu.size())},{"fcs_rechecked",true},{"age_basis",f.host_time_ns?"Host acquisition estimate":"Decoder arrival only; acquisition age unknown"}};
        provenance["host_time_ns"]=f.host_time_ns?J(*f.host_time_ns):J(nullptr);provenance["device_time_ns"]=f.device_time_ns?J(*f.device_time_ns):J(nullptr);
        for(auto m:p["messages"]){std::string id=m["type"];if(id=="Basic ID")id+="/"+std::to_string(m["id_type"].get<int>());if(id=="Authentication")id+="/"+std::to_string(m["auth_type"].get<int>())+"/"+std::to_string(m["data_page"].get<int>());m["source"]=provenance;
            if(!t.messages.count(id)&&t.messages.size()>=32){auto oldest=std::min_element(t.messages.begin(),t.messages.end(),[](const auto& a,const auto& b){return a.second.received<b.second.received;});t.messages.erase(oldest);}
            t.messages[id]={m,now,age};}
        tracks_[key]=std::move(t);
        ++accepted_;
    }
}
RidJson RemoteIdMonitor::snapshot(){
    std::lock_guard<std::mutex> lock(mutex_);J j={{"enabled",enabled_.load()},{"examined",examined_},{"accepted",accepted_},{"not_decoded",rejected_},{"errors",errors_},{"out_of_order",out_of_order_},{"queue_depth",queue_.size()},{"dropped",dropped_.load()},{"tracks",J::array()}};
    auto now=std::chrono::steady_clock::now();for(const auto& item:tracks_){const auto& t=item.second;auto info=t.info;info["messages"]=J::array();bool fresh_id=false;for(const auto& pair:t.messages){auto m=pair.second.value;double age=pair.second.initial_age+std::chrono::duration<double>(now-pair.second.received).count();m["age_s"]=age;m["stale"]=age>10;if(m["type"]=="Basic ID"&&age<=10)fresh_id=true;info["messages"].push_back(m);}std::stable_sort(info["messages"].begin(),info["messages"].end(),[](const J& a,const J& b){
        auto rank=[](const J& m){const std::string type=m.at("type");const char* order[]={"Basic ID","Location","System","Self ID","Operator ID","Authentication"};for(int i=0;i<6;++i)if(type==order[i])return i;return 6;};return rank(a)<rank(b);
    });info["identity_status"]=fresh_id?"Reported ID recently received":"ID stale or unavailable";j["tracks"].push_back(info);}return j;
}
}
