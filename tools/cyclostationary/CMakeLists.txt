cmake_minimum_required(VERSION 3.16)

# Can also build alone: no GUI, UHD or scanner dependency in this milestone.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(rfmon_cyclostationary_offline LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  enable_testing()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(KISSFFT REQUIRED IMPORTED_TARGET kissfft-float)
  add_link_options(-B/usr/bin)
endif()

get_filename_component(CYCLO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
find_path(CYCLO_JSON_INCLUDE nlohmann/json.hpp REQUIRED)
find_package(Threads REQUIRED)
add_library(rfmon_cyclo_core STATIC
  ${CYCLO_ROOT}/src/cyclostationary/iq_file.cpp
  ${CYCLO_ROOT}/src/cyclostationary/analysis_samples.cpp
  ${CYCLO_ROOT}/src/cyclostationary/ofdm_structure.cpp
  ${CYCLO_ROOT}/src/cyclostationary/chirp_structure.cpp
  ${CYCLO_ROOT}/src/cyclostationary/link_evidence.cpp
  ${CYCLO_ROOT}/src/cyclostationary/roi_measurements.cpp
  ${CYCLO_ROOT}/src/cyclostationary/worker_protocol.cpp
  ${CYCLO_ROOT}/src/cyclostationary/spectral_correlation.cpp)
target_include_directories(rfmon_cyclo_core PUBLIC ${CYCLO_ROOT}/src)
target_link_libraries(rfmon_cyclo_core PUBLIC PkgConfig::KISSFFT)
target_compile_options(rfmon_cyclo_core PRIVATE -Wall -Wextra -Wpedantic)
add_library(rfmon_cyclo_transport STATIC ${CYCLO_ROOT}/src/cyclostationary/process_client.cpp)
target_link_libraries(rfmon_cyclo_transport PUBLIC rfmon_cyclo_core Threads::Threads)
target_compile_options(rfmon_cyclo_transport PRIVATE -Wall -Wextra -Wpedantic)
add_executable(wifi_cyclo_worker ${CYCLO_ROOT}/tools/wifi_cyclo_worker.cpp)
target_link_libraries(wifi_cyclo_worker PRIVATE rfmon_cyclo_transport)
add_library(rfmon_cyclo_offline STATIC ${CYCLO_ROOT}/src/cyclostationary/shadow_worker.cpp)
target_link_libraries(rfmon_cyclo_offline PUBLIC rfmon_cyclo_transport)
target_compile_options(rfmon_cyclo_offline PRIVATE -Wall -Wextra -Wpedantic)
add_dependencies(rfmon_cyclo_offline wifi_cyclo_worker)

add_executable(wifi_cyclo_replay ${CYCLO_ROOT}/tools/wifi_cyclo_replay.cpp)
target_include_directories(wifi_cyclo_replay PRIVATE ${CYCLO_JSON_INCLUDE})
target_link_libraries(wifi_cyclo_replay PRIVATE rfmon_cyclo_offline)
target_compile_options(wifi_cyclo_replay PRIVATE -Wall -Wextra -Wpedantic)

add_executable(test_cyclostationary ${CYCLO_ROOT}/tests/test_cyclostationary.cpp)
target_link_libraries(test_cyclostationary PRIVATE rfmon_cyclo_offline)
add_test(NAME cyclostationary_math COMMAND test_cyclostationary)
add_executable(test_cyclo_roi ${CYCLO_ROOT}/tests/test_cyclo_roi.cpp)
target_link_libraries(test_cyclo_roi PRIVATE rfmon_cyclo_offline)
add_test(NAME cyclostationary_roi COMMAND test_cyclo_roi)
add_executable(test_cyclo_chirp ${CYCLO_ROOT}/tests/test_cyclo_chirp.cpp)
target_link_libraries(test_cyclo_chirp PRIVATE rfmon_cyclo_core)
add_test(NAME cyclostationary_chirp COMMAND test_cyclo_chirp)
add_executable(test_cyclo_evidence ${CYCLO_ROOT}/tests/test_cyclo_evidence.cpp)
target_link_libraries(test_cyclo_evidence PRIVATE rfmon_cyclo_core)
add_test(NAME cyclostationary_evidence COMMAND test_cyclo_evidence)
add_executable(test_cyclo_shadow ${CYCLO_ROOT}/tests/test_cyclo_shadow.cpp)
target_link_libraries(test_cyclo_shadow PRIVATE rfmon_cyclo_offline)
add_test(NAME cyclostationary_shadow COMMAND test_cyclo_shadow)
foreach(mode RANGE 1 4)
  add_executable(cyclo_fault_worker_${mode} ${CYCLO_ROOT}/tests/cyclo_fault_worker.cpp)
  target_compile_definitions(cyclo_fault_worker_${mode} PRIVATE CYCLO_FAULT_MODE=${mode})
  target_link_libraries(cyclo_fault_worker_${mode} PRIVATE rfmon_cyclo_transport)
endforeach()
add_executable(test_cyclo_process ${CYCLO_ROOT}/tests/test_cyclo_process.cpp)
target_link_libraries(test_cyclo_process PRIVATE rfmon_cyclo_offline)
add_dependencies(test_cyclo_process cyclo_fault_worker_1 cyclo_fault_worker_2 cyclo_fault_worker_3 cyclo_fault_worker_4)
add_test(NAME cyclostationary_process COMMAND test_cyclo_process $<TARGET_FILE:wifi_cyclo_worker>
  $<TARGET_FILE:cyclo_fault_worker_1> $<TARGET_FILE:cyclo_fault_worker_2>
  $<TARGET_FILE:cyclo_fault_worker_3> $<TARGET_FILE:cyclo_fault_worker_4>)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME cyclostationary_reference_corpus COMMAND ${Python3_EXECUTABLE}
    ${CYCLO_ROOT}/tests/test_cyclo_reference_corpus.py ${CYCLO_ROOT}/tests/fixtures/wifi_ofdm)
  add_test(NAME cyclostationary_cli COMMAND ${Python3_EXECUTABLE}
    ${CYCLO_ROOT}/tests/test_cyclo_cli.py $<TARGET_FILE:wifi_cyclo_replay>)
endif()
