cmake_minimum_required(VERSION 3.21)

# Development / IDE build of the whole repository in one CMake project: the
# Qt6 GUI app (via add_subdirectory(Qt6)), every headless tool and probe
# that tools/build_*.sh builds by hand, and the Core/ test suite as a real
# CTest target. Opening the repo root in CLion (or any CMake-aware IDE)
# therefore gives run/debug configurations for all of them -- see the
# shared ones in .run/ and the "CLion / IDE Integration" section of
# docs/Building.md.
#
# This is NOT the release entry point. Qt6/CMakeLists.txt stays the
# authoritative app build: build_and_run.sh, docs/Building.md's per-platform
# recipes and every packaging job in .github/workflows/build.yml configure
# `-S Qt6`, and this file only pulls that same directory in unchanged.
#
# The tool/probe/test source lists below duplicate the ones hand-maintained
# in tools/build_*.sh and tools/run_tests.sh, so they can drift. The
# `core-tests` job in .github/workflows/build.yml configures *this* file and
# runs CTest to catch that -- if you add a Core/ source, keep both in sync.

set(CMAKE_OSX_DEPLOYMENT_TARGET "15.8" CACHE STRING "Minimum macOS version")

if(APPLE)
  project(Calc-U-1600 LANGUAGES CXX OBJCXX)
else()
  project(Calc-U-1600 LANGUAGES CXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# No find_package(Qt6) here: only Qt6/CMakeLists.txt needs Qt, and its own
# macOS Homebrew-prefix fallback (which also has to work when that directory
# is configured directly, as every release build does) covers this build too.

set(CORE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/Core)
set(TOOLS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tools)

# Preset loading (libsharpdx)
if(WIN32)
  set(SHARPDX_LIB ${CORE_ROOT}/Basic/vendor/sharpdx/sharpdx.lib)
elseif(APPLE)
  set(SHARPDX_LIB ${CORE_ROOT}/Basic/vendor/sharpdx/libsharpdx.a)
else()
  set(SHARPDX_LIB ${CORE_ROOT}/Basic/vendor/sharpdx/libsharpdx-linux.a)
endif()

# Core sources used by tools and tests
set(CORE_COMMON_SOURCES
  ${CORE_ROOT}/Debug/BasicPointerTable.cpp
  ${CORE_ROOT}/CPU/LH5801/LH5801.cpp
  ${CORE_ROOT}/CPU/SC7852/SC7852.cpp
  ${CORE_ROOT}/CPU/LH5803/LH5803Memory.cpp
  ${CORE_ROOT}/CPU/LH5803/LH5803SharedMemory.cpp
  ${CORE_ROOT}/Audio/PiezoSampler.cpp
  ${CORE_ROOT}/PC1500/PC1500Memory.cpp
  ${CORE_ROOT}/PC1500/PC1500Machine.cpp
  ${CORE_ROOT}/PC1500/PC1500Keyboard.cpp
  ${CORE_ROOT}/PC1500/PC1500Display.cpp
  ${CORE_ROOT}/PC1500/Upd1990ac.cpp
  ${CORE_ROOT}/PC1500/PC1500TraceFile.cpp
  ${CORE_ROOT}/PC1600/PC1600Memory.cpp
  ${CORE_ROOT}/PC1600/PC1600SubCpu.cpp
  ${CORE_ROOT}/PC1600/TC8576F.cpp
  ${CORE_ROOT}/Serial/PtySerialLink.cpp
  ${CORE_ROOT}/PC1600/PC1600Display.cpp
  ${CORE_ROOT}/PC1600/PC1600Keyboard.cpp
  ${CORE_ROOT}/PC1600/PC1600Machine.cpp
  ${CORE_ROOT}/Display/LcdScreenshot.cpp
  ${CORE_ROOT}/KeyPaste.cpp
  ${CORE_ROOT}/MachineCodeFile.cpp
  ${CORE_ROOT}/PC1500/PC1500MachineCodeLoader.cpp
  ${CORE_ROOT}/PC1600/PC1600MachineImage.cpp
  ${CORE_ROOT}/PC1600/PC1600MachineCodeLoader.cpp
  ${CORE_ROOT}/PC1600/PC1600ProgramPlacement.cpp
  ${CORE_ROOT}/PC1500/PC1500BasicTyper.cpp
  ${CORE_ROOT}/PC1600/PC1600BasicTyper.cpp
)

if(EXISTS ${SHARPDX_LIB})
  list(APPEND CORE_COMMON_SOURCES
    ${CORE_ROOT}/Preset/PresetFile.cpp
    ${CORE_ROOT}/Preset/PresetRunner.cpp
    ${CORE_ROOT}/PC1500/PC1500BasicLoader.cpp
    ${CORE_ROOT}/PC1500/PC1500PresetLoader.cpp
    ${CORE_ROOT}/PC1600/PC1600BasicLoader.cpp
    ${CORE_ROOT}/PC1600/PC1600PresetLoader.cpp
    ${CORE_ROOT}/Basic/BasicBinaryImage.cpp
    ${CORE_ROOT}/Basic/BasicProgramSource.cpp
  )
endif()

# -----------------------------------------------------------------------------
# Qt6 GUI Application target
# -----------------------------------------------------------------------------
add_subdirectory(Qt6)

# -----------------------------------------------------------------------------
# Headless CLI tools & probes
# -----------------------------------------------------------------------------
# pc1500_cli
set(PC1500_CLI_SOURCES
  ${CORE_ROOT}/CPU/LH5801/LH5801.cpp
  ${CORE_ROOT}/Audio/PiezoSampler.cpp
  ${CORE_ROOT}/PC1500/PC1500Memory.cpp
  ${CORE_ROOT}/PC1500/PC1500Machine.cpp
  ${CORE_ROOT}/PC1500/PC1500Keyboard.cpp
  ${CORE_ROOT}/PC1500/PC1500Display.cpp
  ${CORE_ROOT}/PC1500/Upd1990ac.cpp
  ${CORE_ROOT}/Preset/PresetFile.cpp
  ${CORE_ROOT}/Preset/PresetRunner.cpp
  ${CORE_ROOT}/PC1500/PC1500BasicTyper.cpp
  ${CORE_ROOT}/PC1500/PC1500BasicLoader.cpp
  ${CORE_ROOT}/PC1500/PC1500PresetLoader.cpp
  ${CORE_ROOT}/PC1500/PC1500MachineCodeLoader.cpp
  ${CORE_ROOT}/Display/LcdScreenshot.cpp
  ${CORE_ROOT}/Basic/BasicBinaryImage.cpp
  ${CORE_ROOT}/Basic/BasicProgramSource.cpp
  ${CORE_ROOT}/PC1500/PC1500TraceFile.cpp
  ${CORE_ROOT}/Serial/PtySerialLink.cpp
  ${TOOLS_ROOT}/pc1500_cli.cpp
)
add_executable(pc1500_cli ${PC1500_CLI_SOURCES})
target_include_directories(pc1500_cli PRIVATE ${CORE_ROOT})
if(EXISTS ${SHARPDX_LIB})
  target_link_libraries(pc1500_cli PRIVATE ${SHARPDX_LIB})
endif()

# pc1600_cli
set(PC1600_CLI_SOURCES
  ${CORE_ROOT}/CPU/LH5801/LH5801.cpp
  ${CORE_ROOT}/CPU/SC7852/SC7852.cpp
  ${CORE_ROOT}/CPU/LH5803/LH5803Memory.cpp
  ${CORE_ROOT}/CPU/LH5803/LH5803SharedMemory.cpp
  ${CORE_ROOT}/Preset/PresetFile.cpp
  ${CORE_ROOT}/Preset/PresetRunner.cpp
  ${CORE_ROOT}/PC1500/PC1500Keyboard.cpp
  ${CORE_ROOT}/PC1500/PC1500TraceFile.cpp
  ${CORE_ROOT}/Audio/PiezoSampler.cpp
  ${CORE_ROOT}/PC1600/PC1600Memory.cpp
  ${CORE_ROOT}/PC1600/PC1600SubCpu.cpp
  ${CORE_ROOT}/PC1600/TC8576F.cpp
  ${CORE_ROOT}/Serial/PtySerialLink.cpp
  ${CORE_ROOT}/PC1600/PC1600Display.cpp
  ${CORE_ROOT}/PC1600/PC1600Keyboard.cpp
  ${CORE_ROOT}/PC1600/PC1600Machine.cpp
  ${CORE_ROOT}/PC1600/PC1600BasicTyper.cpp
  ${CORE_ROOT}/PC1600/PC1600BasicLoader.cpp
  ${CORE_ROOT}/PC1600/PC1600ProgramPlacement.cpp
  ${CORE_ROOT}/PC1600/PC1600MachineImage.cpp
  ${CORE_ROOT}/PC1600/PC1600MachineCodeLoader.cpp
  ${CORE_ROOT}/PC1600/PC1600PresetLoader.cpp
  ${CORE_ROOT}/Display/LcdScreenshot.cpp
  ${CORE_ROOT}/Basic/BasicBinaryImage.cpp
  ${CORE_ROOT}/Basic/BasicProgramSource.cpp
  ${TOOLS_ROOT}/pc1600_cli.cpp
)
add_executable(pc1600_cli ${PC1600_CLI_SOURCES})
target_include_directories(pc1600_cli PRIVATE ${CORE_ROOT})
if(EXISTS ${SHARPDX_LIB})
  target_link_libraries(pc1600_cli PRIVATE ${SHARPDX_LIB})
endif()

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
  ${CORE_ROOT}/PC1500/PC1500TraceFile.cpp
  ${TOOLS_ROOT}/pc1600_uart_probe.cpp
)
add_executable(pc1600_uart_probe ${PC1600_UART_PROBE_SOURCES})
target_include_directories(pc1600_uart_probe PRIVATE ${CORE_ROOT})
target_compile_definitions(pc1600_uart_probe PRIVATE PC1600_POWER_PROBE)

# pc1600_plotter_probe
set(PC1600_PLOTTER_PROBE_SOURCES
  ${CORE_ROOT}/CPU/LH5801/LH5801.cpp
  ${CORE_ROOT}/Audio/PiezoSampler.cpp
  ${CORE_ROOT}/PC1500/PC1500Memory.cpp
  ${CORE_ROOT}/PC1500/PC1500Machine.cpp
  ${CORE_ROOT}/PC1500/PC1500Keyboard.cpp
  ${CORE_ROOT}/PC1500/PC1500Display.cpp
  ${CORE_ROOT}/PC1500/Upd1990ac.cpp
  ${CORE_ROOT}/Preset/PresetFile.cpp
  ${CORE_ROOT}/Preset/PresetRunner.cpp
  ${CORE_ROOT}/PC1500/PC1500BasicTyper.cpp
  ${CORE_ROOT}/PC1500/PC1500PresetLoader.cpp
  ${CORE_ROOT}/PC1500/PC1500TraceFile.cpp
  ${CORE_ROOT}/PC1600/PC1600Memory.cpp
  ${CORE_ROOT}/PC1600/PC1600Keyboard.cpp
  ${CORE_ROOT}/PC1600/PC1600Display.cpp
  ${CORE_ROOT}/PC1600/PC1600SubCpu.cpp
  ${CORE_ROOT}/PC1600/TC8576F.cpp
  ${CORE_ROOT}/CPU/SC7852/SC7852.cpp
  ${CORE_ROOT}/CPU/LH5803/LH5803Memory.cpp
  ${CORE_ROOT}/CPU/LH5803/LH5803SharedMemory.cpp
  ${CORE_ROOT}/PC1600/PC1600Machine.cpp
  ${CORE_ROOT}/PC1600/PC1600BasicTyper.cpp
  ${CORE_ROOT}/PC1600/PC1600MachineImage.cpp
  ${CORE_ROOT}/PC1600/PC1600MachineCodeLoader.cpp
  ${CORE_ROOT}/PC1600/PC1600PresetLoader.cpp
  ${CORE_ROOT}/Display/LcdScreenshot.cpp
  ${CORE_ROOT}/Basic/BasicBinaryImage.cpp
  ${CORE_ROOT}/Basic/BasicProgramSource.cpp
  ${CORE_ROOT}/PC1500/PC1500BasicLoader.cpp
  ${CORE_ROOT}/PC1600/PC1600BasicLoader.cpp
  ${CORE_ROOT}/PC1600/PC1600ProgramPlacement.cpp
  ${CORE_ROOT}/PC1500/PC1500MachineCodeLoader.cpp
  ${TOOLS_ROOT}/pc1600_plotter_probe.cpp
)
add_executable(pc1600_plotter_probe ${PC1600_PLOTTER_PROBE_SOURCES})
target_include_directories(pc1600_plotter_probe PRIVATE ${CORE_ROOT})
if(EXISTS ${SHARPDX_LIB})
  target_link_libraries(pc1600_plotter_probe PRIVATE ${SHARPDX_LIB})
endif()

# -----------------------------------------------------------------------------
# Test Suite target
# -----------------------------------------------------------------------------
find_package(ZLIB REQUIRED)

set(TEST_SOURCES
  ${CORE_COMMON_SOURCES}
  ${CORE_ROOT}/tests/lh5801_tests.cpp
  ${CORE_ROOT}/tests/connector_tests.cpp
  ${CORE_ROOT}/tests/ce155_tests.cpp
  ${CORE_ROOT}/tests/ce1638_tests.cpp
  ${CORE_ROOT}/tests/ce502b_tests.cpp
  ${CORE_ROOT}/tests/ce163f_tests.cpp
  ${CORE_ROOT}/tests/memory_card_tests.cpp
  ${CORE_ROOT}/tests/battery_card_instance_tests.cpp
  ${CORE_ROOT}/tests/preset_tests.cpp
  ${CORE_ROOT}/tests/basictyper_tests.cpp
  ${CORE_ROOT}/tests/presetloader_trace_tests.cpp
  ${CORE_ROOT}/tests/pc1600_basictyper_tests.cpp
  ${CORE_ROOT}/tests/pc1600_bank_tests.cpp
  ${CORE_ROOT}/tests/sc7852_tests.cpp
  ${CORE_ROOT}/tests/tc8576f_tests.cpp
  ${CORE_ROOT}/tests/lh5803_tests.cpp
  ${CORE_ROOT}/tests/pc1600_machine_tests.cpp
  ${CORE_ROOT}/tests/pc1600_phase54_tests.cpp
  ${CORE_ROOT}/tests/pc1600_keyboard_display_tests.cpp
  ${CORE_ROOT}/tests/pc1600_slot_ram_tests.cpp
  ${CORE_ROOT}/tests/pc1600_slot_module_tests.cpp
  ${CORE_ROOT}/tests/pc1600_preset_tests.cpp
  ${CORE_ROOT}/tests/ce1600p_tests.cpp
  ${CORE_ROOT}/tests/ce1600f_tests.cpp
  ${CORE_ROOT}/tests/ce150_tests.cpp
  ${CORE_ROOT}/tests/pc1600_ce150_tests.cpp
  ${CORE_ROOT}/tests/ce158_tests.cpp
  ${CORE_ROOT}/tests/basic_binary_image_tests.cpp
  ${CORE_ROOT}/tests/basic_fastloader_tests.cpp
  ${CORE_ROOT}/tests/pc1600_basicloader_tests.cpp
  ${CORE_ROOT}/tests/basic_program_source_tests.cpp
  ${CORE_ROOT}/tests/pc1600_program_placement_tests.cpp
  ${CORE_ROOT}/tests/pc1600_machine_image_tests.cpp
  ${CORE_ROOT}/tests/piezo_sampler_tests.cpp
  ${CORE_ROOT}/tests/key_paste_tests.cpp
  ${CORE_ROOT}/tests/lcd_screenshot_tests.cpp
  ${CORE_ROOT}/tests/machine_code_file_tests.cpp
)

add_executable(CoreTests ${TEST_SOURCES})
target_include_directories(CoreTests PRIVATE ${CORE_ROOT})
target_link_libraries(CoreTests PRIVATE ZLIB::ZLIB)
if(EXISTS ${SHARPDX_LIB})
  target_link_libraries(CoreTests PRIVATE ${SHARPDX_LIB})
endif()

enable_testing()
add_test(NAME CoreTests COMMAND CoreTests WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
