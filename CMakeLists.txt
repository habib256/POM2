cmake_minimum_required(VERSION 3.16)
project(pom2_imgui VERSION 0.5 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
                 Debug Release RelWithDebInfo MinSizeRel)
endif()

# ─── Optional sanitizer build ─────────────────────────────────────────────
# Use with: cmake -DPOM2_SANITIZE=address|thread|undefined ..
# Applied globally (compile + link) so the GLFW / OpenGL / miniaudio C ABI
# call-throughs are instrumented too — `thread` in particular needs every
# TU built with the flag or it deadlocks/false-positives. Empty string =
# disabled. Sanitizers conflict pairwise (asan ≠ tsan), so only one at a
# time; the option's STRINGS hint surfaces the typical choices in
# cmake-gui / ccmake.
set(POM2_SANITIZE "" CACHE STRING
    "Build with -fsanitize=<value>. Empty = off. address/thread/undefined.")
set_property(CACHE POM2_SANITIZE PROPERTY STRINGS
             "" "address" "thread" "undefined" "address,undefined")
if(POM2_SANITIZE)
    if(MSVC)
        message(WARNING "POM2_SANITIZE is gcc/clang-only; ignoring under MSVC.")
    else()
        message(STATUS "POM2 sanitizer build: -fsanitize=${POM2_SANITIZE}")
        add_compile_options(-fsanitize=${POM2_SANITIZE} -fno-omit-frame-pointer -g)
        add_link_options   (-fsanitize=${POM2_SANITIZE})
    endif()
endif()

# ─── Dear ImGui (vendored) ────────────────────────────────────────────────
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/imgui)
if(NOT EXISTS "${IMGUI_DIR}/imgui.cpp")
    message(FATAL_ERROR
        "Dear ImGui not found in ${IMGUI_DIR}. Run ./setup_imgui.sh first.")
endif()

file(GLOB IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

# ─── POM2 sources ────────────────────────────────────────────────────────
set(SOURCES
    M6502.cpp
    Memory.cpp
    MemoryProfile_IIcClass.cpp
    AiControlServer.cpp
    Apple2Display.cpp
    AtaBlockDevice.cpp
    AudioDevice.cpp
    CassetteDevice.cpp
    CffaCard.cpp
    CassetteDeck_ImGui.cpp
    CliDispatcher.cpp
    CliRunner.cpp
    ClockCard.cpp
    Disassembler6502.cpp
    Disk35Image.cpp
    Disk35Controller_ImGui.cpp
    DiskController_ImGui.cpp
    DiskIICard.cpp
    DiskImage.cpp
    DiskLibrary_ImGui.cpp
    EmulationController.cpp
    FloppySoundDevice.cpp
    HdvController_ImGui.cpp
    IWMDevice.cpp
    SmartPortHub.cpp
    Sony35Drive.cpp
    CharRomCatalog.cpp
    JoystickInput.cpp
    JoystickPanel_ImGui.cpp
    LeChatMauveCard.cpp
    LeChatMauve_ImGui.cpp
    M68705P3.cpp
    MainWindow.cpp
    MainWindow_MemoryMaps.cpp
    MainWindow_Slots.cpp
    MC6821.cpp
    MemoryViewer_ImGui.cpp
    Mockingboard.cpp
    MouseCard.cpp
    Block512Backing.cpp
    ProDOSHardDiskCard.cpp
    ProDOSVolume.cpp
    RomLoader.cpp
    FloppyEmuDevice.cpp
    FloppyEmu_ImGui.cpp
    Settings.cpp
    SlotBus.cpp
    SlotManager_ImGui.cpp
    SmartPort35Unit.cpp
    SmartPortCard.cpp
    SmartPortHdvUnit.cpp
    SmartPortUnit.cpp
    SmartPort_ImGui.cpp
    SnapshotIO.cpp
    SpeakerDevice.cpp
    SuperSerialCard.cpp
    SystemProfile.cpp
    Toolbar_ImGui.cpp
    main.cpp
)

set(HEADERS
    M6502.h
    Memory.h
    MemoryProfile.h
    MemoryProfile_IIcClass.h
    AiControlServer.h
    Apple2Display.h
    AtaBlockDevice.h
    AudioDevice.h
    CassetteDevice.h
    CassetteDeck_ImGui.h
    CffaCard.h
    CharRomCatalog.h
    CliDispatcher.h
    ClockCard.h
    Disassembler6502.h
    Disk35Image.h
    Disk35Controller_ImGui.h
    DiskController_ImGui.h
    DiskIICard.h
    DiskImage.h
    SmartPortHub.h
    Sony35Drive.h
    EmulationController.h
    FloppySoundDevice.h
    FloppySoundSink.h
    HdvController_ImGui.h
    JoystickInput.h
    JoystickPanel_ImGui.h
    IconsFontAwesome6.h
    LeChatMauveCard.h
    LeChatMauve_ImGui.h
    M68705P3.h
    MainWindow.h
    MC6821.h
    MemoryViewer_ImGui.h
    Mockingboard.h
    MouseCard.h
    MountableMediaCard.h
    Block512Backing.h
    ProDOSHardDiskCard.h
    ProDOSVolume.h
    Logger.h
    Settings.h
    CpuClock.h
    RomLoader.h
    FloppyEmuDevice.h
    FloppyEmu_ImGui.h
    SlotBus.h
    SlotCardCatalog.h
    SlotManager_ImGui.h
    SlotPeripheral.h
    SmartPort35Unit.h
    SmartPortCard.h
    SmartPortHdvUnit.h
    SmartPortUnit.h
    SnapshotIO.h
    SpeakerDevice.h
    SuperSerialCard.h
)

# Strict warnings on POM2 first-party sources only. Vendored Dear ImGui
# keeps its upstream baseline so this CMakeLists doesn't churn every time
# we resync `imgui/`. Applied per source-file rather than per target so
# the same flags propagate naturally to pom2_headless (which compiles a
# subset of ${SOURCES}) without listing them twice.
if(MSVC)
    set(POM2_WARN_FLAGS "/W4")
else()
    set(POM2_WARN_FLAGS "-Wall -Wextra -Wshadow")
endif()
foreach(_pom2_src ${SOURCES})
    set_source_files_properties(${_pom2_src} PROPERTIES
        COMPILE_FLAGS "${POM2_WARN_FLAGS}")
endforeach()

add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS} ${IMGUI_SOURCES})
set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "POM2")

target_include_directories(${PROJECT_NAME} PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# ─── Platform deps ───────────────────────────────────────────────────────
find_package(OpenGL REQUIRED)

find_package(glfw3 3.3 QUIET CONFIG)
if(glfw3_FOUND)
    set(GLFW_TARGET glfw)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GLFW3 REQUIRED IMPORTED_TARGET glfw3)
    set(GLFW_TARGET PkgConfig::GLFW3)
endif()

target_link_libraries(${PROJECT_NAME}
    ${GLFW_TARGET}
    ${OPENGL_LIBRARIES}
)

# Miniaudio + stb_vorbis (vendored single-header decoders) need libdl /
# libpthread / libm on Linux; CoreAudio + AudioToolbox + AudioUnit on
# macOS; nothing extra on Windows beyond winmm.
if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(${PROJECT_NAME} ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(${PROJECT_NAME}
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework AudioUnit"
    )
endif()
if(WIN32)
    target_link_libraries(${PROJECT_NAME} winmm)
endif()

if(APPLE)
    target_link_libraries(${PROJECT_NAME}
        "-framework Cocoa"
        "-framework IOKit"
        "-framework CoreVideo"
    )
    target_compile_definitions(${PROJECT_NAME} PRIVATE GL_SILENCE_DEPRECATION)
endif()

if(WIN32)
    target_link_libraries(${PROJECT_NAME} opengl32)
endif()

# ─── pom2_headless ────────────────────────────────────────────────────────
# Console-only binary: same emulator core (Memory + M6502 + SlotBus + the
# Disk II / Super Serial cards) wired to a TCP listener via the SSC, so
# you can `telnet 127.0.0.1 6502` and drive the Apple II from anywhere
# without an X server / Cocoa window. Used for self-tests and CI.
add_executable(pom2_headless
    pom2_headless.cpp
    AudioDevice.cpp
    CassetteDevice.cpp
    ClockCard.cpp
    Disk35Image.cpp
    DiskImage.cpp
    DiskIICard.cpp
    EmulationController.cpp
    FloppySoundDevice.cpp
    IWMDevice.cpp
    SmartPortHub.cpp
    Sony35Drive.cpp
    LeChatMauveCard.cpp
    M6502.cpp
    Memory.cpp
    MemoryProfile_IIcClass.cpp
    Mockingboard.cpp
    RomLoader.cpp
    SlotBus.cpp
    SnapshotIO.cpp
    SpeakerDevice.cpp
    SuperSerialCard.cpp
    SystemProfile.cpp
)
target_include_directories(pom2_headless PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${IMGUI_DIR}     # only for miniaudio.h location, no ImGui linkage
)
if(UNIX AND NOT APPLE)
    target_link_libraries(pom2_headless PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(pom2_headless PRIVATE
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework AudioUnit"
    )
endif()
if(WIN32)
    target_link_libraries(pom2_headless PRIVATE winmm)
endif()

if(MSVC)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN)
    target_compile_options(${PROJECT_NAME} PRIVATE /utf-8)
endif()

# ─── Tests ───────────────────────────────────────────────────────────────
option(POM2_ENABLE_TESTS "Build POM2 unit tests" ON)
if(POM2_ENABLE_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Enable LTO on non-Debug builds when the toolchain supports it.
include(CheckIPOSupported)
check_ipo_supported(RESULT POM2_IPO_SUPPORTED OUTPUT POM2_IPO_ERROR)
if(POM2_IPO_SUPPORTED)
    set_property(TARGET ${PROJECT_NAME} PROPERTY
                 INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    set_property(TARGET ${PROJECT_NAME} PROPERTY
                 INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
endif()
