// Exercises production navigation with real mouse/keyboard events, disconnected.
#include "gui/app_shell.hpp"
#include "ml_panel.hpp"
#include "device_inventory_panel.hpp"
#include "imgui_internal.h"
#include "backends/imgui_impl_opengl3.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace rfmon::gui;
static void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
int main(int argc,char** argv) {
    auto display=eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA,EGL_DEFAULT_DISPLAY,nullptr);
    require(eglInitialize(display,nullptr,nullptr)&&eglBindAPI(EGL_OPENGL_API),"Offscreen EGL unavailable");
    EGLint attrs[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_NONE};
    EGLConfig config;EGLint count;require(eglChooseConfig(display,attrs,&config,1,&count)&&count,"EGL config");
    EGLint dims[]={EGL_WIDTH,1600,EGL_HEIGHT,1000,EGL_NONE};
    auto surface=eglCreatePbufferSurface(display,config,dims);auto context=eglCreateContext(display,config,EGL_NO_CONTEXT,nullptr);
    require(eglMakeCurrent(display,surface,surface,context),"EGL make current");
    ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.DisplaySize={1600,1000};io.DeltaTime=1.f/60;
    configure_font();apply_theme();ImGui_ImplOpenGL3_Init("#version 130");
    rfmon::ml::Inference inference;
    State state;Context ctx;ctx.radio="USRP X310";ctx.band="Wi-Fi 2.4 GHz";ctx.run="synthetic-display";
    int commands=0;bool preview=false,stft_mode=false;ImVec2 command_center;std::string text;
    rfmon::ml::Result stft_result;auto stft=std::make_shared<rfmon::ml::StftPreview>();
    stft->db_min=-100;stft->db_max=20;stft->pixels.resize(256*625);
    for(int f=0;f<256;++f)for(int t=0;t<625;++t)
        stft->pixels[f*625+t]=(std::abs(f-(90+t/5))<3|| (t>450&&t<500&&f>30&&f<210))?235:20;
    stft_result.stft=stft;stft_result.center=2440e6;stft_result.capture=42;stft_result.current=true;
    stft_result.completed=std::chrono::steady_clock::now();
    // Synthetic device rows (no receiver): one or two per technology.
    std::vector<rfmon::devices::Device> devices;
    rfmon::devices::PanelState devices_panel;
    const int64_t now=1793800000;
    {
        using rfmon::devices::Technology;
        auto add=[&](Technology t,const char* id,const char* kind,const char* name,const char* role,const char* vendor,const char* ch,uint64_t n,int64_t age){
            rfmon::devices::Device d;d.technology=t;d.identifier=id;d.key=std::string(rfmon::devices::technology_name(t))+" "+id;d.identifier_kind=kind;
            d.name=name;d.role=role;d.vendor=vendor;d.channel=ch;d.observations=n;d.observations_unit="frames";d.first_seen=now-age-600;d.last_seen=now-age;
            d.details.push_back({"Note","SYNTHETIC fixture row"});devices.push_back(d);};
        add(Technology::Wifi24,"02:00:00:00:00:01","MAC (global)","SYNTHETIC-AP","AP / network interface","Example Vendor","ch 6",420,3);
        add(Technology::Wifi24,"02:00:00:00:00:02","MAC (locally administered / randomised)","","Station transmitter (observed)","","ch 6",37,40);
        add(Technology::Wifi5,"02:00:00:00:00:03","MAC (global)","SYNTHETIC-5G","AP / network interface","Example Vendor","ch 36",900,1);
        add(Technology::Bluetooth,"C0:FF:EE:00:00:01","random static","SYNTHETIC-TAG","Advertiser","","BLE 37, 38, 39",1200,0);
        add(Technology::Zigbee,"PAN 0x1A62 / 0x0000","PAN + short address","","Coordinator","","ch 15",55,90);
        add(Technology::Lora,"26011BDA","DevAddr","","End device (uplink seen)","","865.063 MHz SF7",9,4000);
        devices_panel.selected=devices[0].key;
    }
    auto frame=[&](bool log=false){
        ImGui_ImplOpenGL3_NewFrame();ImGui::NewFrame();if(log)ImGui::LogToBuffer(0);
        draw_workspace(state,ctx,true,[&](Page page){
            ImGui::TextUnformatted("SYNTHETIC UI fixture — receiver disconnected; no RF acquisition");
            if(!preview) {
            if(ImGui::Button("Explicit fixture command"))++commands;
            auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();command_center={(a.x+b.x)/2,(a.y+b.y)/2};
            if(section("Complete retained details",true))ImGui::TextUnformatted("Unavailable values remain unavailable; exact raw evidence retained.");
            }
            if(page==Page::Settings)draw_capabilities(true,true);
            if(page==Page::ModelInference) {
                if(stft_mode)rfmon::ml::draw_stft_preview(stft_result);
                else rfmon::ml::draw_panel(inference,false,"wifi_2g4");
            }
            if(page==Page::Overview){Overview data;data.captures=12;data.groups=4;draw_overview(state,ctx,data,true,[]{note("Synthetic fixture: frequency map unavailable; no RF samples acquired.");},
                [&]{rfmon::devices::draw_panel(devices_panel,devices,now);});}
        });
        if(log){text=ImGui::GetCurrentContext()->LogBuffer.c_str();ImGui::LogFinish();}
        ImGui::Render();require(ImGui::GetCurrentContext()->ErrorCountCurrentFrame==0,"ImGui state error");require(ImGui::GetDrawData()->TotalVtxCount>0,"Empty draw data");
        glViewport(0,0,int(io.DisplaySize.x),int(io.DisplaySize.y));glClearColor(.065,.085,.115,1);glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());require(glGetError()==GL_NO_ERROR,"OpenGL error");
    };
    auto click=[&](ImVec2 pos){io.AddMousePosEvent(pos.x,pos.y);frame();io.AddMouseButtonEvent(0,true);frame();io.AddMouseButtonEvent(0,false);frame();};
    frame();frame();
    // The production rail's first row begins after its WORKSPACES label.
    int navigation_row=0;
    for(int i=0;i<int(Page::Count);++i) {
        if(static_cast<Page>(i)==Page::Wifi5)continue; // unified Wi-Fi workspace
        ImGuiWindow* nav=nullptr;for(auto* w:ImGui::GetCurrentContext()->Windows)if(std::string(w->Name).find("/Navigation_")!=std::string::npos)nav=w;
        require(nav,"Navigation child missing");
        const float y=nav->DC.CursorStartPos.y+ImGui::GetTextLineHeightWithSpacing()+navigation_row++*(32+ImGui::GetStyle().ItemSpacing.y)+16;
        click({nav->DC.CursorStartPos.x+70,y});
        require(state.page==static_cast<Page>(i),"Navigation selected wrong page");
        require(commands==0,"Navigation emitted command");
    }
    require(!inference.snapshot().enabled,"Navigation started inference");
    state.page=Page::Settings;frame();
    const auto& style=ImGui::GetStyle();
    const float setup_x=style.WindowPadding.x+ImGui::CalcTextSize("RF MONITOR").x+style.ItemSpacing.x+ImGui::CalcTextSize("/ Settings").x+style.ItemSpacing.x+(ImGui::CalcTextSize("Monitoring setup").x+2*style.FramePadding.x)/2;
    click({setup_x,style.WindowPadding.y+ImGui::GetFrameHeight()/2});
    require(state.setup_open && commands==0,"Monitoring setup must open without a command");
    click({setup_x,style.WindowPadding.y+ImGui::GetFrameHeight()/2});
    require(!state.setup_open && commands==0,"Closing setup must not issue command");
    click(command_center);require(commands==1,"Explicit action must issue exactly one command");
    ctx.stalled=true;ctx.overflow=true;ctx.error="Synthetic recording failure";frame(true);
    require(text.find("RX stalled")!=std::string::npos&&text.find("Synthetic recording failure")!=std::string::npos,"Material warnings hidden");
    require(commands==1&&ctx.band=="Wi-Fi 2.4 GHz","Passive rendering changed acquisition");
    state.page=Page::Overview;frame();frame(true);
    require(text.find("SYNTHETIC-AP")!=std::string::npos&&text.find("26011BDA")!=std::string::npos&&text.find("6 of 6 identifiers shown")!=std::string::npos,"Combined device list missing");
    std::snprintf(devices_panel.search,sizeof devices_panel.search,"%s","zigbee");frame();frame(true);
    require(text.find("1 of 6 identifiers shown")!=std::string::npos,"Device search did not filter");
    devices_panel.search[0]=0;
    for(float scale:{1.f,1.25f,1.5f,2.f}) {
        state.scale=scale;apply_theme(scale,true);io.DisplaySize={1280,800};frame();frame();
        state.legacy=true;frame();state.legacy=false;
        require(commands==1,"Scale/layout switch emitted command");
    }
    preview=true;ctx.error.clear();ctx.stalled=false;ctx.overflow=false;
    state.page=Page::ModelInference;frame(true);
    require(text.find("DSP triage (observation only)")!=std::string::npos,"DSP observation control missing");
    require(text.find("No protocol-based rejection is enabled")!=std::string::npos,"DSP limits hidden");
    require(!inference.snapshot().enabled,"DSP rendering started worker");
    stft_mode=true;state.page=Page::ModelInference;frame(true);
    require(text.find("Colour range")!=std::string::npos,"STFT preview missing");
    stft_result.current=false;frame(true);
    require(text.find("Historical / stale")!=std::string::npos,"STFT staleness hidden");
    stft_result.current=true;
    state.scale=1;apply_theme();io.DisplaySize={1600,1000};state.page=argc>2?Page::ModelInference:Page::Overview;frame();frame();
    const auto start=std::chrono::steady_clock::now();for(int i=0;i<120;++i)frame();
    std::cout<<"Shell software-render average ms/frame: "<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()/120<<'\n';
    if(argc>1){glFinish();std::vector<unsigned char> bytes(1600*1000*3);glPixelStorei(GL_PACK_ALIGNMENT,1);glReadPixels(0,0,1600,1000,GL_RGB,GL_UNSIGNED_BYTE,bytes.data());std::ofstream out(argv[1],std::ios::binary);out<<"P6\n1600 1000\n255\n";for(int y=999;y>=0;--y)out.write(reinterpret_cast<const char*>(bytes.data()+y*1600*3),1600*3);}
    rfmon::ml::release_preview_texture();
    ImGui_ImplOpenGL3_Shutdown();ImGui::DestroyContext();eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);eglDestroyContext(display,context);eglDestroySurface(display,surface);eglTerminate(display);
    std::cout<<"Production navigation, warning visibility, explicit command and scale isolation: PASS\n";
}
