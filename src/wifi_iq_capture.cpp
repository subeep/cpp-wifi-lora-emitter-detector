#include "wifi_iq_capture.hpp"
#include "capture_timing_json.hpp"
#include <filesystem>
#include <fstream>
#include <cstring>
#include <limits>
namespace rfmon {
namespace {
namespace fs=std::filesystem;
constexpr size_t max_samples=64000000;
void require(bool b,const char* message) {if(!b)throw std::runtime_error(message);}
size_t validate(const nlohmann::json& j) {
    require((j.at("schema")==1 || j.at("schema")==2) && j.at("format")=="cf32_le","Unsupported Wi-Fi IQ schema/format");
    size_t n=capture_sample_count(j.at("samples"));require(n && n<=max_samples,"Wi-Fi IQ sample count outside limits");
    double rate=j.at("sample_rate_hz"),center=j.at("capture_center_hz"),channel=j.at("channel_hz");
    require(std::isfinite(rate)&&rate>=1e6&&rate<=64e6&&std::isfinite(center)&&center>=2.4e9&&center<=6e9&&
        std::isfinite(channel)&&channel>=2.4e9&&channel<=6e9,"Invalid Wi-Fi IQ frequency/rate");
    fs::path file=j.at("iq_file").get<std::string>();
    require(!file.empty()&&!file.has_parent_path()&&file!="."&&file!="..","IQ filename must be local");
    if(j.at("schema")==2) {
        require(j.at("run_id").is_string()&&!j.at("run_id").get<std::string>().empty()&&
            j.at("run_id").get<std::string>().size()<=256,"Invalid capture run identity");
        capture_sample_count(j.at("radio_session"));capture_sample_count(j.at("capture_seq"));
        auto t=capture_timing_from_json(j.at("timing"),n,rate);
        require(j.at("overflow").get<bool>()==!t.overflows.empty(),"Overflow flag disagrees with timing");
    }
    return n;
}
float unpack(const unsigned char* bytes) {
    uint32_t u=0;for(int b=0;b<4;++b)u|=uint32_t(bytes[b])<<(8*b);
    float f;std::memcpy(&f,&u,4);return f;
}
void pack(float f,unsigned char* bytes) {
    static_assert(sizeof(float)==4 && std::numeric_limits<float>::is_iec559);
    uint32_t u;std::memcpy(&u,&f,4);for(int b=0;b<4;++b)bytes[b]=uint8_t(u>>(8*b));
}
}
WifiIqCapture load_wifi_iq_capture(const std::string& manifest) {
    fs::path path(manifest);require(fs::file_size(path)<=1048576,"Wi-Fi IQ manifest too large");
    WifiIqCapture c;std::ifstream in(path);in.exceptions(std::ios::badbit);in>>c.metadata;
    const size_t n=validate(c.metadata);const auto file=path.parent_path()/c.metadata.at("iq_file").get<std::string>();
    require(fs::file_size(file)==n*8,"IQ file length mismatch");
    if(c.metadata.at("schema")==2)c.timing=capture_timing_from_json(c.metadata.at("timing"),n,c.metadata.at("sample_rate_hz").get<double>());
    std::ifstream raw(file,std::ios::binary);raw.exceptions(std::ios::badbit|std::ios::failbit);
    c.iq.resize(n);uint64_t hash=14695981039346656037ull;
    // Bulk reads avoid per-byte stream overhead on full captures.
    std::vector<unsigned char> bytes(std::min(n,size_t(8192))*8);
    for(size_t begin=0;begin<n;) {
        size_t count=std::min(n-begin,bytes.size()/8);raw.read(reinterpret_cast<char*>(bytes.data()),count*8);
        for(size_t b=0;b<count*8;++b)hash=(hash^bytes[b])*1099511628211ull;
        for(size_t i=0;i<count;++i) {
            float re=unpack(bytes.data()+i*8),im=unpack(bytes.data()+i*8+4);
            require(std::isfinite(re)&&std::isfinite(im),"Non-finite IQ sample");c.iq[begin+i]={re,im};
        }
        begin+=count;
    }
    require(c.metadata.at("fnv1a64")==std::to_string(hash),"IQ checksum mismatch");return c;
}
void save_wifi_iq_capture(const std::string& manifest,nlohmann::json j,
                         const std::vector<std::complex<float>>& iq,const CaptureTiming& t) {
    j["schema"]=2;j["format"]="cf32_le";j["samples"]=iq.size();j["timing"]=capture_timing_json(t);
    j["overflow"]=!t.overflows.empty();validate(j);
    for(auto x:iq)require(std::isfinite(x.real())&&std::isfinite(x.imag()),"Non-finite IQ sample");
    fs::path path(manifest),file=path.parent_path()/j.at("iq_file").get<std::string>(),tmp=path.string()+".tmp";
    require(path!=file&&tmp!=file,"Manifest and IQ paths collide");
    require(!fs::exists(path)&&!fs::exists(tmp)&&!fs::exists(file),"Capture output already exists");
    try {
        std::ofstream raw(file,std::ios::binary);raw.exceptions(std::ios::badbit|std::ios::failbit);
        uint64_t hash=14695981039346656037ull;
        for(auto x:iq) {
            unsigned char bytes[8];pack(x.real(),bytes);pack(x.imag(),bytes+4);
            for(auto b:bytes)hash=(hash^b)*1099511628211ull;
            raw.write(reinterpret_cast<char*>(bytes),8);
        }
        raw.close();j["fnv1a64"]=std::to_string(hash);
        std::ofstream out(tmp);out.exceptions(std::ios::badbit|std::ios::failbit);out<<j.dump(2)<<'\n';out.close();fs::rename(tmp,path);
    } catch(...) {std::error_code ec;fs::remove(file,ec);fs::remove(tmp,ec);throw;}
}
} // namespace rfmon
