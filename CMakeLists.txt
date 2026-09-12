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
# Synthetic correctness test for RF fingerprint extraction - see
# src/fingerprint.hpp. Pure software, no hardware.
# ---------------------------------------------------------------------------
add_executable(test_fingerprint
  tests/test_fingerprint.cpp
  src/fingerprint.cpp
)
target_include_directories(test_fingerprint PRIVATE ${CMAKE_SOURCE_DIR}/src)

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
  src/fingerprint.cpp
)
target_include_directories(band_smoke_test PRIVATE ${CMAKE_SOURCE_DIR}/src ${UHD_INCLUDE_DIRS})
target_link_libraries(band_smoke_test PRIVATE PkgConfig::KISSFFT Threads::Threads dl ${UHD_LIBRARIES})
