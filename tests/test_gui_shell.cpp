// Exercises production navigation with real mouse/keyboard events, disconnected.
#include "gui/app_shell.hpp"
#include "ml_panel.hpp"
#include "imgui_internal.h"
#include "backends/imgui_impl_opengl3.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <chrono>
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
    int commands=0;bool preview=false;ImVec2 command_center;std::string text;
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
            if(page==Page::ModelInference)rfmon::ml::draw_panel(inference,false,"wifi_2g4");
            if(page==Page::Overview){Overview data;data.captures=12;data.groups=4;draw_overview(state,ctx,data,true,[]{note("Synthetic fixture: frequency map unavailable; no RF samples acquired.");});}
        });
        if(log){text=ImGui::GetCurrentContext()->LogBuffer.c_str();ImGui::LogFinish();}
        ImGui::Render();require(ImGui::GetCurrentContext()->ErrorCountCurrentFrame==0,"ImGui state error");require(ImGui::GetDrawData()->TotalVtxCount>0,"Empty draw data");
        glViewport(0,0,int(io.DisplaySize.x),int(io.DisplaySize.y));glClearColor(.065,.085,.115,1);glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());require(glGetError()==GL_NO_ERROR,"OpenGL error");
    };
    auto click=[&](ImVec2 pos){io.AddMousePosEvent(pos.x,pos.y);frame();io.AddMouseButtonEvent(0,true);frame();io.AddMouseButtonEvent(0,false);frame();};
    frame();frame();
    // The production rail's first row begins after its WORKSPACES label.
    for(int i=0;i<int(Page::Count);++i) {
        ImGuiWindow* nav=nullptr;for(auto* w:ImGui::GetCurrentContext()->Windows)if(std::string(w->Name).find("/Navigation_")!=std::string::npos)nav=w;
        require(nav,"Navigation child missing");
        const float y=nav->DC.CursorStartPos.y+ImGui::GetTextLineHeightWithSpacing()+i*(32+ImGui::GetStyle().ItemSpacing.y)+16;
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
    for(float scale:{1.f,1.25f,1.5f,2.f}) {
        state.scale=scale;apply_theme(scale,true);io.DisplaySize={1280,800};frame();frame();
        state.legacy=true;frame();state.legacy=false;
        require(commands==1,"Scale/layout switch emitted command");
    }
    preview=true;ctx.error.clear();ctx.stalled=false;ctx.overflow=false;
    state.scale=1;apply_theme();io.DisplaySize={1600,1000};state.page=argc>2?Page::ModelInference:Page::Overview;frame();frame();
    const auto start=std::chrono::steady_clock::now();for(int i=0;i<120;++i)frame();
    std::cout<<"Shell software-render average ms/frame: "<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()/120<<'\n';
    if(argc>1){glFinish();std::vector<unsigned char> bytes(1600*1000*3);glPixelStorei(GL_PACK_ALIGNMENT,1);glReadPixels(0,0,1600,1000,GL_RGB,GL_UNSIGNED_BYTE,bytes.data());std::ofstream out(argv[1],std::ios::binary);out<<"P6\n1600 1000\n255\n";for(int y=999;y>=0;--y)out.write(reinterpret_cast<const char*>(bytes.data()+y*1600*3),1600*3);}
    ImGui_ImplOpenGL3_Shutdown();ImGui::DestroyContext();eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);eglDestroyContext(display,context);eglDestroySurface(display,surface);eglTerminate(display);
    std::cout<<"Production navigation, warning visibility, explicit command and scale isolation: PASS\n";
}
