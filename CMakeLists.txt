cmake_minimum_required(VERSION 3.21)

# Development / IDE build of the whole repository in one CMake project: the
# calcu_core / calcu_shared libraries (cmake/CalcU.cmake), the Qt6 GUI app
# (via add_subdirectory(Qt6)), the headless CLIs and probes, and the
# Core/ + Shared/ test suite as a real CTest target. Opening the repo root
# in CLion (or any CMake-aware IDE) therefore gives run/debug
# configurations for all of them -- see the shared ones in .run/ and the
# "CLion / IDE Integration" section of docs/developer/Building.md.
#
# This is NOT the release entry point. Qt6/CMakeLists.txt stays the
# authoritative app build: build_and_run.sh, docs/developer/Building.md's
# per-platform recipes and every packaging job in .github/workflows/build.yml
# configure `-S Qt6`, and this file only pulls that same directory in.
#
# Core/ and Shared/ sources are globbed by cmake/CalcU.cmake; only the
# probes (which need PC1600_POWER_PROBE) and tools/build_*.sh still list
# Core sources by hand.
#
# Options:
#   CALCU_BUILD_QT     build the Qt6 app (default ON); tools/run_tests.sh
#                      turns it off so the tests need no Qt
#   CALCU_TESTS_DEBUG  build Core and the tests at -O0 for stepping through

set(CMAKE_OSX_DEPLOYMENT_TARGET "15.8" CACHE STRING "Minimum macOS version")

if(APPLE)
  project(Calc-U-1600 LANGUAGES CXX OBJCXX)
else()
  project(Calc-U-1600 LANGUAGES CXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# No find_package(Qt6) here: only Qt6/CMakeLists.txt needs Qt, and its own
# macOS Homebrew-prefix fallback (which also has to work when that directory
# is configured directly, as every release build does) covers this build too.

set(CORE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/Core)
set(TOOLS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tools)

# The suite runs ~3x faster at -O1 than at -O0, so a Debug build compiles
# Core and the tests at -O1 (CI builds RelWithDebInfo). To step through a
# failing test, configure a separate full-debug tree:
#   cmake -S . -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCALCU_TESTS_DEBUG=ON
option(CALCU_TESTS_DEBUG "Build Core and CoreTests at -O0 for debugging" OFF)
option(CALCU_BUILD_QT "Build the Qt6 GUI app" ON)
if(NOT CALCU_TESTS_DEBUG)
  set(CALCU_CORE_DEBUG_O1 ON)
endif()
include(cmake/CalcU.cmake)

# -----------------------------------------------------------------------------
# Qt6 GUI Application target
# -----------------------------------------------------------------------------
if(CALCU_BUILD_QT)
  add_subdirectory(Qt6)
endif()

# -----------------------------------------------------------------------------
# Headless CLI tools & probes
# -----------------------------------------------------------------------------
# pc1500_cli
add_executable(pc1500_cli ${TOOLS_ROOT}/pc1500_cli.cpp)
target_link_libraries(pc1500_cli PRIVATE calcu_core)

# pc1600_cli
add_executable(pc1600_cli ${TOOLS_ROOT}/pc1600_cli.cpp)
target_link_libraries(pc1600_cli PRIVATE calcu_core)

# pc1600_power_probe
set(PC1600_POWER_PROBE_SOURCES
  ${CORE_ROOT}/CPU/LH5801/LH5801.cpp
  ${CORE_ROOT}/CPU/SC7852/SC7852.cpp
  ${CORE_ROOT}/CPU/LH5803/LH5803Memory.cpp
  ${CORE_ROOT}/CPU/LH5803/LH5803SharedMemory.cpp
  ${CORE_ROOT}/PC1500/PC1500TraceFile.cpp
  ${CORE_ROOT}/Audio/PiezoSampler.cpp
  ${CORE_ROOT}/PC1600/PC1600Memory.cpp
  ${CORE_ROOT}/PC1600/PC1600Keyboard.cpp
  ${CORE_ROOT}/PC1600/PC1600Display.cpp
  ${CORE_ROOT}/PC1600/PC1600SubCpu.cpp
  ${CORE_ROOT}/PC1600/TC8576F.cpp
  ${CORE_ROOT}/PC1600/PC1600Machine.cpp
  ${TOOLS_ROOT}/pc1600_power_probe.cpp
)
add_executable(pc1600_power_probe ${PC1600_POWER_PROBE_SOURCES})
target_include_directories(pc1600_power_probe PRIVATE ${CORE_ROOT})
target_compile_definitions(pc1600_power_probe PRIVATE PC1600_POWER_PROBE)

# pc1600_uart_probe
set(PC1600_UART_PROBE_SOURCES
  ${CORE_ROOT}/CPU/LH5801/LH5801.cpp
  ${CORE_ROOT}/CPU/SC7852/SC7852.cpp
  ${CORE_ROOT}/CPU/LH5803/LH5803Memory.cpp
  ${CORE_ROOT}/CPU/LH5803/LH5803SharedMemory.cpp
  ${CORE_ROOT}/Audio/PiezoSampler.cpp
  ${CORE_ROOT}/PC1600/PC1600Memory.cpp
  ${CORE_ROOT}/PC1600/PC1600Keyboard.cpp
  ${CORE_ROOT}/PC1600/PC1600Display.cpp
  ${CORE_ROOT}/PC1600/PC1600SubCpu.cpp
  ${CORE_ROOT}/PC1600/TC8576F.cpp
  ${CORE_ROOT}/Serial/PtySerialLink.cpp
  ${CORE_ROOT}/PC1600/PC1600Machine.cpp
  ${CORE_ROOT}/PC1600/PC1600BasicTyper.cpp
  ${CORE_ROOT}/PC1600/PC1600ProgramPlacement.cpp
  ${CORE_ROOT}/PC1500/PC1500TraceFile.cpp
  ${TOOLS_ROOT}/pc1600_uart_probe.cpp
)
add_executable(pc1600_uart_probe ${PC1600_UART_PROBE_SOURCES})
target_include_directories(pc1600_uart_probe PRIVATE ${CORE_ROOT})
target_compile_definitions(pc1600_uart_probe PRIVATE PC1600_POWER_PROBE)

# pc1600_plotter_probe
add_executable(pc1600_plotter_probe ${TOOLS_ROOT}/pc1600_plotter_probe.cpp)
target_link_libraries(pc1600_plotter_probe PRIVATE calcu_core)

# -----------------------------------------------------------------------------
# Test Suite target
# -----------------------------------------------------------------------------
find_package(ZLIB REQUIRED)

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS
  ${CORE_ROOT}/tests/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/Shared/tests/*.cpp
)

add_executable(CoreTests ${TEST_SOURCES})
target_link_libraries(CoreTests PRIVATE calcu_shared ZLIB::ZLIB)
target_compile_options(CoreTests PRIVATE ${CALCU_WARNINGS})
if(NOT CALCU_TESTS_DEBUG AND NOT MSVC)
  target_compile_options(CoreTests PRIVATE $<$<CONFIG:Debug>:-O1>)
endif()

enable_testing()
add_test(NAME CoreTests COMMAND CoreTests WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
