# The emulation core and the frontend-neutral Shared/ layer as two static
# libraries, for every CMake build in the repository:
#
#   calcu_core    Core/  -- the hardware (CPUs, machines, cards, loaders, presets)
#   calcu_shared  Shared/ -- everything above Core that isn't drawing
#                 (docs/developer/Shared-Architecture.md); links calcu_core
#
# Included by the repo-root CMakeLists.txt and by Qt6/CMakeLists.txt (which
# must still configure on its own, `-S Qt6`, for the release builds). The
# NOT TARGET guard makes the second include in a root build a no-op.
#
# The source lists are globbed: every Core/**/*.cpp except Core/tests/ and
# every Shared/**/*.cpp except Shared/tests/. The probes in the root file
# compile their own Core lists, because they need PC1600_POWER_PROBE.
#
# Options the including file may set before include():
#   CALCU_CORE_DEBUG_O1  compile calcu_core at -O1 in Debug (the test suite
#                        runs ~3x faster); the root build sets it unless
#                        CALCU_TESTS_DEBUG is ON.

if(TARGET calcu_core)
  return()
endif()

get_filename_component(CALCU_ROOT ${CMAKE_CURRENT_LIST_DIR} DIRECTORY)
set(CALCU_CORE_ROOT ${CALCU_ROOT}/Core)
set(CALCU_SHARED_ROOT ${CALCU_ROOT}/Shared)

if(MSVC)
  set(CALCU_WARNINGS "")
else()
  set(CALCU_WARNINGS -Wall -Wextra)
endif()

# ---- libsharpdx: every program loader needs it (Core/Basic/vendor/sharpdx/README.md)
#  - macOS: libsharpdx.a is committed (tools/refresh_sharpdx.sh).
#  - Linux/Windows: fetched by tools/fetch_sharpdx.sh into libsharpdx-linux.a
#    / sharpdx.lib -- different names, so a fetch never dirties the
#    committed macOS binary.
#  - Emscripten: libsharpdx-wasm.a (tools/build_sharpdx_wasm.sh).
if(EMSCRIPTEN)
  set(SHARPDX_LIB ${CALCU_CORE_ROOT}/Basic/vendor/sharpdx/libsharpdx-wasm.a)
elseif(WIN32)
  set(SHARPDX_LIB ${CALCU_CORE_ROOT}/Basic/vendor/sharpdx/sharpdx.lib)
elseif(APPLE)
  set(SHARPDX_LIB ${CALCU_CORE_ROOT}/Basic/vendor/sharpdx/libsharpdx.a)
else()
  set(SHARPDX_LIB ${CALCU_CORE_ROOT}/Basic/vendor/sharpdx/libsharpdx-linux.a)
endif()
if(NOT EXISTS ${SHARPDX_LIB})
  message(FATAL_ERROR "${SHARPDX_LIB} is missing -- run tools/fetch_sharpdx.sh (Linux/Windows), "
                      "tools/refresh_sharpdx.sh (macOS) or tools/build_sharpdx_wasm.sh (Emscripten)")
endif()

add_library(calcu_sharpdx INTERFACE)
target_link_libraries(calcu_sharpdx INTERFACE ${SHARPDX_LIB})
# The vendored/fetched libs are sharpdx's serial-free build, so there's no
# libudev (Linux), IOKit (macOS) or serialport import lib (Windows) to link
# -- only Rust std's own Windows system libs, below.
if(WIN32)
  # `cargo build` links the Windows system import libs Rust's std needs
  # (sockets, CSPRNG seeding, futex-based sync, ...) into sde.exe
  # automatically; consuming the raw sharpdx.lib from CMake doesn't, so
  # without this the link fails with unresolved externals such as
  # __imp_freeaddrinfo (ws2_32). The authoritative list is the one rustc
  # reported for that exact build: SharpDataExchange's release packaging
  # captures `rustc --print=native-static-libs` into
  # native-libs-windows.txt and tools/fetch_sharpdx.sh vendors it next to
  # sharpdx.lib. Only a hand-placed sharpdx.lib without that file falls
  # back to the hardcoded guess.
  #
  # On *-pc-windows-msvc rustc's note is MSVC linker syntax (bare
  # `name.lib` names and the odd `/defaultlib:x`), not `-lname`, so it goes
  # in as raw link options: target_link_libraries would look for a library
  # literally named "/defaultlib:msvcrt" and fail.
  set(SHARPDX_NATIVE_LIBS_FILE ${CALCU_CORE_ROOT}/Basic/vendor/sharpdx/native-libs-windows.txt)
  if(EXISTS ${SHARPDX_NATIVE_LIBS_FILE})
    file(READ ${SHARPDX_NATIVE_LIBS_FILE} SHARPDX_NATIVE_LIBS_RAW)
    string(STRIP "${SHARPDX_NATIVE_LIBS_RAW}" SHARPDX_NATIVE_LIBS_RAW)
    separate_arguments(SHARPDX_NATIVE_LINK_OPTS UNIX_COMMAND "${SHARPDX_NATIVE_LIBS_RAW}")
    message(STATUS "sharpdx: linking native libs from native-libs-windows.txt: ${SHARPDX_NATIVE_LINK_OPTS}")
    target_link_options(calcu_sharpdx INTERFACE ${SHARPDX_NATIVE_LINK_OPTS})
  else()
    set(SHARPDX_NATIVE_LIBS_FALLBACK ws2_32 userenv advapi32 bcrypt ntdll synchronization)
    message(STATUS "sharpdx: native-libs-windows.txt not found, using hardcoded fallback: ${SHARPDX_NATIVE_LIBS_FALLBACK}")
    target_link_libraries(calcu_sharpdx INTERFACE ${SHARPDX_NATIVE_LIBS_FALLBACK})
  endif()
endif()

# ---- calcu_core
file(GLOB_RECURSE CALCU_CORE_SOURCES CONFIGURE_DEPENDS ${CALCU_CORE_ROOT}/*.cpp)
list(FILTER CALCU_CORE_SOURCES EXCLUDE REGEX "^${CALCU_CORE_ROOT}/tests/")
add_library(calcu_core STATIC ${CALCU_CORE_SOURCES})
target_include_directories(calcu_core PUBLIC ${CALCU_CORE_ROOT})
target_compile_features(calcu_core PUBLIC cxx_std_20)
target_compile_options(calcu_core PRIVATE ${CALCU_WARNINGS})
# The sources are UTF-8 without a BOM (U'á' literals, PC1600TypedInput.hpp);
# MSVC would read them in the ANSI codepage (C2015). Targets linking Qt got
# /utf-8 from Qt6::Platform; Core and Shared don't link Qt, so set it here --
# PUBLIC, since everything that includes Core's headers needs it too.
if(MSVC)
  target_compile_options(calcu_core PUBLIC /utf-8)
endif()
target_link_libraries(calcu_core PUBLIC calcu_sharpdx)
if(CALCU_CORE_DEBUG_O1 AND NOT MSVC)
  target_compile_options(calcu_core PRIVATE $<$<CONFIG:Debug>:-O1>)
endif()

# ---- calcu_shared
# Shared/ is C++20 and standard-library-only: no Qt, Cocoa or Emscripten
# headers, and single-threaded (docs/developer/Shared-Architecture.md).
# Headers include their Core dependencies relative to Core/ ("PC1600/...")
# and each other as "Shared/...".
file(GLOB_RECURSE CALCU_SHARED_SOURCES CONFIGURE_DEPENDS ${CALCU_SHARED_ROOT}/*.cpp)
list(FILTER CALCU_SHARED_SOURCES EXCLUDE REGEX "^${CALCU_SHARED_ROOT}/tests/")
add_library(calcu_shared STATIC ${CALCU_SHARED_SOURCES})
target_include_directories(calcu_shared PUBLIC ${CALCU_ROOT})
target_compile_options(calcu_shared PRIVATE ${CALCU_WARNINGS})
target_link_libraries(calcu_shared PUBLIC calcu_core)
