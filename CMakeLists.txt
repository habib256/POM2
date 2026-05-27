cmake_minimum_required(VERSION 3.16)
project(pom2_imgui VERSION 0.6 LANGUAGES CXX)

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
    EchoPlusCard.cpp
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
    MouseCardAppleWin.cpp
    PhasorCard.cpp
    PrinterCard.cpp
    Ssi263.cpp
    Ssi263PhonemeData.cpp
    Block512Backing.cpp
    ProDOSHardDiskCard.cpp
    ProDOSVolume.cpp
    ResourcePaths.cpp
    RomLoader.cpp
    FloppyEmuDevice.cpp
    FloppyEmu_ImGui.cpp
    Settings.cpp
    SlotBus.cpp
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
    EchoPlusCard.h
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
    MouseCardAppleWin.h
    MountableMediaCard.h
    Ay3_8910.h
    PhasorCard.h
    PrinterCard.h
    Ssi263.h
    Ssi263PhonemeData.h
    Via6522.h
    Block512Backing.h
    ProDOSHardDiskCard.h
    ProDOSVolume.h
    Logger.h
    Settings.h
    CpuClock.h
    ResourcePaths.h
    RomLoader.h
    FloppyEmuDevice.h
    FloppyEmu_ImGui.h
    SlotBus.h
    SlotCardCatalog.h
    SlotPeripheral.h
    SmartPort35Unit.h
    SmartPortCard.h
    SmartPortHdvUnit.h
    SmartPortUnit.h
    SnapshotIO.h
    SpeakerDevice.h
    StatusLed.h
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

# Convenience alias: the CMake target is ${PROJECT_NAME} (pom2_imgui) but the
# output binary is POM2, so `cmake --build . --target POM2` is otherwise a
# silent no-op (make: "Nothing to be done for 'POM2'"). This phony target maps
# the intuitive name back to the real one. Use either; `cmake --build .` (no
# --target) still builds everything.
add_custom_target(POM2 DEPENDS ${PROJECT_NAME})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# ─── Platform deps ───────────────────────────────────────────────────────
if(EMSCRIPTEN)
    # Browser build: Emscripten ships GLFW3 + WebGL2 ports built-in. The
    # POSIX socket bridges (telnet, AI control HTTP) are compiled out
    # via #ifdef __EMSCRIPTEN__ guards in SuperSerialCard.cpp /
    # AiControlServer.cpp — symbols stay declared so callers still link.
    #
    # Single-threaded build by design — runs on any static host
    # (GitHub Pages, Cloudflare Pages, plain S3, etc.) without the
    # COOP/COEP headers that pthreads / SharedArrayBuffer would
    # require. The CPU worker thread is replaced by
    # `EmulationController::tickFrame()` called from the main render
    # loop (see main.cpp). Trade-off: no parallel audio thread, but
    # miniaudio's Web Audio backend runs in a worklet outside our
    # control, so this is invisible in practice.
    #
    # FORCE_FILESYSTEM + preload-file: ROMs are baked into POM2.data
    # at build time. Settings persistence routes through IDBFS
    # (mounted at /persistent in the shell preRun hook).
    set(POM2_WASM_LINK
        "-sUSE_GLFW=3"
        "-sUSE_WEBGL2=1"
        "-sFULL_ES3=1"
        "-sMIN_WEBGL_VERSION=2"
        "-sMAX_WEBGL_VERSION=2"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sINITIAL_MEMORY=134217728"      # 128 MiB
        "-sSTACK_SIZE=1048576"            # 1 MiB stack
        "-sEXIT_RUNTIME=0"
        "-sFORCE_FILESYSTEM=1"
        "-lidbfs.js"
        "-sEXPORTED_RUNTIME_METHODS=['FS','IDBFS','ccall','cwrap']"
        "--shell-file" "${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html"
    )
    # Preload host folders into POM2.data. Each clause maps an
    # on-disk source folder to a virtual path the binary's std::ifstream
    # / ResourcePaths probes will resolve against (`./roms/...`,
    # `./disks/...`, etc — exactly what the CWD-based search finds on
    # native).
    # Default WASM bundle: ROMs (~600 KB) + fonts (~1 MB). The user-disk
    # folders (`disks/`, `disks35/`, `hdv/`, `floppyemu/`) are
    # opt-in because they can be hundreds of MB and there's no point
    # shipping someone else's `.dsk` collection. Build with
    # `-DPOM2_WASM_BUNDLE_DISKS=ON` to fold them in.
    #
    # --preload-file: pass src+dst as ONE token via SHELL: so CMake
    # doesn't try to interpret the bare `@` as a response-file marker.
    set(POM2_WASM_BUNDLE "roms;fonts" CACHE STRING
        "Asset folders to preload into POM2.data (semicolon-separated)")
    option(POM2_WASM_BUNDLE_DISKS
        "Include disks/disks35/hdv/floppyemu in the WASM bundle" OFF)
    if(POM2_WASM_BUNDLE_DISKS)
        list(APPEND POM2_WASM_BUNDLE disks disks35 hdv floppyemu)
    endif()
    foreach(_pom2_asset ${POM2_WASM_BUNDLE})
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_pom2_asset}")
            list(APPEND POM2_WASM_LINK
                "SHELL:--preload-file ${CMAKE_CURRENT_SOURCE_DIR}/${_pom2_asset}@/${_pom2_asset}")
        endif()
    endforeach()
    target_link_options(${PROJECT_NAME} PRIVATE ${POM2_WASM_LINK})
    # Match shell.html's <canvas id="canvas"> ID convention.
    set_target_properties(${PROJECT_NAME} PROPERTIES
        OUTPUT_NAME "POM2"
        SUFFIX      ".html"
    )
else()
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
endif()

# ─── pom2_headless ────────────────────────────────────────────────────────
# Console-only binary: same emulator core (Memory + M6502 + SlotBus + the
# Disk II / Super Serial cards) wired to a TCP listener via the SSC, so
# you can `telnet 127.0.0.1 6502` and drive the Apple II from anywhere
# without an X server / Cocoa window. Used for self-tests and CI.
# Skipped under Emscripten — no TCP listener, no terminal anyway.
if(NOT EMSCRIPTEN)
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
    ResourcePaths.cpp
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
endif() # NOT EMSCRIPTEN (pom2_headless)

if(MSVC)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN)
    target_compile_options(${PROJECT_NAME} PRIVATE /utf-8)
endif()

# ─── Tests ───────────────────────────────────────────────────────────────
option(POM2_ENABLE_TESTS "Build POM2 unit tests" ON)
if(POM2_ENABLE_TESTS AND NOT EMSCRIPTEN)
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

# ─── Install + packaging ──────────────────────────────────────────────────
# Layout: the binary in <prefix>/bin, redistributable assets in
# <prefix>/share/POM2. ResourcePaths resolves assets via <exeDir>/../share/
# POM2 (and the per-user ~/.local/share/POM2), so this same tree works for
# a relocatable tarball (CPack TGZ), an FHS /usr install (CPack DEB), and an
# AppImage. The Apple ROMs are deliberately NOT installed — they are user-
# supplied (copyright); only a drop-here README ships in roms/.
include(GNUInstallDirs)
set(POM2_DATADIR "${CMAKE_INSTALL_DATADIR}/POM2")
# GNUInstallDirs derives DOCDIR from the (internal) project name pom2_imgui;
# force the user-facing "POM2" so docs land in share/doc/POM2.
set(CMAKE_INSTALL_DOCDIR "${CMAKE_INSTALL_DATADIR}/doc/POM2")

install(TARGETS ${PROJECT_NAME}
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

# Bundled UI fonts (DejaVu Sans + Font Awesome Free — both redistributable).
install(FILES fonts/DejaVuSans.ttf fonts/fa-solid-900.ttf
        DESTINATION "${POM2_DATADIR}/fonts")

# MAME floppy mechanical samples (BSD-3-Clause — roms/floppy_samples/
# README.txt). Optional: only when present in the source tree.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/roms/floppy_samples")
    install(DIRECTORY roms/floppy_samples
            DESTINATION "${POM2_DATADIR}/roms"
            FILES_MATCHING PATTERN "*.wav" PATTERN "*.txt")
endif()

# Drop-here note in place of the (non-redistributable) Apple ROMs.
install(FILES packaging/roms_README.txt
        DESTINATION "${POM2_DATADIR}/roms" RENAME README.txt)

# Docs + desktop integration.
install(FILES README.md CHANGELOG.md DEV.md TODO.md LICENSE
        DESTINATION "${CMAKE_INSTALL_DOCDIR}")
install(FILES packaging/POM2.desktop
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")

# ─── CPack ─────────────────────────────────────────────────────────────────
# `cpack -G TGZ` → relocatable POM2-<ver>-Linux.tar.gz (bin/ + share/POM2).
# `cpack -G DEB` → .deb depending on the system GLFW + OpenGL runtimes.
# build_dist.sh drives this and collects the artifacts into DIST/.
set(CPACK_PACKAGE_NAME            "POM2")
set(CPACK_PACKAGE_VENDOR          "POM2")
set(CPACK_PACKAGE_VERSION         "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Apple II / II+ / //e / //c / //c+ emulator")
set(CPACK_PACKAGE_HOMEPAGE_URL    "https://github.com/")
set(CPACK_RESOURCE_FILE_LICENSE   "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_CONTACT         "gist974@gmail.com")
set(CPACK_PACKAGE_FILE_NAME
    "POM2-v${PROJECT_VERSION}-linux-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_STRIP_FILES             ON)

# Debian package: link against the distro's shared GLFW + OpenGL.
set(CPACK_DEBIAN_PACKAGE_DEPENDS  "libglfw3 (>= 3.3), libgl1")
set(CPACK_DEBIAN_PACKAGE_SECTION  "otherosfs")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)

include(CPack)
