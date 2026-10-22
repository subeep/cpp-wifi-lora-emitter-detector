#include "ml_inference.hpp"
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
namespace rfmon::ml {
namespace {
using Tree=boost::property_tree::ptree;
using Clock=std::chrono::steady_clock;
Tree parse(const std::string& text) { Tree t;std::istringstream in(text);boost::property_tree::read_json(in,t);return t; }
std::string encode(const Tree& t){std::ostringstream out;boost::property_tree::write_json(out,t,false);return out.str();}
struct Client {
    int fd=-1;pid_t pid=-1;
    const std::atomic<bool>& enabled;
    explicit Client(const std::atomic<bool>& e):enabled(e){}
    ~Client() {
        if(fd>=0)close(fd);
        if(pid>0) {
            pid_t r;do{r=waitpid(pid,nullptr,WNOHANG);}while(r<0&&errno==EINTR);
            if(r==0){kill(pid,SIGKILL);do{r=waitpid(pid,nullptr,0);}while(r<0&&errno==EINTR);}
        }
    }
    void start(const Options& o) {
        int sockets[2];if(socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,sockets))throw std::runtime_error("Cannot create ML channel");
        posix_spawn_file_actions_t actions;
        int init=posix_spawn_file_actions_init(&actions),error=init;
        if(!error)error=posix_spawn_file_actions_addclose(&actions,sockets[0]);
        if(!error)error=posix_spawn_file_actions_adddup2(&actions,sockets[1],3);
        if(!error)error=posix_spawn_file_actions_addclosefrom_np(&actions,4);
        for(int i=0;i<3&&!error;++i)error=posix_spawn_file_actions_addopen(&actions,i,"/dev/null",i?O_WRONLY:O_RDONLY,0);
        std::vector<std::string> args={o.python,"-u",o.script,"--ipc-fd","3","--model",o.model};
        if(!o.audit_root.empty()) {
            args.insert(args.end(),{"--audit-root",o.audit_root,"--audit-context",o.audit_context,"--audit-notes",o.audit_notes});
        }
        std::vector<char*> argv;for(auto& a:args)argv.push_back(a.data());argv.push_back(nullptr);
        if(!error)error=posix_spawn(&pid,o.python.c_str(),&actions,nullptr,argv.data(),environ);
        if(!init)posix_spawn_file_actions_destroy(&actions);
        close(sockets[1]);fd=sockets[0];
        if(error)throw std::runtime_error("Cannot start Python ML worker: "+std::string(strerror(error)));
        if(fcntl(fd,F_SETFL,O_NONBLOCK)<0)throw std::runtime_error("Cannot configure ML channel");
    }
    void transfer(void* data,size_t n,bool writing,Clock::time_point deadline) {
        auto* bytes=static_cast<unsigned char*>(data);
        while(n) {
            if(!enabled)throw std::runtime_error("Inference stopped");
            if(Clock::now()>deadline)throw std::runtime_error("ML worker timeout");
            pollfd p{fd,short(writing?POLLOUT:POLLIN),0};
            int r=poll(&p,1,50);
            if(r<0&&errno==EINTR)continue;
            if(r<0)throw std::runtime_error("ML poll failed");
            if(!r)continue;
            ssize_t size=writing?send(fd,bytes,n,MSG_NOSIGNAL):recv(fd,bytes,n,0);
            if(size<0&&(errno==EAGAIN||errno==EINTR))continue;
            if(size<=0)throw std::runtime_error("ML worker channel closed");
            bytes+=size;n-=size;
        }
    }
    void header(uint32_t& value,bool writing,Clock::time_point deadline) {
        unsigned char b[4];
        if(writing)for(int i=0;i<4;++i)b[i]=(value>>(8*i))&255;
        transfer(b,4,writing,deadline);
        if(!writing){value=0;for(int i=0;i<4;++i)value|=uint32_t(b[i])<<(8*i);}
    }
    std::string receive(Clock::time_point deadline) {
        uint32_t n=0;header(n,false,deadline);
        if(!n||n>262144)throw std::runtime_error("Invalid ML response size");
        std::string text(n,'\0');transfer(text.data(),n,false,deadline);return text;
    }
    void request(std::string text,std::complex<float>* iq,size_t samples,Clock::time_point deadline) {
        uint32_t n=text.size();if(!n||n>8192)throw std::runtime_error("Oversized capture metadata");
        header(n,true,deadline);transfer(text.data(),n,true,deadline);
        transfer(iq,samples*sizeof(iq[0]),true,deadline);
    }
};
std::string source_key(const wifi_security::CaptureRecord& c) {
    std::ostringstream s;s.precision(17);
    s<<c.run_id<<'/'<<c.radio_session<<'/'<<c.source<<'/'<<c.band<<'/'<<c.capture_center_hz<<'/'
     <<c.actual_rf_hz<<'/'<<c.actual_dsp_hz<<'/'<<c.sample_rate_hz;
    return s.str();
}
}

Inference::~Inference(){stop();if(thread_.joinable())thread_.join();}
bool Inference::start(Options options) {
    if(enabled_)return false;
    if(thread_.joinable())thread_.join();
    if(options.python.empty()||options.python[0]!='/'||options.script.empty()||options.script[0]!='/'||
       options.model.empty()||options.model[0]!='/'||options.deadline_ms<100||options.deadline_ms>60000||
       (!options.audit_root.empty()&&options.audit_root[0]!='/')||options.audit_notes.size()>1024) {
        std::lock_guard<std::mutex> l(mutex_);
        state_.state="Failed";state_.error="Use absolute model, Python, worker and audit paths, bounded notes and a valid deadline";
        return false;
    }
    {
        std::lock_guard<std::mutex> l(mutex_);
        state_.state="Loading";state_.error.clear();state_.model_hash.clear();state_.classes.clear();
        state_.audit_status="disabled";state_.audit_path.clear();state_.audit_error.clear();state_.audit_saved=0;
        queued_=false;source_key_.clear();last_submit_={};
        dispersed_intervals_=options.dispersed_intervals;
        for(auto& r:state_.history)r.current=false;
    }
    ++generation_;enabled_=true;ready_=false;
    try{thread_=std::thread(&Inference::run,this,std::move(options));}
    catch(...){enabled_=false;throw;}
    return true;
}
void Inference::stop() noexcept {
    enabled_=false;ready_=false;++generation_;wake_.notify_all();
}
void Inference::invalidate() {
    ++generation_;std::lock_guard<std::mutex> l(mutex_);
    queued_=false;source_key_.clear();
    for(auto& r:state_.history)r.current=false;
}
void Inference::clear_history(){std::lock_guard<std::mutex> l(mutex_);state_.history.clear();}
void Inference::observe(const std::vector<std::complex<float>>& iq,const wifi_security::CaptureRecord& c) noexcept {
    if(!enabled_||!ready_)return;
    const auto observed_generation=generation_.load();
    struct Duration {
        Clock::time_point start=Clock::now();
        std::atomic<uint64_t>& maximum;
        ~Duration(){
            uint64_t n=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count();
            auto old=maximum.load();while(n>old&&!maximum.compare_exchange_weak(old,n)){}
        }
    } duration{Clock::now(),max_observer_ns_};
    ++offered_;offered_samples_+=iq.size();
    try {
        const double rate=c.sample_rate_hz;
        if(!c.contiguous()||!std::isfinite(rate)||rate<20e6||rate>500e6||rate!=std::floor(rate)||
           c.samples_received!=iq.size()||c.run_id.size()>256||c.source.size()>512||
           !std::isfinite(c.capture_center_hz)||!std::isfinite(c.actual_rf_hz)||!std::isfinite(c.actual_dsp_hz)) {
            ++invalid_;++generation_;return;
        }
        const size_t n=std::llround(rate*.008);
        if(n>max_samples||iq.size()<n){++invalid_;return;}
        std::unique_lock<std::mutex> l(mutex_,std::try_to_lock);
        if(!l.owns_lock()){++dropped_;return;}
        if(!enabled_||!ready_||observed_generation!=generation_.load()){++dropped_;return;}
        auto key=source_key(c);
        if(key!=source_key_) {
            ++generation_;source_key_=std::move(key);if(queued_)++state_.stale;queued_=false;
            for(auto& r:state_.history)r.current=false;
        }
        if(queued_||Clock::now()-last_submit_<std::chrono::milliseconds(500)){++dropped_;return;}
        pending_.selection=select_intervals(iq.size(),n,dispersed_intervals_,max_samples);
        const auto& selection=pending_.selection;
        pending_.iq.resize(selection.count*n);
        for(size_t i=0;i<selection.count;++i)
            std::copy_n(iq.begin()+selection.offsets[i],n,pending_.iq.begin()+i*n);
        pending_.capture=c;pending_.generation=generation_;
        pending_.submitted=Clock::now();last_submit_=pending_.submitted;
        queued_=true;++state_.submitted_jobs;state_.submitted+=selection.count;
        state_.submitted_samples+=selection.count*n;wake_.notify_one();
    }catch(...){++invalid_;}
}
Snapshot Inference::snapshot() const {
    std::lock_guard<std::mutex> l(mutex_);auto s=state_;
    s.enabled=enabled_;s.ready=ready_;s.queued=queued_;
    s.offered=offered_;s.offered_samples=offered_samples_;s.dropped=dropped_;s.invalid=invalid_;
    s.max_observer_ms=max_observer_ns_.load()/1e6;
    if(!s.enabled&&s.state!="Failed"&&s.state!="Disabled")s.state="Stopped";
    for(auto& r:s.history)if(!s.enabled||r.generation!=generation_||Clock::now()-r.completed>std::chrono::seconds(2))r.current=false;
    return s;
}
void Inference::run(Options options) {
    try {
        // Both fixed-capacity slots are allocated outside acquisition.
        {
            std::lock_guard<std::mutex> l(mutex_);
            pending_.iq.reserve(max_samples);working_.iq.reserve(max_samples);
        }
        Client client(enabled_);client.start(options);
        auto hello=parse(client.receive(Clock::now()+std::chrono::seconds(60)));
        if(hello.get<std::string>("schema","")!="rfmon.ml.ready.v1")
            throw std::runtime_error(hello.get<std::string>("error","Invalid model handshake"));
        {
            std::lock_guard<std::mutex> l(mutex_);
            state_.model_hash=hello.get<std::string>("model_sha256");
            state_.architecture=hello.get<std::string>("architecture");
            state_.audit_status=hello.get<std::string>("audit.status","disabled");
            state_.audit_path=hello.get<std::string>("audit.path","");
            state_.audit_error=hello.get<std::string>("audit.error","");
            for(auto& x:hello.get_child("classes"))state_.classes+=x.second.get_value<std::string>()+"\n";
            state_.state="Waiting for compatible Wi-Fi data";
        }
        ready_=true;
        while(enabled_) {
            {
                std::unique_lock<std::mutex> l(mutex_);
                wake_.wait(l,[&]{return !enabled_||queued_;});
                if(!enabled_)break;
                std::swap(working_,pending_);queued_=false;
                if(working_.generation!=generation_){++state_.stale;continue;}
                state_.state="Running";
            }
            const auto& c=working_.capture;
            for(size_t interval=0;interval<working_.selection.count;++interval) {
            if(!enabled_)break;
            if(working_.generation!=generation_){std::lock_guard<std::mutex> l(mutex_);++state_.stale;break;}
            const auto samples=working_.selection.samples;
            const auto sample_offset=working_.selection.offsets[interval];
            Tree meta;meta.put("schema","rfmon.ml.iq.v1");meta.put("samples",samples);
            meta.put("want_stft_preview",true);
            meta.put("sample_rate_hz",c.sample_rate_hz);meta.put("offset_samples",sample_offset);
            meta.put("interval_index",interval);meta.put("interval_count",working_.selection.count);
            meta.put("selection","disjoint_capture_intervals_v1");
            meta.put("device_time_anchor","capture_first_sample; add offset_samples / sample_rate_hz");
            meta.put("capture_seq",c.capture_seq);meta.put("radio_session",c.radio_session);
            meta.put("generation",working_.generation);meta.put("run_id",c.run_id);
            meta.put("center_hz",c.actual_rf_hz?c.actual_rf_hz+c.actual_dsp_hz:c.capture_center_hz);
            meta.put("device",c.device);meta.put("source",c.source);meta.put("band",c.band);
            meta.put("clock_domain",int(c.clock));meta.put("device_time_ns",c.device_time_ns);
            meta.put("host_before_ns",c.host_before_ns);meta.put("host_after_ns",c.host_after_ns);
            meta.put("requested_rate_hz",c.requested_rate_hz);meta.put("requested_center_hz",c.capture_center_hz);
            meta.put("actual_rf_hz",c.actual_rf_hz);meta.put("actual_dsp_hz",c.actual_dsp_hz);
            meta.put("channel",c.channel);meta.put("antenna",c.antenna);
            if(c.gain_db)meta.put("gain_db",*c.gain_db);
            meta.put("capture_samples",c.samples_received);meta.put("contiguous",c.contiguous());
            auto deadline=Clock::now()+std::chrono::milliseconds(options.deadline_ms);
            client.request(encode(meta),working_.iq.data()+interval*samples,samples,deadline);
            auto raw=client.receive(deadline);auto t=parse(raw);
            if(t.get<std::string>("schema","")!="rfmon.ml.result.v1")
                throw std::runtime_error(t.get<std::string>("error","Invalid model result"));
            if(t.get<uint64_t>("request.capture_seq")!=c.capture_seq||
               t.get<uint64_t>("request.generation")!=working_.generation||
               t.get<uint64_t>("request.offset_samples")!=sample_offset||
               t.get<std::string>("request.run_id")!=c.run_id)throw std::runtime_error("Mismatched model result");
            Result result;result.raw=raw;result.status=t.get<std::string>("status");
            result.candidate=t.get<std::string>("candidate","");
            if(auto preview=t.get_child_optional("stft_preview")) {
                auto p=std::make_shared<StftPreview>();
                p->rows=preview->get<int>("rows");p->cols=preview->get<int>("cols");
                p->db_min=preview->get<double>("db_min");p->db_max=preview->get<double>("db_max");
                p->rate_hz=preview->get<double>("rate_hz");p->duration_seconds=preview->get<double>("duration_seconds");
                p->nfft=preview->get<int>("nfft");p->hop=preview->get<int>("hop");
                if(preview->get<std::string>("schema","")!="rfmon.ml.stft.v1"||
                   preview->get<std::string>("encoding","")!="u8_frequency_time"||
                   preview->get<std::string>("scaling","")!="source_unit_rms_log_power_before_checkpoint_normalization"||
                   p->rows!=256||p->cols!=625||preview->get<int>("bytes")!=160000||
                   !std::isfinite(p->db_min)||!std::isfinite(p->db_max)||p->db_max<=p->db_min||
                   p->rate_hz!=20e6||p->duration_seconds!=.008||p->nfft!=256||p->hop!=256)
                    throw std::runtime_error("Invalid STFT preview contract");
                if(!t.get_optional<double>("selected_offset_hz"))
                    throw std::runtime_error("Missing STFT frequency provenance");
                auto pixels=client.receive(deadline);
                if(pixels.size()!=160000)throw std::runtime_error("Invalid STFT preview length");
                p->pixels.assign(pixels.begin(),pixels.end());result.stft=std::move(p);
            }
            // Defense in depth: this integration has no qualified live policy,
            // even if an alternate worker sends a non-null accepted identity.
            result.accepted.clear();
            if(auto reason=t.get_child_optional("reason")) {
                result.reason=reason->get_value<std::string>();
                for(auto& r:*reason)result.reason+=r.second.get_value<std::string>()+" ";
            }
            if(auto top=t.get_child_optional("top_candidates"))for(auto& r:*top) {
                double score=r.second.get<double>("score");
                if(!std::isfinite(score)||score<0||score>1)throw std::runtime_error("Invalid model score");
                if(result.top.size()<3)result.top.push_back({r.second.get<std::string>("label"),score});
            }
            result.processing_ms=t.get<double>("processing_ms");
            if(auto offset=t.get_optional<double>("selected_offset_hz")) {
                if(!std::isfinite(*offset))throw std::runtime_error("Invalid selected view");
                result.selected_offset=*offset;result.selected_offset_available=true;
            }
            if(!std::isfinite(result.processing_ms)||result.processing_ms<0)throw std::runtime_error("Invalid latency");
            result.total_ms=std::chrono::duration<double,std::milli>(Clock::now()-working_.submitted).count();
            result.capture=c.capture_seq;result.session=c.radio_session;result.run=c.run_id;
            result.offset_samples=sample_offset;result.interval_index=interval;result.interval_count=working_.selection.count;
            result.source=c.source;result.band=c.band;result.device=c.device;result.rate=c.sample_rate_hz;
            result.center=meta.get<double>("center_hz");result.generation=working_.generation;
            result.completed=Clock::now();result.current=true;
            std::lock_guard<std::mutex> l(mutex_);
            state_.audit_status=t.get<std::string>("audit.status",state_.audit_status);
            state_.audit_path=t.get<std::string>("audit.path",state_.audit_path);
            state_.audit_error=t.get<std::string>("audit.error",state_.audit_error);
            state_.audit_saved=t.get<uint64_t>("audit.saved",state_.audit_saved);
            if(!enabled_||result.generation!=generation_){++state_.stale;continue;}
            result.model_hash=state_.model_hash;result.architecture=state_.architecture;
            ++state_.processed;
            // Only the latest observation retains display pixels. Snapshot
            // copies share this immutable buffer; history never copies grids.
            if(!state_.history.empty())state_.history.front().stft.reset();
            state_.history.insert(state_.history.begin(),std::move(result));
            if(state_.history.size()>history_limit)state_.history.pop_back();
            state_.state="Waiting for compatible Wi-Fi data";
            }
        }
    }catch(const std::exception& e) {
        std::lock_guard<std::mutex> l(mutex_);
        if(enabled_){state_.error=e.what();state_.state="Failed";}
    }
    ready_=false;enabled_=false;
}
}
