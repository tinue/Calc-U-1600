cmake_minimum_required(VERSION 3.21)
if(APPLE)
  # MacClipboardImage.mm needs Objective-C++ -- see its own doc comment for
  # why the plotter-paper clipboard export can't use Qt's cross-platform
  # QClipboard here.
  project(CalcU1600Qt CXX OBJCXX)
else()
  project(CalcU1600Qt CXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC OFF)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
qt_standard_project_setup()

set(CORE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../Core)

# ---- App version / build id (About dialog, macOS bundle Info.plist) ----
# Single source of truth for the version number -- both the generated
# Version.h below and the MACOSX_BUNDLE_*VERSION* properties further down
# reference this instead of each hardcoding "0.1.0" separately.
set(APP_VERSION "0.3.0")

# git describe gives a short, human-meaningful build id (tag or short hash,
# +"-dirty" if the tree has local changes) for bug reports. Falls back to
# "unknown" when building from a source tree with no .git (e.g. a tarball
# produced by `git archive`) or when git itself isn't available -- this is
# a graceful degradation, not a build failure. Captured once at CMake
# configure time (not re-derived on every rebuild); re-run cmake to refresh
# it after checking out a different commit.
set(APP_BUILD_ID "unknown")
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../.git)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} describe --always --dirty
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE APP_BUILD_ID_RAW
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE GIT_DESCRIBE_RESULT
    ERROR_QUIET
  )
  if(GIT_DESCRIBE_RESULT EQUAL 0 AND NOT APP_BUILD_ID_RAW STREQUAL "")
    set(APP_BUILD_ID ${APP_BUILD_ID_RAW})
  endif()
endif()
message(STATUS "Calc-U-1600 version ${APP_VERSION} build ${APP_BUILD_ID}")

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/app/Version.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/generated/Version.h @ONLY)

# Trimmed from tools/build_cli.sh / tools/build_pc1600_cli.sh's own source
# lists. Core/Connector/* is header-only (no .cpp files exist there at
# all), so nothing from it needs to be added to this list even though
# PC1500Machine.hpp/PC1600Machine.hpp #include several of its headers.
set(CORE_SOURCES
  ${CORE_ROOT}/Debug/BasicPointerTable.cpp
  ${CORE_ROOT}/CPU/LH5801/LH5801.cpp
  ${CORE_ROOT}/CPU/SC7852/SC7852.cpp
  ${CORE_ROOT}/CPU/LH5803/LH5803Memory.cpp
  ${CORE_ROOT}/CPU/LH5803/LH5803SharedMemory.cpp
  ${CORE_ROOT}/PC1500/PC1500Memory.cpp
  ${CORE_ROOT}/PC1500/PC1500Machine.cpp
  ${CORE_ROOT}/PC1500/PC1500Keyboard.cpp
  ${CORE_ROOT}/PC1500/PC1500Display.cpp
  ${CORE_ROOT}/PC1500/Upd1990ac.cpp
  ${CORE_ROOT}/PC1500/PC1500TraceFile.cpp
  ${CORE_ROOT}/PC1600/PC1600Memory.cpp
  ${CORE_ROOT}/PC1600/PC1600SubCpu.cpp
  ${CORE_ROOT}/PC1600/TC8576F.cpp
  ${CORE_ROOT}/PC1600/PtySerialLink.cpp
  ${CORE_ROOT}/PC1600/PC1600Display.cpp
  ${CORE_ROOT}/PC1600/PC1600Keyboard.cpp
  ${CORE_ROOT}/PC1600/PC1600Machine.cpp
)

# Preset loading, entire (PresetFile.cpp, *PresetLoader.cpp,
# *BasicTyper.cpp, *BasicLoader.cpp, PC1600ProgramPlacement.cpp,
# PC1600MachineImage.cpp, Core/Basic/*), needs the vendored Rust
# `libsharpdx` static library (Core/Basic/vendor/sharpdx/) that
# Core/Basic/BasicProgramSource.cpp tokenizes BASIC programs through --
# rather than split the loader files themselves at a finer grain
# (PC1500PresetLoader.cpp's basic-text/basic-binary program branches call
# straight into PC1500BasicLoader.cpp, which calls
# BasicProgramSource.cpp -- pulling that thread apart isn't worth it), the
# whole feature is gated on that library being present:
#  - macOS: Core/Basic/vendor/sharpdx/libsharpdx.a is committed directly
#    (refreshed via tools/refresh_sharpdx.sh from a local
#    SharpDataExchangeRust checkout).
#  - Linux/Windows: fetched at build time from a SharpDataExchangeRust
#    GitHub release via tools/fetch_sharpdx.sh (see
#    .github/workflows/qt6-prototype.yml's "Fetch sharpdx" step) into
#    libsharpdx-linux.a / sharpdx.lib respectively -- deliberately
#    different filenames from the macOS binary so a fetch here can never
#    collide with, or dirty the git status of, the committed one.
# SHARPDX_LIB below just resolves to whichever of those a given build is
# expected to have; if it isn't there (fetch step skipped, release not
# reachable, etc.) the whole block is skipped and CalcU1600Qt still builds
# and runs -- the "Open Preset…" action just reports it's unavailable
# (see CALCU1600_PRESET_LOADER_AVAILABLE, PresetController.cpp).
if(WIN32)
  set(SHARPDX_LIB ${CORE_ROOT}/Basic/vendor/sharpdx/sharpdx.lib)
elseif(APPLE)
  set(SHARPDX_LIB ${CORE_ROOT}/Basic/vendor/sharpdx/libsharpdx.a)
else()
  set(SHARPDX_LIB ${CORE_ROOT}/Basic/vendor/sharpdx/libsharpdx-linux.a)
endif()

if(EXISTS ${SHARPDX_LIB})
  list(APPEND CORE_SOURCES
    ${CORE_ROOT}/PC1500/PresetFile.cpp
    ${CORE_ROOT}/PC1500/PC1500BasicTyper.cpp
    ${CORE_ROOT}/PC1500/PC1500BasicLoader.cpp
    ${CORE_ROOT}/PC1500/PC1500PresetLoader.cpp
    ${CORE_ROOT}/PC1600/PC1600BasicTyper.cpp
    ${CORE_ROOT}/PC1600/PC1600BasicLoader.cpp
    ${CORE_ROOT}/PC1600/PC1600ProgramPlacement.cpp
    ${CORE_ROOT}/PC1600/PC1600MachineImage.cpp
    ${CORE_ROOT}/PC1600/PC1600PresetLoader.cpp
    ${CORE_ROOT}/Basic/BasicBinaryImage.cpp
    ${CORE_ROOT}/Basic/BasicProgramSource.cpp
  )
endif()

# MACOSX_BUNDLE: build a real .app on macOS (Contents/MacOS + Contents/
# Resources), not a bare executable -- needed both for normal macOS app
# behavior (Dock icon, Info.plist) and because the bundled .card.yaml
# resources below are placed under Contents/Resources, which only exists
# for a bundled target. WIN32 (plain CMake add_executable() keyword,
# passed through by qt_add_executable): link with the Windows GUI
# subsystem instead of the console one, so no terminal window opens
# alongside the app -- see app/AppLogging.cpp for where qDebug/qWarning/
# etc. go instead now that there's no console to print them to.
qt_add_executable(CalcU1600Qt
  MACOSX_BUNDLE
  WIN32
  app/main.cpp
  app/MainWindow.cpp
  app/MachineController.cpp
  app/FaceplateWidget.cpp
  app/LcdWidget.cpp
  app/ControlBar.cpp
  app/DebugPanel.cpp
  app/PlotterController.cpp
  app/PlotterPaperWidget.cpp
  app/PC1500KeyboardMap.cpp
  app/keylayout/PC1500KeyLayout.cpp
  app/keylayout/PC1600KeyLayout.cpp
  app/AppPaths.cpp
  app/AppLogging.cpp
  app/MemoryModuleManager.cpp
  app/SettingsDialog.cpp
  app/AboutDialog.cpp
  app/PresetController.cpp
  ${CORE_SOURCES}
  resources/calc-u-1600.qrc
)

if(APPLE)
  target_sources(CalcU1600Qt PRIVATE app/MacClipboardImage.mm)
  target_link_libraries(CalcU1600Qt PRIVATE "-framework Cocoa")
endif()

if(WIN32)
  # Embeds the app icon into the .exe itself (Explorer/Start Menu/taskbar
  # pinned icon before launch) -- see resources/windows/calc-u-1600.rc.
  # Separate from QApplication::setWindowIcon() in app/main.cpp, which
  # only covers the running window.
  target_sources(CalcU1600Qt PRIVATE resources/windows/calc-u-1600.rc)
endif()

target_include_directories(CalcU1600Qt PRIVATE ${CORE_ROOT} app ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(CalcU1600Qt PRIVATE Qt6::Widgets)

if(EXISTS ${SHARPDX_LIB})
  # See the CORE_SOURCES SHARPDX_LIB block above.
  target_link_libraries(CalcU1600Qt PRIVATE ${SHARPDX_LIB})
  if(UNIX AND NOT APPLE)
    # sharpdx's Linux serial-port backend needs libudev; unlike Windows'
    # system import libs (below), this one really is always on the
    # system's own default linker search path once libudev-dev is
    # installed (see build.yml), so no vendoring is needed here -- just
    # the link.
    target_link_libraries(CalcU1600Qt PRIVATE udev)
  endif()
  if(WIN32)
    # Some of sharpdx's Windows dependencies (a transitive older
    # windows-sys pulled in by its serial-port backend) link against a
    # crate-vendored import lib like windows.0.52.0.lib that isn't part
    # of the Windows SDK/MSVC-CRT default search path -- SharpDataExchangeRust's
    # package.sh vendors the actual file alongside sharpdx.lib for
    # exactly this reason (see its own comment), so just add that
    # directory to the link path; harmless if no such file was shipped.
    target_link_directories(CalcU1600Qt PRIVATE ${CORE_ROOT}/Basic/vendor/sharpdx)
    # `cargo build` links the Windows system import libs Rust's std needs
    # (sockets, CSPRNG seeding, futex-based sync, ...) into sde.exe
    # automatically -- consuming the raw sharpdx.lib straight from CMake
    # doesn't get that for free, so without this the link fails with
    # unresolved externals like __imp_freeaddrinfo (ws2_32). Rather than
    # hand-maintain a guessed list here (which silently drifts as
    # sharpdx's own dependencies change), prefer the authoritative list
    # rustc itself reported for that exact build -- SharpDataExchangeRust's
    # release packaging captures it via `rustc --print=native-static-libs`
    # into native-libs-windows.txt, and tools/fetch_sharpdx.sh vendors it
    # alongside sharpdx.lib when the release has one (see its own doc
    # comment). Only fall back to a hardcoded guess for an older release
    # (or a hand-placed sharpdx.lib) that predates this file existing.
    #
    # On x86_64-pc-windows-msvc rustc's note is MSVC linker syntax --
    # space-separated bare `name.lib` filenames and the occasional
    # `/defaultlib:x` flag -- NOT the `-lname` GCC form, so this is passed
    # to the linker as raw link options (target_link_options), not treated
    # as a list of plain library names (target_link_libraries would try to
    # look up a library literally named "/defaultlib:msvcrt" and fail).
    set(SHARPDX_NATIVE_LIBS_FILE ${CORE_ROOT}/Basic/vendor/sharpdx/native-libs-windows.txt)
    if(EXISTS ${SHARPDX_NATIVE_LIBS_FILE})
      file(READ ${SHARPDX_NATIVE_LIBS_FILE} SHARPDX_NATIVE_LIBS_RAW)
      string(STRIP "${SHARPDX_NATIVE_LIBS_RAW}" SHARPDX_NATIVE_LIBS_RAW)
      separate_arguments(SHARPDX_NATIVE_LINK_OPTS UNIX_COMMAND "${SHARPDX_NATIVE_LIBS_RAW}")
      message(STATUS "sharpdx: linking native libs from native-libs-windows.txt: ${SHARPDX_NATIVE_LINK_OPTS}")
      target_link_options(CalcU1600Qt PRIVATE ${SHARPDX_NATIVE_LINK_OPTS})
    else()
      set(SHARPDX_NATIVE_LIBS_FALLBACK ws2_32 userenv advapi32 bcrypt ntdll synchronization)
      message(STATUS "sharpdx: native-libs-windows.txt not found, using hardcoded fallback: ${SHARPDX_NATIVE_LIBS_FALLBACK}")
      target_link_libraries(CalcU1600Qt PRIVATE ${SHARPDX_NATIVE_LIBS_FALLBACK})
    endif()
  endif()
  target_compile_definitions(CalcU1600Qt PRIVATE CALCU1600_PRESET_LOADER_AVAILABLE)
endif()

# ---- App icon (macOS bundle icon; Windows' equivalent is the WIN32
# block's calc-u-1600.rc above, Linux's is the AppImage's --icon-file in
# build.yml). Generated once from the Xcode project's own
# Assets.xcassets/AppIcon.appiconset via `iconutil -c icns` and committed
# here, same as the vendored libsharpdx.a above -- regenerate with
# iconutil if the source PNGs ever change.
if(APPLE)
  set(APP_ICON_FILE ${CMAKE_CURRENT_SOURCE_DIR}/resources/CalcU1600.icns)
  set_source_files_properties(${APP_ICON_FILE} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
  target_sources(CalcU1600Qt PRIVATE ${APP_ICON_FILE})
  # Explicit bundle identifier (same ch.erzberger.* namespace as
  # SharpDataExchangeRust's ch.erzberger.SharpDataExchange) -- needed for
  # stable codesigning/notarization identity across CI runs, rather than
  # whatever default CMake would otherwise synthesize.
  set_target_properties(CalcU1600Qt PROPERTIES
    MACOSX_BUNDLE_ICON_FILE CalcU1600.icns
    MACOSX_BUNDLE_GUI_IDENTIFIER ch.erzberger.CalcU1600Qt
    MACOSX_BUNDLE_BUNDLE_VERSION ${APP_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${APP_VERSION}
    MACOSX_BUNDLE_BUNDLE_NAME "Calc-U-1600"
  )
endif()

# User-facing output name: "Calc-U-1600" everywhere (the .app bundle on
# macOS, the plain executable on Linux/Windows) -- the CMake target
# identifier above stays CalcU1600Qt (internal only, not user-visible;
# renaming it would mean touching every target_* call in this file for
# no user-facing benefit).
set_target_properties(CalcU1600Qt PROPERTIES OUTPUT_NAME "Calc-U-1600")

# ---- Memory-card resource files, and bundled ROMs (loose, NOT .qrc --
# Core's scanMemoryCardDirectory()/BundledRomCatalog read a real filesystem
# directory via std::ifstream/std::filesystem, so these must be real files
# on disk) ----
set(CARD_RESOURCES_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/resources/cards)
file(GLOB CARD_YAML_FILES ${CARD_RESOURCES_SRC_DIR}/*.card.yaml)
set(ROM_RESOURCES_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../roms)
file(GLOB BUNDLED_ROM_FILES ${ROM_RESOURCES_SRC_DIR}/*.ROM ${ROM_RESOURCES_SRC_DIR}/*.bin)
set(BUNDLED_RESOURCE_FILES ${CARD_YAML_FILES} ${BUNDLED_ROM_FILES})

if(APPLE)
  # Inside the .app bundle: Contents/Resources -- AppPaths::
  # bundledResourcesDir() resolves applicationDirPath()/../Resources
  # (Contents/MacOS/../Resources) to land here.
  set_source_files_properties(${BUNDLED_RESOURCE_FILES} PROPERTIES
    MACOSX_PACKAGE_LOCATION "Resources")
  target_sources(CalcU1600Qt PRIVATE ${BUNDLED_RESOURCE_FILES})
else()
  # Windows/Linux: a "resources" directory copied next to the built
  # executable at build time, so a plain `cmake --build` produces a
  # runnable, self-contained build dir.
  add_custom_command(TARGET CalcU1600Qt POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:CalcU1600Qt>/resources"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${BUNDLED_RESOURCE_FILES}
            "$<TARGET_FILE_DIR:CalcU1600Qt>/resources"
    COMMENT "Copying bundled .card.yaml/ROM resources next to the executable")
  install(FILES ${BUNDLED_RESOURCE_FILES} DESTINATION resources)
endif()

# ---- License / third-party-notice files -- ships GPLv3 (this project)
# and LGPLv3 (bundled Qt6) license text alongside the binary, so LGPLv3's
# obligation to provide the license text travels with every distributed
# build, not just the git repo. See ../THIRD-PARTY-NOTICES.md.
set(LICENSE_FILES
  ${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE
  ${CMAKE_CURRENT_SOURCE_DIR}/../THIRD-PARTY-NOTICES.md
  ${CMAKE_CURRENT_SOURCE_DIR}/../licenses/LGPL-3.0.txt
  ${CMAKE_CURRENT_SOURCE_DIR}/../licenses/GPL-3.0.txt
)

if(APPLE)
  set_source_files_properties(${LICENSE_FILES} PROPERTIES
    MACOSX_PACKAGE_LOCATION "Resources/licenses")
  target_sources(CalcU1600Qt PRIVATE ${LICENSE_FILES})
else()
  add_custom_command(TARGET CalcU1600Qt POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:CalcU1600Qt>/licenses"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${LICENSE_FILES}
            "$<TARGET_FILE_DIR:CalcU1600Qt>/licenses"
    COMMENT "Copying license/third-party-notice files next to the executable")
  install(FILES ${LICENSE_FILES} DESTINATION licenses)
endif()
