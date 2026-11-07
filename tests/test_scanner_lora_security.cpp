// Production scanner with a fake receive device and real archived SX1262 IQ.
// No USRP/ESP32 is connected or constructed. All writes stay in a temporary root.
#include "scanner.hpp"
#include "lora_capture.hpp"
#include "channelize.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <cctype>
#include <kissfft/kiss_fft.h>
#include <chrono>
#include <set>
#include <filesystem>
#include <iostream>
#include <unistd.h>
namespace fs=std::filesystem;
using namespace rfmon;
class FixtureRadio:public CaptureDevice {
public:
    std::vector<std::complex<float>> iq;
    CaptureResult capture_detailed(double,double rate,double,double,bool)override {
        CaptureResult r;r.samples=iq;r.sample_rate_hz=rate;
        r.timing.device_time_valid=true;r.timing.device_time_ns=5000000000;
        r.timing.host_before_ns=1700000000000000000;r.timing.host_after_ns=r.timing.host_before_ns+100000;
        r.timing.requested_samples=iq.size();r.timing.gain_db=20;
        return r;
    }
    void set_gain(std::optional<double>)override{}
};
namespace rfmon {
struct ScannerTestAccess {
    static void listen(Scanner& s,std::unique_ptr<CaptureDevice> device) {
        s.sdr_=std::move(device);s.radio_session_=1;s.lora_security_->start();
        std::vector<Detection> detections;
        s.run_lora_listen_step(888e6,device_profile(SdrDeviceType::X310),DEFAULT_DETECTION_THRESHOLD_DB,detections);
        s.lora_security_->stop();
    }
    static void wideband(Scanner& s,std::unique_ptr<CaptureDevice> device,const lora_plan::Capture& plan) {
        s.sdr_=std::move(device);s.radio_session_=1;s.lora_security_->start();
        std::vector<Detection> detections;
        s.run_lora_wideband_step(plan,device_profile(SdrDeviceType::X310),DEFAULT_DETECTION_THRESHOLD_DB,detections);
        s.lora_security_->stop();
    }
};
}
void check(bool b,const char* msg){if(!b)throw std::runtime_error(msg);std::cout<<"PASS "<<msg<<'\n';}
int main(){char tmp[]="/tmp/rfmon-scanner-lora-XXXXXX";if(!mkdtemp(tmp))return 1;
 const auto cwd=fs::current_path();
 try{
    auto fixture=load_lora_capture(std::string(PROJECT_ROOT_DIR)+"/tests/fixtures/lora_sx1262/capture-1790061256825496286-0");
    fs::current_path(tmp);
    {
        Scanner scanner(tmp);auto fake=std::make_unique<FixtureRadio>();fake->iq=fixture.iq;
        scanner.request_lora_capture_save(std::string(tmp)+"/captures");ScannerTestAccess::listen(scanner,std::move(fake));
        auto snapshot=scanner.lora_security_snapshot();
        check(snapshot->captures==1&&snapshot->device_timed==1&&snapshot->eligible>=1&&snapshot->excluded==0,
            "Production LoRa scanner submits decoded evidence and coverage to consumer");
        const auto display=scanner.lora_packets();
        check(!display.empty()&&!scanner.lora_capture_pending(),"Existing LoRa GUI rows and requested capture save still work");
        bool tagged=false;
        for(const auto& row:display)if(row.header_valid) {
            check(row.decoder_data_start.has_value() && row.reception_id.find("/session-1/capture-1/reception-")!=std::string::npos && row.reception_hypotheses>=1,
                  "Production scanner publishes capture-scoped grouping without replacing hypothesis rows");
            tagged=true;
        }
        check(tagged,"Live grouping fixture must contain a located decoded row");
        for(const auto& directory:fs::directory_iterator(std::string(tmp)+"/captures")) {
            auto saved=load_lora_capture(directory.path().string());
            check(saved.timing&&saved.timing->device_time_ns==5000000000&&saved.capture_seq==snapshot->latest.capture_seq,
                "Scanner IQ manifest preserves the consumer capture identity/anchor");
            auto rows=analyze_lora_capture(saved.iq,saved.sample_rate_hz,saved.requested_center_hz);
            lora_security::State replay;replay.ingest(lora_security::make_batch(saved,saved.iq.size(),rows));
            check(lora_security::snapshot_json(replay.snapshot())==lora_security::snapshot_json(*snapshot),
                "Scanner -> saved IQ -> offline decode produces identical security evidence");
        }
        check(fs::file_size(std::string(tmp)+"/lora_security/events.ndjson")>0,"Live LoRa capture evidence persists to bounded recording");
    }
    {
        // Adjacent channel 200 kHz away (EU868 spacing): the packet must not fold
        // into this channel's BW125 hypothesis (live: 866.9 MHz seen on 867.1 MHz
        // at +50 kHz). On its own channel it still decodes.
        // analyze_lora_capture shares the scanner's decimator (lora_decimator.hpp).
        const double rate=fixture.sample_rate_hz;
        auto shifted=[&](double hz){
            auto out=fixture.iq;
            for(size_t i=0;i<out.size();++i){const double c=hz*double(i)/rate;out[i]*=std::polar(1.0f,float(2*M_PI*(c-std::floor(c))));}
            return out;};
        auto valid=[](const std::vector<LoraPacketRow>& rows){
            for(const auto& r:rows)if(r.crc==LoraPacketRow::Crc::Valid&&r.payload_repr&&r.payload_repr->find("888MHz")!=std::string::npos)return true;
            return false;};
        check(rate==500e3,"SX1262 fixture is a 500 kHz capture");
        check(valid(analyze_lora_capture(fixture.iq,rate,888e6)),"Packet decodes on its own channel");
        // Sub-chip timing: every quarter-chip sampling phase and crystal-scale
        // CFO of all three recorded packets must decode CRC-valid (phase 0
        // alone: 44 of 60; the retry at the other phases recovers the rest).
        {
            const auto expected=nlohmann::json::parse(std::ifstream(std::string(PROJECT_ROOT_DIR)+"/tests/fixtures/lora_sx1262/expected.json"));
            int ok=0,cases=0,recovered=0;
            for(const auto& e:expected){
                const auto cap=load_lora_capture(std::string(PROJECT_ROOT_DIR)+"/tests/fixtures/lora_sx1262/"+e["directory"].get<std::string>());
                for(double cfo:{0.0,-20e3,20e3})for(int phase=0;phase<4;++phase){
                    std::vector<std::complex<float>> x(cap.iq.begin()+phase,cap.iq.end());
                    for(size_t i=0;i<x.size();++i){const double c=cfo*double(i)/rate;x[i]*=std::polar(1.0f,float(2*M_PI*(c-std::floor(c))));}
                    bool good=false;
                    for(const auto& r:analyze_lora_capture(x,rate,888e6))
                        if(r.crc==LoraPacketRow::Crc::Valid&&r.bandwidth_khz==125&&r.sf==7){
                            std::string hex;for(char ch:r.payload_hex)if(ch!=' ')hex+=char(std::tolower(ch));
                            if(hex==e["payload_hex"].get<std::string>()){good=true;recovered+=r.detail.find("sub-chip sampling phase")!=std::string::npos;}
                        }
                    ok+=good;++cases;
                }
            }
            std::cout<<"INFO sub-chip phases: "<<ok<<"/"<<cases<<" CRC-valid ("<<recovered<<" recovered by the phase retry)\n";
            check(ok==cases,"Every sub-chip sampling phase and CFO of the recorded packets decodes CRC-valid");
            check(recovered>0,"The phase retry is exercised by the recorded packets");
        }
        for(double off:{-200e3,200e3}) {
            size_t bw125=0;
            for(const auto& r:analyze_lora_capture(shifted(off),rate,888e6))if(r.header_valid&&r.bandwidth_khz==125)++bw125;
            std::cout<<"INFO packet at "<<off/1e3<<" kHz: "<<bw125<<" BW125 decoded row(s)\n";
            check(bw125==0,"A packet 200 kHz away does not alias into this channel's BW125 decode");
        }
    }
    {
        // RF identity: one transmitter must stay one RF cluster. The three
        // recorded packets of one SX1262, each at all four sub-chip sampling
        // phases, go through the production listen path; every decoded packet
        // is fingerprinted from the decoder's alignment and all land in one
        // cluster (the burst-search fingerprint split them by arrival time).
        const std::string root=std::string(tmp)+"/identity";fs::create_directories(root);fs::current_path(root);
        Scanner scanner(root);
        int tagged=0,decoded=0;
        for(const auto& entry:fs::directory_iterator(std::string(PROJECT_ROOT_DIR)+"/tests/fixtures/lora_sx1262"))if(entry.is_directory()){
            const auto cap=load_lora_capture(entry.path().string());
            for(int phase=0;phase<4;++phase){
                auto fake=std::make_unique<FixtureRadio>();fake->iq.assign(cap.iq.begin()+phase,cap.iq.end());
                ScannerTestAccess::listen(scanner,std::move(fake));
            }
        }
        std::set<int> clusters;
        for(const auto& r:scanner.lora_packets())if(r.crc==LoraPacketRow::Crc::Valid){++decoded;if(r.rf_cluster_id){++tagged;clusters.insert(*r.rf_cluster_id);}}
        const auto rows=scanner.lora_master_snapshot();
        std::cout<<"INFO identity: "<<decoded<<" CRC-valid packets, "<<tagged<<" fingerprinted, "<<rows.size()<<" RF cluster(s)\n";
        check(decoded==12&&tagged>=10,"Every decoded packet of the recorded transmitter is fingerprinted");
        check(rows.size()==1&&clusters.size()==1,"One transmitter at every sub-chip phase forms one RF cluster");
        fs::current_path(tmp);
    }
    {
        // Wideband listen: the 888.0 MHz SX1262 packet placed inside a 3-channel
        // capture is derived and decoded on its own channel only.
        const std::string root=std::string(tmp)+"/wideband";fs::create_directories(root);fs::current_path(root);
        Scanner scanner(root);
        scanner.set_lora_channels({887.6e6,888.0e6,888.9e6});
        const auto profile=device_profile(SdrDeviceType::X310);
        const auto rotation=lora_plan::plan(scanner.lora_channels(),profile.lora_listen_capture_rate_hz,
                                            std::min(profile.max_sample_rate_hz,30e6/scanner.lora_capture_seconds()),true);
        check(rotation.size()==1&&rotation[0].wideband&&rotation[0].channels_hz.size()==3,"Three nearby channels plan as one wideband capture");
        const auto& plan=rotation[0];
        const size_t up=size_t(std::llround(plan.rate_hz/500e3));
        const std::vector<std::complex<float>>& narrow=fixture.iq; // 0.16 s cropped SX1262 packet
        std::vector<std::complex<float>> stuffed(narrow.size()*up);
        for(size_t i=0;i<narrow.size();++i)stuffed[i*up]=narrow[i]*float(up);
        const auto taps=blackman_lowpass(0.5*500e3/plan.rate_hz,0.04*500e3/plan.rate_hz);
        std::vector<std::complex<float>> wide(stuffed.size());
        const size_t half=taps.size()/2;
        for(size_t n=0;n<wide.size();++n){
            std::complex<float> acc;
            for(size_t k=0;k<taps.size();++k){const long m=long(n)+long(half)-long(k);if(m>=0&&size_t(m)<stuffed.size())acc+=stuffed[size_t(m)]*taps[k];}
            const double ph=2*M_PI*(888.0e6-plan.center_hz)*double(n)/plan.rate_hz;
            wide[n]=acc*std::complex<float>(float(std::cos(ph)),float(std::sin(ph)));
        }
        auto fake=std::make_unique<FixtureRadio>();fake->iq=std::move(wide);
        scanner.request_lora_capture_save(root+"/captures");
        const auto t0=std::chrono::steady_clock::now();
        ScannerTestAccess::wideband(scanner,std::move(fake),plan);
        const double took=std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count();
        bool on_channel=false,off_channel=false;
        if(std::getenv("LORA_TEST_VERBOSE"))for(const auto& row:scanner.lora_packets())
            std::cout<<"ROW "<<row.freq_mhz<<" SF"<<row.sf<<" BW"<<row.bandwidth_khz<<" "<<row.status<<" crc="<<lora_crc_label(row.crc)<<" "<<row.payload_repr.value_or("")<<"\n";
        for(const auto& row:scanner.lora_packets()) if(row.crc==LoraPacketRow::Crc::Valid) {
            if(std::abs(row.freq_mhz-888.0)<1e-6&&row.payload_repr&&row.payload_repr->find("888MHz")!=std::string::npos)on_channel=true;
            else off_channel=true;
        }
        check(on_channel,"Wideband capture decodes the packet on its derived channel");
        check(!off_channel,"Adjacent derived channels do not report the packet");
        const auto stats=scanner.lora_listen_stats();
        check(stats.captures==1&&stats.last_wideband&&stats.last_channels==3&&stats.channel_listened_s.size()==3,
              "Listen statistics credit every channel of the wideband capture");
        check(scanner.lora_security_snapshot()->captures==3,"Each derived channel is its own security coverage record");
        check(scanner.lora_security_snapshot()->excluded==0&&scanner.lora_security_snapshot()->time_regressions==0,
              "Derived channels sharing one capture start are usable evidence, not clock regressions");
        size_t saved=0;
        for(const auto& d:fs::directory_iterator(root+"/captures")){auto c=load_lora_capture(d.path().string());saved+=c.sample_rate_hz==500e3;}
        check(saved==3,"Capture save writes one replayable 500 kHz capture per channel");
        std::cout<<"INFO wideband step (3 channels, "<<plan.rate_hz/1e6<<" Msps, 0.16 s IQ) took "<<took<<" s\n";
        fs::current_path(tmp);
    }
    {
        // Default 20 MHz listening: the packet is FFT-upsampled into a 20 Msps
        // capture covering the planned channels, then decoded on its channel.
        const std::string root=std::string(tmp)+"/wideband20";fs::create_directories(root);fs::current_path(root);
        Scanner scanner(root);
        // 13 channels, as in the broad IN865 + EU868 preset: one evidence batch each.
        scanner.set_lora_channels({883e6,884e6,885e6,886e6,887.6e6,888.0e6,888.9e6,889.5e6,890e6,891e6,892e6,893e6,894e6});
        check(scanner.lora_wideband_rate()==std::optional<double>(20e6),"LoRa wideband listening defaults to 20 MHz");
        const auto rotation=scanner.lora_listen_plan();
        check(rotation.size()==1&&rotation[0].wideband&&rotation[0].rate_hz==20e6&&rotation[0].channels_hz.size()==13,
              "Planned channels share one 20 MHz capture");
        const auto& plan=rotation[0];
        const size_t n=80000,up=40,big=n*up;
        std::vector<kiss_fft_cpx> a(n),A(n),B(big),b(big);
        for(size_t i=0;i<n;++i){a[i].r=fixture.iq[i].real();a[i].i=fixture.iq[i].imag();}
        kiss_fft_cfg f1=kiss_fft_alloc(int(n),0,nullptr,nullptr),f2=kiss_fft_alloc(int(big),1,nullptr,nullptr);
        kiss_fft(f1,a.data(),A.data());
        const long shift=std::lround((888.0e6-plan.center_hz)/(20e6/double(big)));
        for(auto& v:B)v.r=v.i=0;
        for(long k=-long(n/2);k<long(n/2);++k){const auto& v=A[size_t((k+long(n))%long(n))];auto& w=B[size_t(((k+shift)%long(big)+long(big))%long(big))];w.r=v.r/float(n);w.i=v.i/float(n);}
        kiss_fft(f2,B.data(),b.data());kiss_fft_free(f1);kiss_fft_free(f2);
        auto fake=std::make_unique<FixtureRadio>();fake->iq.resize(big);
        for(size_t i=0;i<big;++i)fake->iq[i]={b[i].r,b[i].i};
        const auto t0=std::chrono::steady_clock::now();
        ScannerTestAccess::wideband(scanner,std::move(fake),plan);
        const double took=std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count();
        bool on_channel=false,off_channel=false;
        for(const auto& row:scanner.lora_packets()) if(row.crc==LoraPacketRow::Crc::Valid) {
            if(std::abs(row.freq_mhz-888.0)<1e-6&&row.payload_repr&&row.payload_repr->find("888MHz")!=std::string::npos)on_channel=true;
            else off_channel=true;
        }
        check(on_channel&&!off_channel,"20 MHz capture decodes the packet on its channel only");
        const auto evidence=scanner.lora_security_snapshot();
        check(evidence->captures==13&&evidence->excluded==0&&evidence->input_loss==0,
              "All 13 derived channels reach the evidence monitor as usable evidence");
        std::cout<<"INFO 20 Msps wideband step (13 channels, 0.16 s IQ) took "<<took<<" s\n";
        fs::current_path(tmp);
    }
    fs::current_path(cwd);fs::remove_all(tmp);return 0;
 }catch(const std::exception& e){fs::current_path(cwd);std::cerr<<"FAIL "<<e.what()<<" ("<<tmp<<")\n";return 1;}}
