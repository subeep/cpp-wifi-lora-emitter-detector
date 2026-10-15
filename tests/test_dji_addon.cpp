#include "drones/panel.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include <iostream>
#include <functional>
#ifdef DJI_EGL
#include "backends/imgui_impl_opengl3.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#endif
#include <stdexcept>

static void require(bool ok,const char* text) { if(!ok) throw std::runtime_error(text); }
static void wait_until(const std::function<bool()>& done) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(45);
    while(!done() && std::chrono::steady_clock::now()<deadline) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    require(done(),"worker timeout");
}
int main(int argc,char** argv) {
#ifdef DJI_EGL
    EGLDisplay display=eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA,EGL_DEFAULT_DISPLAY,nullptr);
    require(eglInitialize(display,nullptr,nullptr)&&eglBindAPI(EGL_OPENGL_API),"EGL unavailable");
    EGLint attrs[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_NONE};
    EGLConfig config; EGLint count;
    require(eglChooseConfig(display,attrs,&config,1,&count)&&count,"EGL config failed");
    EGLint size[]={EGL_WIDTH,1280,EGL_HEIGHT,900,EGL_NONE};
    auto surface=eglCreatePbufferSurface(display,config,size);
    auto context=eglCreateContext(display,config,EGL_NO_CONTEXT,nullptr);
    require(eglMakeCurrent(display,surface,surface,context),"EGL context failed");
#endif
    drone::Addon a;
    rfmon::wifi_security::CaptureRecord record;
    std::vector<std::complex<float>> iq(20000);
    a.observe(iq,record); require(a.offered==0,"disabled observer must be inert");
    a.live.start(); wait_until([&]{return a.live.snapshot().ready||!a.live.running();});
    require(a.live.snapshot().ready,"decoder handshake failed");
    a.observe(iq,record); require(a.unhealthy==1,"unhealthy capture must be rejected");
    std::ifstream fixture(std::string(PROJECT_ROOT_DIR)+"/tests/fixtures/drones/dji_mavic_air_2.fc32",std::ios::binary|std::ios::ate);
    iq.resize(size_t(fixture.tellg())/8); fixture.seekg(0); fixture.read(reinterpret_cast<char*>(iq.data()),iq.size()*8);
    record.samples_received=iq.size();record.sample_rate_hz=50e6;record.capture_center_hz=2444.5e6;
    record.clock=rfmon::wifi_security::ClockDomain::UsrpDevice;record.run_id="radio-free-fixture";
    a.observe(iq,record); wait_until([&]{return a.live.snapshot().samples>=iq.size()||!a.live.running();});
    require(a.live.snapshot().decoded==1,"capture observer did not deliver real DJI fixture");
    a.live.stop();
    a.load(std::string(PROJECT_ROOT_DIR)+"/tests/fixtures/drones/dji_mavic_air_2.fc32",50e6,2444.5e6,0);
    wait_until([&]{return !a.replay_busy;});
    auto s=a.replay.snapshot();
    require(s.decoded==1,"real fixture failed through C++ subprocess transport");
    require(s.observations.at(0).model=="Mavic Air 2","wrong decoded model");
    require(s.observations.at(0).source.find("OFFLINE")!=std::string::npos,"missing provenance");
    require(a.live.snapshot().decoded==1,"offline evidence leaked into live state");
    a.load("/no/such/recording",50e6,2444.5e6,0); wait_until([&]{return !a.replay_busy;});
    require(a.replay_status().find("regular")!=std::string::npos,"file error not exposed");
    drone::RidJson frames;
    std::ifstream(std::string(PROJECT_ROOT_DIR)+"/tests/fixtures/remote_id/monitor_frames.json")>>frames;
    rfmon::wifi_security::FrameEvent rid_event;rid_event.run_id="synthetic-GUI-fixture";rid_event.fcs_valid=true;
    rid_event.host_time_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    auto rid_hex=frames.at("full").get<std::string>();
    for(size_t i=0;i<rid_hex.size();i+=2)rid_event.mpdu.push_back(std::stoul(rid_hex.substr(i,2),nullptr,16));
    a.remote_id.enable(true);a.remote_id.submit(rid_event);
    wait_until([&]{return a.remote_id.snapshot()["examined"].get<int>()==1;});
    auto rid=a.remote_id.snapshot();require(rid["tracks"].size()==1,"RID fixture unavailable for GUI");
    ImGui::CreateContext(); auto& io=ImGui::GetIO(); io.IniFilename=nullptr; io.DisplaySize={1280,900}; io.DeltaTime=1.f/60;
#ifndef DJI_EGL
    unsigned char* pixels;int w,h;io.Fonts->GetTexDataAsRGBA32(&pixels,&w,&h);
#endif
#ifdef DJI_EGL
    ImGui_ImplOpenGL3_Init("#version 130");
#endif
    for(int i=0;i<3;++i) {
#ifdef DJI_EGL
        ImGui_ImplOpenGL3_NewFrame();
#endif
        ImGui::NewFrame(); ImGui::SetNextWindowSize(io.DisplaySize); ImGui::SetNextWindowPos({0,0});
        ImGui::Begin("Drones - recorded Mavic Air 2 validation");
        ImGui::TextWrapped("DJI DroneID | Checksum validated; reported identity is unauthenticated | OFFLINE recording");
        ImGui::LogToBuffer(); drone::draw_results(s,true);
        const std::string text=ImGui::GetCurrentContext()->LogBuffer.c_str(); ImGui::LogFinish();
        require(text.find("Mavic Air 2")!=std::string::npos,"model missing from GUI");
        require(text.find("OFFLINE")!=std::string::npos,"offline label missing from GUI");
        ImGui::End(); ImGui::Render(); require(ImGui::GetDrawData()->TotalVtxCount>0,"empty GUI");
    }
    // Exercise the native Remote ID inspector independently of the DJI view.
    for(int i=0;i<3;++i) {
#ifdef DJI_EGL
        ImGui_ImplOpenGL3_NewFrame();
#endif
        ImGui::NewFrame();ImGui::SetNextWindowSize(io.DisplaySize);ImGui::SetNextWindowPos({0,0});
        ImGui::Begin("Drones - Wi-Fi Remote ID / synthetic reference fixture");
        ImGui::TextWrapped("Synthetic protocol fixture; no live aircraft reception. Reported identity is unauthenticated.");
        if(i<2) {
            ImGui::LogToBuffer();drone::draw_remote_id(rid);
            std::string rendered=ImGui::GetCurrentContext()->LogBuffer.c_str();ImGui::LogFinish();
            require(rendered.find("TEST-RID-FULL-0001")!=std::string::npos,"reported ID missing from GUI");
            require(rendered.find("Wi-Fi Beacon")!=std::string::npos,"transport missing from GUI");
        } else drone::draw_remote_id(rid);
        ImGui::End();ImGui::Render();require(ImGui::GetCurrentContext()->ErrorCountCurrentFrame==0,"ImGui renderer state error");
    }
#ifdef DJI_EGL
    glViewport(0,0,1280,900); glClear(GL_COLOR_BUFFER_BIT); ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    if(argc>1) {
        std::vector<unsigned char> rgb(1280*900*3); glPixelStorei(GL_PACK_ALIGNMENT,1);
        glReadPixels(0,0,1280,900,GL_RGB,GL_UNSIGNED_BYTE,rgb.data());
        std::ofstream out(argv[1],std::ios::binary); out<<"P6\n1280 900\n255\n";
        for(int y=899;y>=0;--y) out.write(reinterpret_cast<const char*>(rgb.data()+y*1280*3),1280*3);
    }
    ImGui_ImplOpenGL3_Shutdown();
#endif
    ImGui::DestroyContext();
#ifdef DJI_EGL
    eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
    eglDestroyContext(display,context);eglDestroySurface(display,surface);eglTerminate(display);
#endif
    std::cout<<"DJI worker, isolation, source failure and GUI checks passed\n";
}
