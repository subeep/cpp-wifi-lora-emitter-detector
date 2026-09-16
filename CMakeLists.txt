cmake_minimum_required(VERSION 3.16)
project(rf_monitor_gui CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# This machine has a Homebrew/linuxbrew `ld` earlier on PATH than the
# system linker. The Homebrew ld's built-in library search path does not
# include Ubuntu's multiarch dir (/usr/lib/x86_64-linux-gnu), so it fails
# to locate transitive shared-library dependencies (libX11, libGLdispatch,
# etc.) even though they are installed. -B/usr/bin tells gcc/g++ to look in
# /usr/bin first for its subprograms (ld, as, ...), forcing use of the
# system linker, which has the correct multiarch search path built in.
add_link_options(-B/usr/bin)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
# This system only ships the glfw3 pkg-config file, not a CMake config
# package, so pull it in via pkg-config and link the imported target it
# generates (PkgConfig::GLFW) rather than assuming Glfw3::glfw3 exists.
pkg_check_modules(GLFW REQUIRED IMPORTED_TARGET glfw3)

# KissFFT (libkissfft-dev) - single-precision build, matches our
# complex64 IQ data. Only ships a pkg-config file, not a CMake package.
pkg_check_modules(KISSFFT REQUIRED IMPORTED_TARGET kissfft-float)

find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

# UHD (libuhd-dev) - ships an old-style CMake config package (UHD_LIBRARIES/
# UHD_INCLUDE_DIRS variables, no modern imported target).
find_package(UHD REQUIRED)

# ---------------------------------------------------------------------------
# Dear ImGui (vendored under third_party/imgui, pinned to v1.92.9)
# ---------------------------------------------------------------------------
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)

set(IMGUI_SOURCES
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/imgui_demo.cpp
  ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
  ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

# ---------------------------------------------------------------------------
# Application sources
#
# main.cpp is always built. Any additional RF/DSP/.cpp files dropped into
# src/ (e.g. config.cpp, spectrum.cpp, detector.cpp, classifier.cpp,
# registry.cpp, sdr_capture.cpp, scanner.cpp) are picked up automatically by
# the glob below -- no CMakeLists.txt edit needed to add a new source file.
# If you add/remove files and CMake doesn't notice, just re-run the cmake
# configure step (glob is evaluated at configure time).
# ---------------------------------------------------------------------------
file(GLOB APP_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.cpp)

add_executable(rf_monitor_gui
  ${APP_SOURCES}
  ${IMGUI_SOURCES}
)

target_include_directories(rf_monitor_gui PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${IMGUI_DIR}
  ${IMGUI_DIR}/backends
  ${GLFW_INCLUDE_DIRS}
  ${UHD_INCLUDE_DIRS}
)

# Lets src/lora_master.cpp resolve its persistent-storage directory
# from the source tree root rather than the process's current working
# directory (which has always just been wherever the binary happened
# to be launched from, e.g. build/) - so a build/ wipe/rebuild can
# never take the "permanent" master emitter list down with it.
target_compile_definitions(rf_monitor_gui PRIVATE PROJECT_ROOT_DIR="${CMAKE_SOURCE_DIR}")

target_link_libraries(rf_monitor_gui PRIVATE
  PkgConfig::GLFW
  PkgConfig::KISSFFT
  OpenGL::GL
  Threads::Threads
  dl
  ${UHD_LIBRARIES}
)

# ---------------------------------------------------------------------------
# Synthetic DSP correctness test - pure software, no SDR/GUI dependency.
# Direct C++ port of the validated Python prototype's tests/test_dsp.py.
# ---------------------------------------------------------------------------
add_executable(test_dsp
  tests/test_dsp.cpp
  src/spectrum.cpp
  src/detector.cpp
  src/classifier.cpp
)
target_include_directories(test_dsp PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_dsp PRIVATE PkgConfig::KISSFFT)

# ---------------------------------------------------------------------------
# Synthetic LoRa PHY codec correctness test - pure software, no hardware.
# Direct C++ port of the validated Python prototype's tests/test_lora_phy.py.
# ---------------------------------------------------------------------------
add_executable(test_lora_phy
  tests/test_lora_phy.cpp
  src/lora_phy.cpp
)
target_include_directories(test_lora_phy PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_lora_phy PRIVATE PkgConfig::KISSFFT)

# ---------------------------------------------------------------------------
# Synthetic correctness test for the second, standards-compliant
# (SX1272/76-family) LoRa PHY codec - see src/lora_phy_std.hpp for what
# this is and why it exists alongside lora_phy.cpp's self-consistent
# one. Pure software, no hardware.
# ---------------------------------------------------------------------------
add_executable(test_lora_phy_std
  tests/test_lora_phy_std.cpp
  src/lora_phy_std.cpp
)
target_include_directories(test_lora_phy_std PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_lora_phy_std PRIVATE PkgConfig::KISSFFT)

# ---------------------------------------------------------------------------
# Synthetic correctness test for the Wi-Fi DSSS/OFDM modulation
# correlators - see src/wifi_phy.hpp. Pure software, no hardware.
# ---------------------------------------------------------------------------
add_executable(test_wifi_phy
  tests/test_wifi_phy.cpp
  src/wifi_phy.cpp
)
target_include_directories(test_wifi_phy PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_wifi_phy PRIVATE PkgConfig::KISSFFT)

# ---------------------------------------------------------------------------
# 802.11 frame-layer decode (PLCP header, FCS, descrambler, beacon
# MAC/IE parsing) - see src/wifi_frame.hpp. Pure byte logic, no DSP and
# no hardware; every checksum is asserted against a PUBLISHED vector
# rather than against anything this codebase generates.
# ---------------------------------------------------------------------------
add_executable(test_wifi_frame
  tests/test_wifi_frame.cpp
  src/wifi_frame.cpp
)
target_include_directories(test_wifi_frame PRIVATE ${CMAKE_SOURCE_DIR}/src)

# ---------------------------------------------------------------------------
# End-to-end 802.11b 1 Mbps DSSS receive chain - see src/wifi_dsss_rx.hpp.
# The test's transmitter is written independently of the receiver so a
# pass means it decodes 802.11, not that two copies of the same mistake
# agree. Pure software, no hardware.
# ---------------------------------------------------------------------------
add_executable(test_wifi_dsss_rx
  tests/test_wifi_dsss_rx.cpp
  src/wifi_dsss_rx.cpp
  src/wifi_frame.cpp
)
target_include_directories(test_wifi_dsss_rx PRIVATE ${CMAKE_SOURCE_DIR}/src)

# ---------------------------------------------------------------------------
# Adversarial stress matrix for the 2.4GHz classifier - detection rate
# and misclassification rate vs SNR, plus a false-positive battery
# against carriers/noise. Uses spec-faithful signal generators that do
# NOT share formulas with the code under test (see the file header for
# why the original suite was circular). Pure software, no hardware.
# ---------------------------------------------------------------------------
add_executable(test_wifi_stress
  tests/test_wifi_stress.cpp
  src/wifi_phy.cpp
)
target_include_directories(test_wifi_stress PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_wifi_stress PRIVATE PkgConfig::KISSFFT)

# ---------------------------------------------------------------------------
# Synthetic correctness test for RF fingerprint extraction - see
# src/fingerprint.hpp. Pure software, no hardware.
# ---------------------------------------------------------------------------
add_executable(test_fingerprint
  tests/test_fingerprint.cpp
  src/fingerprint.cpp
)
target_include_directories(test_fingerprint PRIVATE ${CMAKE_SOURCE_DIR}/src)

# ---------------------------------------------------------------------------
# Synthetic correctness test for Wi-Fi RF fingerprint extraction - see
# src/wifi_fingerprint.hpp. A separate engine from the LoRa one above
# (different PHY, different preamble structure) - see that file's own
# header for why. Pure software, no hardware.
# ---------------------------------------------------------------------------
add_executable(test_wifi_fingerprint
  tests/test_wifi_fingerprint.cpp
  src/wifi_fingerprint.cpp
  src/wifi_phy.cpp
  src/wifi_dsss_rx.cpp
  src/wifi_frame.cpp
)
target_include_directories(test_wifi_fingerprint PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_wifi_fingerprint PRIVATE PkgConfig::KISSFFT)

# ---------------------------------------------------------------------------
# Correctness test for WifiMasterList - persistence and MAC-vs-
# fingerprint-cluster matching. Pure software, no hardware. Points at
# temp directories rather than PROJECT_ROOT_DIR (mirrors
# test_lora_master's own reasoning), so no compile definition needed.
# ---------------------------------------------------------------------------
add_executable(test_wifi_master
  tests/test_wifi_master.cpp
  src/wifi_master.cpp
)
target_include_directories(test_wifi_master PRIVATE ${CMAKE_SOURCE_DIR}/src)

# ---------------------------------------------------------------------------
# Correctness test for DeviceRegistry - permanence and RF-fingerprint
# matching. Pure software, no hardware.
# ---------------------------------------------------------------------------
add_executable(test_registry
  tests/test_registry.cpp
  src/registry.cpp
)
target_include_directories(test_registry PRIVATE ${CMAKE_SOURCE_DIR}/src)

# ---------------------------------------------------------------------------
# Correctness test for the persistent cross-run LoRa master emitter
# list - see src/lora_master.hpp. Pure software, no hardware; points at
# temp directories rather than PROJECT_ROOT_DIR, so no compile
# definition needed here (see lora_master_.cpp's constructor - it takes
# the directory as a plain argument).
# ---------------------------------------------------------------------------
add_executable(test_lora_master
  tests/test_lora_master.cpp
  src/lora_master.cpp
  src/fingerprint.cpp
)
target_include_directories(test_lora_master PRIVATE ${CMAKE_SOURCE_DIR}/src)

# ---------------------------------------------------------------------------
# Validation spike (see tools/sf_estimator_spike.cpp) for a proposed
# non-brute-force SF/BW estimator - NOT wired into the production
# scanner. Needs the real X310 + a live TarangMini burst to be useful.
# ---------------------------------------------------------------------------
add_executable(sf_estimator_spike
  tools/sf_estimator_spike.cpp
  src/sdr_capture.cpp
  src/spectrum.cpp
  src/detector.cpp
  src/classifier.cpp
  src/wifi_phy.cpp
  src/lora_phy.cpp
  src/lora_phy_std.cpp
)
target_include_directories(sf_estimator_spike PRIVATE ${CMAKE_SOURCE_DIR}/src ${UHD_INCLUDE_DIRS})
target_link_libraries(sf_estimator_spike PRIVATE PkgConfig::KISSFFT Threads::Threads dl ${UHD_LIBRARIES})

# ---------------------------------------------------------------------------
# Manual hardware validation tool (see tools/band_smoke_test.cpp) - not
# part of the GUI, needs the real B210 and a couple minutes to run.
# ---------------------------------------------------------------------------
add_executable(band_smoke_test
  tools/band_smoke_test.cpp
  src/config.hpp
  src/scanner.cpp
  src/sdr_capture.cpp
  src/registry.cpp
  src/spectrum.cpp
  src/detector.cpp
  src/classifier.cpp
  src/lora_phy.cpp
  src/lora_phy_std.cpp
  src/wifi_phy.cpp
  src/wifi_frame.cpp
  src/wifi_dsss_rx.cpp
  src/wifi_fingerprint.cpp
  src/wifi_master.cpp
  src/fingerprint.cpp
  src/lora_master.cpp
)
target_include_directories(band_smoke_test PRIVATE ${CMAKE_SOURCE_DIR}/src ${UHD_INCLUDE_DIRS})
target_link_libraries(band_smoke_test PRIVATE PkgConfig::KISSFFT Threads::Threads dl ${UHD_LIBRARIES})
# See rf_monitor_gui's own compile definition above for why.
target_compile_definitions(band_smoke_test PRIVATE PROJECT_ROOT_DIR="${CMAKE_SOURCE_DIR}")

# Wi-Fi identity persistence uses a real JSON parser for arbitrary SSID/WPS strings.
find_path(WIFI_JSON_INCLUDE_DIR nlohmann/json.hpp)
if(NOT WIFI_JSON_INCLUDE_DIR)
  message(FATAL_ERROR "Wi-Fi identity storage requires nlohmann JSON >= 3.9 (nlohmann-json3-dev)")
endif()
foreach(target rf_monitor_gui band_smoke_test test_wifi_master)
  target_include_directories(${target} PRIVATE ${WIFI_JSON_INCLUDE_DIR})
endforeach()
# Application sources already include wifi_vendor.cpp via APP_SOURCES.
foreach(target band_smoke_test test_wifi_master)
  target_sources(${target} PRIVATE src/wifi_vendor.cpp)
endforeach()
target_sources(test_wifi_master PRIVATE src/wifi_frame.cpp)

add_executable(test_wifi_identity tests/test_wifi_identity.cpp src/wifi_frame.cpp
               src/wifi_master.cpp src/wifi_vendor.cpp)
target_include_directories(test_wifi_identity PRIVATE ${CMAKE_SOURCE_DIR}/src ${WIFI_JSON_INCLUDE_DIR})

# Optional offscreen validation of the actual Wi-Fi GUI (no radio or display server).
find_path(WIFI_EGL_INCLUDE_DIR EGL/egl.h)
find_library(WIFI_EGL_LIBRARY EGL)
if(WIFI_EGL_INCLUDE_DIR AND WIFI_EGL_LIBRARY)
  add_executable(test_wifi_gui tests/test_wifi_gui.cpp src/wifi_gui.cpp src/wifi_frame.cpp
      ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp
      ${IMGUI_DIR}/imgui_widgets.cpp ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
  target_include_directories(test_wifi_gui PRIVATE ${CMAKE_SOURCE_DIR}/src ${IMGUI_DIR} ${WIFI_EGL_INCLUDE_DIR})
  target_link_libraries(test_wifi_gui PRIVATE OpenGL::GL ${WIFI_EGL_LIBRARY} dl)
endif()
