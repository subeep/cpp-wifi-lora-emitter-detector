// Production scanner with a fake receive device and real archived SX1262 IQ.
// No USRP/ESP32 is connected or constructed. All writes stay in a temporary root.
#include "scanner.hpp"
#include "lora_capture.hpp"
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
        check(!scanner.lora_packets().empty()&&!scanner.lora_capture_pending(),"Existing LoRa GUI rows and requested capture save still work");
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
    fs::current_path(cwd);fs::remove_all(tmp);return 0;
 }catch(const std::exception& e){fs::current_path(cwd);std::cerr<<"FAIL "<<e.what()<<" ("<<tmp<<")\n";return 1;}}
