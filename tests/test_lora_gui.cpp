// Renders real LoRa widgets offscreen, without a Scanner or radio.
#include "lora_gui.hpp"
#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <fstream>
#include <iostream>
using namespace rfmon;
int main(int argc, char** argv) {
    EGLDisplay display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    if (!eglInitialize(display,nullptr,nullptr) || !eglBindAPI(EGL_OPENGL_API)) return 1;
    EGLint attrs[] = {EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_BIT,
                     EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_NONE};
    EGLConfig config; EGLint count;
    if (!eglChooseConfig(display,attrs,&config,1,&count) || count<1) return 1;
    EGLint size[] = {EGL_WIDTH,1800,EGL_HEIGHT,650,EGL_NONE};
    auto surface=eglCreatePbufferSurface(display,config,size);
    auto context=eglCreateContext(display,config,EGL_NO_CONTEXT,nullptr);
    if (!eglMakeCurrent(display,surface,surface,context)) return 1;
    ImGui::CreateContext(); ImGui::StyleColorsDark();
    auto& io=ImGui::GetIO(); io.IniFilename=nullptr; io.DisplaySize=ImVec2(1800,650); io.DeltaTime=1.0f/60;
    ImGui_ImplOpenGL3_Init("#version 130");
    std::vector<LoraPacketRow> rows;
    for (int i=0;i<5;++i) {
        LoraPacketRow r; r.time="12:00:00"; r.freq_mhz=866.9; r.sf=7; r.bandwidth_khz=125;
        if (i==0) { r.status="Detected only"; r.detail="Preamble only; header unresolved."; }
        else { r.decoder="SX reference (unvalidated OTA)"; r.cr=1; r.payload_len=3;
               set_lora_integrity(r,i!=1,i!=2,i==4);
               if (r.payload_complete) { r.payload_hex="01 02 03"; r.payload_repr="..."; } }
        rows.push_back(r);
    }
    for (int frame=0;frame<3;++frame) {
        ImGui_ImplOpenGL3_NewFrame(); ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0,0)); ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("LoRa interpretation - synthetic display fixtures");
        ImGui::TextWrapped("Each row is an SF/BW hypothesis. CRC does not establish protocol identity or real-radio interoperability.");
        draw_lora_packet_table(rows,260);
        ImGui::SetNextItemOpen(true,ImGuiCond_Always); draw_lora_replay_panel();
        ImGui::End(); ImGui::Render();
        if (ImGui::GetDrawData()->TotalVtxCount<=0) return 1;
        glViewport(0,0,1800,650); glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData()); glFinish();
        if (glGetError()!=GL_NO_ERROR) return 1;
    }
    if (argc>1) {
        std::vector<unsigned char> pixels(1800*650*3); glPixelStorei(GL_PACK_ALIGNMENT,1);
        glReadPixels(0,0,1800,650,GL_RGB,GL_UNSIGNED_BYTE,pixels.data());
        std::ofstream out(argv[1],std::ios::binary); out << "P6\n1800 650\n255\n";
        for (int y=649;y>=0;--y) out.write(reinterpret_cast<const char*>(pixels.data()+y*1800*3),1800*3);
    }
    ImGui_ImplOpenGL3_Shutdown(); ImGui::DestroyContext();
    eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
    eglDestroyContext(display,context); eglDestroySurface(display,surface); eglTerminate(display);
    std::cout << "PASS: LoRa integrity table and replay controls rendered offscreen\n";
}
