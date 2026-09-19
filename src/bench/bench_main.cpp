// wifi_test_bench - standalone Wi-Fi 2.4GHz RF-parameter test bench.
//
// Separate app from rf_monitor_gui (main.cpp): one locked frequency, a
// chosen USRP X310 RX channel, live IQ/FFT + a full-parameter packet
// table, and post-disconnect playback. See bench_capture.hpp for why
// this is a different capture loop from Scanner, and bench_gui.hpp for
// the UI. Links against the SAME production wifi_phy.cpp/
// wifi_fingerprint.cpp/wifi_frame.cpp/wifi_dsss_rx.cpp as the main app -
// nothing here reimplements any RF math.
#include <cstdio>

#include <GLFW/glfw3.h>

#include "imgui.h"
#include "implot.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

#include "bench_capture.hpp"
#include "bench_gui.hpp"
#include "bench_tx.hpp"

using namespace rfmon::bench;

namespace {
void GlfwErrorCallback(int error, const char* description) {
    std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
}
}  // namespace

int main() {
    glfwSetErrorCallback(GlfwErrorCallback);
    if (!glfwInit()) {
        std::fprintf(stderr, "Failed to initialize GLFW\n");
        return 1;
    }

    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    GLFWwindow* window = glfwCreateWindow(1280, 800, "Wi-Fi Test Bench", nullptr, nullptr);
    if (window == nullptr) {
        std::fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // Neither is auto-started - the user picks a USRP channel and
    // clicks Connect (RX) / Connect TX (see bench_gui.cpp).
    BenchCapture capture;
    BenchTx tx;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        draw_bench_window(capture, tx);

        ImGui::Render();

        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.043f, 0.059f, 0.078f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    capture.stop();
    tx.stop();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
