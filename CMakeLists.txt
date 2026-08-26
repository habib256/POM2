cmake_minimum_required(VERSION 3.16)

# CMP0091 NEW: the MSVC runtime library is chosen via the MSVC_RUNTIME_LIBRARY
# target property instead of being baked into CMAKE_CXX_FLAGS_<CONFIG>. Required
# by POM2_WIN_STATIC_RUNTIME (see the Windows section near the bottom) — without
# it the property is silently ignored and the release exe would go out linked
# against the DYNAMIC CRT, reintroducing the app-local-DLL failure the static
# link exists to prevent. Must be set before project().
cmake_policy(SET CMP0091 NEW)

project(pom2_imgui VERSION 0.8.3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build the native regression suite by default.  Declared up front because the
# reusable core has a test twin (same sources, assertions kept enabled) when
# this option is on; the add_subdirectory(tests) call remains near the bottom.
option(POM2_ENABLE_TESTS "Build POM2 unit tests" ON)
option(POM2_BUILD_FRONTEND
       "Build the Dear ImGui/GLFW desktop or WebAssembly frontend" ON)
option(POM2_INSTALL_CORE_SDK
       "Install pom2_core headers and CMake package metadata" OFF)
include(GNUInstallDirs)

# ─── Package payload manifest ──────────────────────────────────────────────
# `packaging/bundle.manifest` is the SINGLE list of what ships inside a POM2
# package, read here once and consumed twice: by the WASM --preload-file block
# (browser) and by the install() rules (every desktop package, since the
# AppImage is staged with `cmake --install`). packaging/stage_data.sh parses the
# same file for the macOS .app and the Windows .zip. Format documented in the
# manifest itself; parse it here rather than in each consumer so a fifth
# consumer cannot start its own drifting copy.
#
# Outputs, all CMake lists:
#   POM2_BUNDLE_DIRS   folders copied whole            ("roms" "fonts")
#   POM2_BUNDLE_FILES  "<src>|<dst>" pairs             ("pic/x.jpg|pic/x.jpg")
#   POM2_BUNDLE_WASM   browser-only folders            ("floppyemu")
set(POM2_BUNDLE_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/packaging/bundle.manifest")
set(POM2_BUNDLE_DIRS  "")
set(POM2_BUNDLE_FILES "")
set(POM2_BUNDLE_WASM  "")
set(POM2_BUNDLE_DENYGLOB "")
if(EXISTS "${POM2_BUNDLE_MANIFEST}")
    # Re-run CMake when the manifest changes: it decides install() rules and
    # link options, and a stale configure would package the previous list.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 "${POM2_BUNDLE_MANIFEST}")
    # ENCODING UTF-8 is REQUIRED, not decoration: plain file(STRINGS) extracts
    # runs of printable ASCII and treats anything else as binary to skip, so it
    # SPLITS every comment line at its em-dash and hands back the tail as if it
    # were another line — which then parses as a bogus entry.
    file(STRINGS "${POM2_BUNDLE_MANIFEST}" _pom2_manifest_lines ENCODING UTF-8)
    foreach(_line IN LISTS _pom2_manifest_lines)
        # Strip the comment with FIND/SUBSTRING, NOT a regex: CMake's `.` does
        # not match the bytes of a multi-byte UTF-8 character, so `#.*$` stops
        # dead at the first em-dash and leaves the tail of a prose comment
        # behind — which then parses as a manifest entry ("unknown entry kind
        # 'the'"). No manifest path may contain a '#'.
        string(FIND "${_line}" "#" _hash)
        if(NOT _hash EQUAL -1)
            string(SUBSTRING "${_line}" 0 ${_hash} _line)
        endif()
        string(STRIP "${_line}" _line)
        if(_line STREQUAL "")
            continue()
        endif()
        separate_arguments(_fields UNIX_COMMAND "${_line}")
        list(GET _fields 0 _kind)
        list(LENGTH _fields _n)
        if(_n LESS 2)
            message(FATAL_ERROR "bundle.manifest: '${_line}' has no path")
        endif()
        list(GET _fields 1 _src)
        if(_kind STREQUAL "dir")
            list(APPEND POM2_BUNDLE_DIRS "${_src}")
        elseif(_kind STREQUAL "file")
            set(_dst "${_src}")
            if(_n GREATER 2)
                list(GET _fields 2 _dst)
            endif()
            list(APPEND POM2_BUNDLE_FILES "${_src}|${_dst}")
        elseif(_kind STREQUAL "wasm")
            list(APPEND POM2_BUNDLE_WASM "${_src}")
        elseif(_kind STREQUAL "denyglob")
            # `dir` entries copy the WORKING TREE, so an archive dropped into
            # roms/ and never committed would ship. Excluded from both the
            # install rules and the WASM preload below.
            list(APPEND POM2_BUNDLE_DENYGLOB "${_src}")
        elseif(_kind STREQUAL "deny")
            # Enforced by stage_data.sh --verify against a STAGED tree; nothing
            # to do at configure time (CMake never copies these folders).
        else()
            message(FATAL_ERROR "bundle.manifest: unknown entry kind '${_kind}'")
        endif()
    endforeach()
else()
    message(WARNING "packaging/bundle.manifest missing — packages will ship no assets")
endif()

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
set(IMGUI_SOURCES "")
if(POM2_BUILD_FRONTEND)
    if(NOT EXISTS "${IMGUI_DIR}/imgui.cpp")
        message(FATAL_ERROR
            "Dear ImGui not found in ${IMGUI_DIR}. Run ./setup_imgui.sh first, "
            "or configure -DPOM2_BUILD_FRONTEND=OFF for core/headless work.")
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
elseif(EMSCRIPTEN)
    message(FATAL_ERROR "POM2_BUILD_FRONTEND=OFF is not a valid WASM product")
endif()

# ─── POM2 sources ────────────────────────────────────────────────────────
# All first-party C++ lives under src/. The prefix is spelled here once per
# file so set_source_files_properties() below still gets a clean key.
set(POM2_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)

# ─── Generated version header ────────────────────────────────────────────
# Single source of truth for the version string = project(... VERSION ...)
# above. configure_file() expands @PROJECT_VERSION@ into the build tree, and
# re-runs whenever CMakeLists.txt changes, so a version bump rebuilds every
# consumer (main.cpp banner/title, MainWindow About + runtime title).
set(POM2_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
configure_file(
    ${POM2_SRC_DIR}/Version.h.in
    ${POM2_GENERATED_DIR}/Version.h
    @ONLY)

# Architectural source manifests. They compile in internal OBJECT targets and
# compose one public static archive; ownership and upward includes are checked
# below before any target is generated.
# Dependency direction is:
#   foundation headers <- media <- machine <- devices <- runtime <- frontend
set(POM2_MEDIA_SOURCES
    src/AtaBlockDevice.cpp
    src/Block512Backing.cpp
    src/Disk35Image.cpp
    src/DiskImage.cpp
    src/ProDOSVolume.cpp
    src/ResourcePaths.cpp
    src/SnapshotIO.cpp
    src/Sony35Gcr.cpp
)

set(POM2_MACHINE_SOURCES
    src/Keyboard.cpp
    src/M6502.cpp
    src/Z80.cpp
    src/Memory.cpp
    src/MemoryProfile_IIcClass.cpp
    src/AppleWinNtsc.cpp
    src/CassetteDevice.cpp
    src/Disassembler6502.cpp
    src/IWMDevice.cpp
    src/SmartPortHub.cpp
    src/Sony35Drive.cpp
    src/NoSlotClock.cpp
    src/PaddleInputs.cpp
    src/SlotBus.cpp
    src/SpeakerDevice.cpp
    src/SystemProfile.cpp
)

set(POM2_DEVICES_SOURCES
    src/CffaCard.cpp
    src/ClockCard.cpp
    src/Cs8900aDevice.cpp
    src/DiskIICard.cpp
    src/EchoPlusCard.cpp
    src/EchoPlusTMS5220Card.cpp
    src/FloppyEmuDevice.cpp
    src/FujiNetCard.cpp
    src/GrapplerCard.cpp
    src/LeChatMauveCard.cpp
    src/M68705P3.cpp
    src/MC6821.cpp
    src/Mockingboard.cpp
    src/MouseCard.cpp
    src/MouseCardAppleWin.cpp
    src/PhasorCard.cpp
    src/PrinterCard.cpp
    src/ProDOSHardDiskCard.cpp
    src/SmartPort35Unit.cpp
    src/SmartPortCard.cpp
    src/SmartPortHdvUnit.cpp
    src/SmartPortUnit.cpp
    src/SoftCardZ80.cpp
    src/Ssi263.cpp
    src/Ssi263PhonemeData.cpp
    src/SuperSerialCard.cpp
    src/UthernetCard.cpp
    src/UthernetIICard.cpp
    src/W5100Device.cpp
)

set(POM2_RUNTIME_SOURCES
    src/Pom2Core.cpp
    src/AiControlServer.cpp
    src/Apple2Display.cpp
    src/AudioDevice.cpp
    src/CharRomCatalog.cpp
    src/ChildProcess.cpp
    src/EmulationController.cpp
    src/FloppySoundDevice.cpp
    src/FujiNetHost.cpp
    src/ImageWriter.cpp
    src/ImageWriterPdf.cpp
    src/MachineSnapshot.cpp
    src/PrinterHistory.cpp
    src/PrinterScreenDump.cpp
    src/PrinterSoundDevice.cpp
    src/RewindBuffer.cpp
    src/RomLoader.cpp
    src/SerialPort.cpp
    src/SlirpNetworkBackend.cpp
    src/SpOverSlipLink.cpp
    src/SpSerialTransport.cpp
    src/SpTcpTransport.cpp
    src/SuperSerialTcpTransport.cpp
    src/W5100HostSockets.cpp
    src/hgrpaint/HgrFont.cpp
)

set(POM2_CORE_SOURCES
    ${POM2_MEDIA_SOURCES}
    ${POM2_MACHINE_SOURCES}
    ${POM2_DEVICES_SOURCES}
    ${POM2_RUNTIME_SOURCES}
)

set(POM2_CORE_PUBLIC_HEADERS
    include/pom2/core.hpp
)

# Host application and presentation layer.  Nothing in pom2_core includes
# ImGui, GLFW or OpenGL; these sources are deliberately kept on the executable
# side of the boundary so headless consumers can link the core alone.
set(POM2_FRONTEND_SOURCES
    src/AudioCoordinator.cpp
    src/DebugCoordinator.cpp
    src/DevicePanelCoordinator.cpp
    src/MouseCoordinator.cpp
    src/NetworkCoordinator.cpp
    src/PrinterCoordinator.cpp
    src/SlotCardFactory.cpp
    src/SlotConfigurationCoordinator.cpp
    src/SlotProvisioningCoordinator.cpp
    src/SlotRebuildCoordinator.cpp
    src/StorageCoordinator.cpp
    src/CassetteDeck_ImGui.cpp
    src/CliDispatcher.cpp
    src/CliRunner.cpp
    src/Disk35Controller_ImGui.cpp
    src/DiskController_ImGui.cpp
    src/DiskLibrary_ImGui.cpp
    src/HdvController_ImGui.cpp
    src/JoystickInput.cpp
    src/JoystickPanel_ImGui.cpp
    src/LeChatMauve_ImGui.cpp
    src/MainWindow.cpp
    src/MainWindow_AuxPanels.cpp
    src/MainWindow_Command.cpp
    src/MainWindow_DevicePanels.cpp
    src/MainWindow_Input.cpp
    src/MainWindow_Media.cpp
    src/MainWindow_MemoryMaps.cpp
    src/MainWindow_Screen.cpp
    src/MainWindow_Slots.cpp
    src/MemoryViewer_ImGui.cpp
    src/NtscPostProcessor.cpp
    src/CrtEffectStack.cpp
    src/Voxel3DRenderer.cpp
    src/OpenGLShader.cpp
    src/ImageWriter_ImGui.cpp
    src/Rewind_ImGui.cpp
    src/FloppyEmu_ImGui.cpp
    src/Settings.cpp
    src/Pom2HgrPaintHost.cpp
    src/hgrpaint/Cam16.cpp
    src/hgrpaint/DhgrNtsc8Palette.cpp
    src/hgrpaint/HgrConvert.cpp
    src/hgrpaint/HgrImageDecode.cpp
    src/hgrpaint/HgrPaintEditor.cpp
    src/hgrpaint/HgrPaintModel.cpp
    src/hgrsprite/HgrSpriteAsmExport.cpp
    src/hgrsprite/HgrSpriteBlit.cpp
    src/hgrsprite/HgrSpriteEditor.cpp
    src/Uthernet_ImGui.cpp
    src/FujiNet_ImGui.cpp
    src/SmartPort_ImGui.cpp
    src/CommandPalette_ImGui.cpp
    src/Pom2Theme.cpp
    src/RomStatus_ImGui.cpp
    src/AbstractionLevels_ImGui.cpp
    src/AppleIIeKeyboardLayout.cpp
    src/Keyboard_ImGui.cpp
    src/Toolbar_ImGui.cpp
    src/main.cpp
)

# Compatibility name used by the existing executable declarations below.
set(POM2_APP_SOURCES ${POM2_FRONTEND_SOURCES})

# Header-only contracts have no source stem from which the checker can infer
# ownership. Keep the cross-layer seams deliberately small and explicit.
set(POM2_FOUNDATION_HEADERS
    src/AudioSource.h
    src/ByteIO.h
    src/FloppySoundSink.h
    src/Logger.h
    src/NetworkBackend.h
    src/Pom2Build.h
    src/PrinterSoundSink.h
)
set(POM2_MEDIA_HEADERS
    src/AtomicFileReplace.h
    src/TwoImg.h
)
set(POM2_MACHINE_HEADERS
    src/Apple2VideoDecode.h
    src/CpuClock.h
    src/MemoryProfile.h
    src/SlotPeripheral.h
)
set(POM2_DEVICES_HEADERS
    src/Ay3_8910.h
    src/AyPsgSynth.h
    src/FujiNetLink.h
    src/MountableMediaCard.h
    src/ProDOSBlockCard.h
    src/SuperSerialTransport.h
    src/Via6522.h
    src/W5100Socket.h
)
set(POM2_RUNTIME_HEADERS
    include/pom2/core.hpp
    src/ImageWriterRom.h
    src/SlipFramer.h
    src/SocketCompat.h
    src/SocketUtil.h
    src/SpTransport.h
    src/SuperSerialTcpTransport.h
)
set(POM2_FRONTEND_HEADERS
    src/AudioCoordinator.h
    src/DebugCoordinator.h
    src/DevicePanelCoordinator.h
    src/MainWindowUiState.h
    src/MouseCoordinator.h
    src/NetworkCoordinator.h
    src/PrinterCoordinator.h
    src/SlotCardFactory.h
    src/SlotConfigurationCoordinator.h
    src/SlotProvisioningCoordinator.h
    src/SlotRebuildCoordinator.h
    src/StorageCoordinator.h
    src/IconsFontAwesome6.h
    src/Mat4.h
    src/MouseGrab.h
    src/PaintCardBatcher.h
    src/Pom2GL.h
    src/PrinterFeedCursor.h
    src/RomCatalog.h
    src/SlotCardCatalog.h
    src/StatusLed.h
    src/hgrpaint/IHgrPaintHost.h
    src/hgrpaint/ImportCommon.h
)

include(cmake/Pom2Architecture.cmake)
pom2_enforce_source_layers(${CMAKE_CURRENT_SOURCE_DIR})
pom2_enforce_mainwindow_line_limit(${CMAKE_CURRENT_SOURCE_DIR} 3000)

# Kept as the complete first-party source inventory for warning properties and
# IDE tooling.  Build targets consume one side of the boundary above.
set(SOURCES ${POM2_CORE_SOURCES} ${POM2_APP_SOURCES})

set(HEADERS
    src/AudioCoordinator.h
    src/DebugCoordinator.h
    src/DevicePanelCoordinator.h
    src/MainWindowUiState.h
    src/MouseCoordinator.h
    src/NetworkCoordinator.h
    src/PrinterCoordinator.h
    src/SlotCardFactory.h
    src/SlotConfigurationCoordinator.h
    src/SlotProvisioningCoordinator.h
    src/SlotRebuildCoordinator.h
    src/StorageCoordinator.h
    src/Keyboard.h
    src/PaddleInputs.h
    src/M6502.h
    src/Z80.h
    src/SoftCardZ80.h
    src/ByteIO.h
    src/Memory.h
    src/MemoryProfile.h
    src/MemoryProfile_IIcClass.h
    src/MachineSnapshot.h
    src/RewindBuffer.h
    src/AiControlServer.h
    src/Apple2Display.h
    src/AppleWinNtsc.h
    src/AtaBlockDevice.h
    src/AudioDevice.h
    src/CassetteDevice.h
    src/CassetteDeck_ImGui.h
    src/CffaCard.h
    src/CharRomCatalog.h
    src/CliDispatcher.h
    src/ClockCard.h
    src/Disassembler6502.h
    src/Disk35Image.h
    src/Disk35Controller_ImGui.h
    src/DiskController_ImGui.h
    src/DiskIICard.h
    src/DiskImage.h
    src/SmartPortHub.h
    src/Sony35Drive.h
    src/EchoPlusCard.h
    src/EchoPlusTMS5220Card.h
    src/EmulationController.h
    src/FloppySoundDevice.h
    src/FloppySoundSink.h
    src/HdvController_ImGui.h
    src/JoystickInput.h
    src/JoystickPanel_ImGui.h
    src/IconsFontAwesome6.h
    src/LeChatMauveCard.h
    src/LeChatMauve_ImGui.h
    src/M68705P3.h
    src/MainWindow.h
    src/MC6821.h
    src/MemoryViewer_ImGui.h
    src/Mockingboard.h
    src/MouseCard.h
    src/MouseCardAppleWin.h
    src/NtscPostProcessor.h
    src/Voxel3DRenderer.h
    src/Mat4.h
    src/CommandPalette_ImGui.h
    src/Pom2Theme.h
    src/OpenGLShader.h
    src/MountableMediaCard.h
    src/Ay3_8910.h
    src/GrapplerCard.h
    src/PhasorCard.h
    src/PrinterCard.h
    src/ImageWriter.h
    src/ImageWriterPdf.h
    src/ImageWriter_ImGui.h
    src/Rewind_ImGui.h
    src/Ssi263.h
    src/Ssi263PhonemeData.h
    src/Via6522.h
    src/Block512Backing.h
    src/ProDOSHardDiskCard.h
    src/ProDOSVolume.h
    src/Logger.h
    src/Settings.h
    src/CpuClock.h
    src/ResourcePaths.h
    src/RomLoader.h
    src/FloppyEmuDevice.h
    src/FloppyEmu_ImGui.h
    src/SlotBus.h
    src/SlotCardCatalog.h
    src/SlotPeripheral.h
    src/NetworkBackend.h
    src/SlirpNetworkBackend.h
    src/Cs8900aDevice.h
    src/UthernetCard.h
    src/W5100Device.h
    src/W5100HostSockets.h
    src/W5100Socket.h
    src/UthernetIICard.h
    src/Uthernet_ImGui.h
    src/SmartPort35Unit.h
    src/SmartPortCard.h
    src/SmartPortHdvUnit.h
    src/SmartPortUnit.h
    src/SnapshotIO.h
    src/SpeakerDevice.h
    src/StatusLed.h
    src/SuperSerialCard.h
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

# ─── Optional: libslirp (Ethernet user-mode NAT) ─────────────────────────
# Powers the Uthernet I (CS8900A), whose guest-side software carries its
# own TCP/IP stack and therefore needs whole Ethernet frames bridged to
# the host. libslirp does that in user space — no root, no TAP device, no
# host configuration — which is why it is preferred over libpcap here.
#
# STRICTLY OPTIONAL. Without it:
#   * Uthernet II still works completely for TCP and UDP (its W5100 is a
#     hardware stack that POM2 maps onto ordinary host sockets), which
#     covers every period IRC / telnet / FTP client. Only its MACRAW and
#     IPRAW modes go dark.
#   * Uthernet I still plugs, resets and probes correctly, but sees no
#     traffic.
# Install with: apt install libslirp-dev / brew install libslirp.
# Force off with -DPOM2_ENABLE_SLIRP=OFF.
# ─── OpenGL ES tier (Raspberry Pi & co) ────────────────────────────────────
# POM2_GLES=ON swaps the desktop GL 3.2 core context for an OpenGL ES 3.0 one
# (GLSL ES 300). That is the tier Raspberry Pi 4/5 actually expose: Mesa's V3D
# caps *desktop* GL at 3.1, so POM2's GL 3.2 core request simply fails there and
# the emulator never opens a window. It also covers most ARM SoCs and older
# iGPUs.
#
# No source fork: the GL subset POM2 uses (VAO/VBO, shaders, FBO, 2D textures)
# is common to both tiers, so the same TUs compile for both. Pom2Build.h's
# POM2_GL_ES macro picks the GLES headers, the `#version 300 es` shader
# prologue, and the direct-link entry points. WASM is GLES already — WebGL 2 IS
# GLES 3.0 — and turns POM2_GL_ES on by itself, without this option.
option(POM2_GLES "Target OpenGL ES 3.0 instead of desktop GL 3.2 (Raspberry Pi & co)" OFF)
if(POM2_GLES)
    if(EMSCRIPTEN)
        message(FATAL_ERROR
            "POM2_GLES is implied by the WASM build (WebGL 2 = GLES 3.0) — drop the flag.")
    endif()
    message(STATUS "POM2: GL tier = OpenGL ES 3.0 / GLSL ES 300")
endif()

option(POM2_ENABLE_SLIRP "Enable libslirp Ethernet backend when available" ON)
set(POM2_SLIRP_TARGET "")
# WIN32 is excluded deliberately, and it is NOT the same gap the host sockets
# were. vcpkg does carry a libslirp port, so the library itself is obtainable;
# what is missing is the POM2 side — SlirpNetworkBackend's poll loop is written
# against POSIX poll() over the fds libslirp hands back, and porting that to
# Winsock cannot be verified without a Windows libslirp build to test against.
# Enabling the detection here would only trade a documented absence for a wall
# of missing-header errors. Uthernet II is unaffected: its W5100 is a hardware
# TCP/IP stack running on ordinary host sockets, which Windows now has.
if(POM2_ENABLE_SLIRP AND NOT EMSCRIPTEN AND NOT WIN32)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SLIRP QUIET IMPORTED_TARGET slirp)
        if(SLIRP_FOUND)
            set(POM2_SLIRP_TARGET PkgConfig::SLIRP)
            message(STATUS "POM2: libslirp ${SLIRP_VERSION} found — "
                           "Uthernet I Ethernet enabled")
        endif()
    endif()
endif()
if(NOT POM2_SLIRP_TARGET)
    message(STATUS "POM2: libslirp not found — Uthernet I will have no host "
                   "transport (Uthernet II TCP/UDP is unaffected)")
endif()

# The Windows resource script embeds the application icon into POM2.exe. It is
# listed as a target source so CMake drives rc.exe (MSVC) or windres (MinGW)
# automatically; on every other platform the list is empty.
set(POM2_PLATFORM_SOURCES "")
if(WIN32)
    list(APPEND POM2_PLATFORM_SOURCES packaging/windows/POM2.rc)
endif()

# ─── Reusable emulator core ───────────────────────────────────────────────
# The logical manifests now have real build identities. OBJECT libraries make
# accidental source ownership visible in IDEs and build diagnostics while the
# installed ABI remains one archive: pom2_core. They are deliberately internal
# implementation targets and never appear in the SDK export set.
add_library(pom2_foundation INTERFACE)
target_compile_features(pom2_foundation INTERFACE cxx_std_17)
target_include_directories(pom2_foundation INTERFACE
    $<BUILD_INTERFACE:${POM2_SRC_DIR}>
    $<BUILD_INTERFACE:${POM2_GENERATED_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

function(pom2_add_core_object_layer target_name)
    add_library(${target_name} OBJECT ${ARGN})
    target_link_libraries(${target_name} PRIVATE pom2_foundation)
endfunction()

pom2_add_core_object_layer(pom2_media
    ${POM2_MEDIA_SOURCES} ${POM2_MEDIA_HEADERS})
pom2_add_core_object_layer(pom2_machine
    ${POM2_MACHINE_SOURCES} ${POM2_MACHINE_HEADERS})
pom2_add_core_object_layer(pom2_devices
    ${POM2_DEVICES_SOURCES} ${POM2_DEVICES_HEADERS})
pom2_add_core_object_layer(pom2_runtime
    ${POM2_RUNTIME_SOURCES} ${POM2_RUNTIME_HEADERS})

set(POM2_CORE_OBJECT_TARGETS
    pom2_media
    pom2_machine
    pom2_devices
    pom2_runtime
)
set(POM2_CORE_OBJECTS "")
foreach(_layer_target IN LISTS POM2_CORE_OBJECT_TARGETS)
    list(APPEND POM2_CORE_OBJECTS $<TARGET_OBJECTS:${_layer_target}>)
endforeach()

# The core owns the machine model and non-graphical host services. Its public
# include path is the existing src/ tree so current consumers keep including
# headers by their stable names ("Memory.h", "SlotBus.h", ...). The alias is
# the namespaced spelling intended for new in-tree and external consumers.
add_library(pom2_core STATIC
    ${POM2_CORE_OBJECTS}
    ${POM2_CORE_PUBLIC_HEADERS})
add_library(POM2::core ALIAS pom2_core)
set_target_properties(pom2_core PROPERTIES EXPORT_NAME core)
target_compile_features(pom2_core PUBLIC cxx_std_17)
target_include_directories(pom2_core PUBLIC
    $<BUILD_INTERFACE:${POM2_SRC_DIR}>
    $<BUILD_INTERFACE:${POM2_GENERATED_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# Release builds define NDEBUG globally, while the existing tests deliberately
# compile their embedded core sources with assertions enabled.  Preserve that
# contract without compiling the core once per test executable: assertion-on
# twins of the four object layers feed one native-only test archive. None of
# these targets are installed or linked by products.
set(POM2_CORE_TARGETS pom2_core)
set(POM2_CORE_COMPILE_TARGETS ${POM2_CORE_OBJECT_TARGETS})
set(POM2_RUNTIME_OBJECT_TARGETS pom2_runtime)
if(POM2_ENABLE_TESTS AND NOT EMSCRIPTEN)
    set(POM2_CORE_TEST_OBJECT_TARGETS "")
    set(POM2_CORE_TEST_OBJECTS "")
    foreach(_layer_target IN LISTS POM2_CORE_OBJECT_TARGETS)
        get_target_property(_layer_sources ${_layer_target} SOURCES)
        set(_test_layer_target ${_layer_target}_test_objects)
        add_library(${_test_layer_target} OBJECT EXCLUDE_FROM_ALL
            ${_layer_sources})
        target_link_libraries(${_test_layer_target} PRIVATE pom2_foundation)
        if(MSVC)
            target_compile_options(${_test_layer_target} PRIVATE /UNDEBUG)
        else()
            target_compile_options(${_test_layer_target} PRIVATE -UNDEBUG)
        endif()
        list(APPEND POM2_CORE_TEST_OBJECT_TARGETS ${_test_layer_target})
        list(APPEND POM2_CORE_TEST_OBJECTS
            $<TARGET_OBJECTS:${_test_layer_target}>)
    endforeach()

    add_library(pom2_core_test STATIC EXCLUDE_FROM_ALL
        ${POM2_CORE_TEST_OBJECTS} ${POM2_CORE_PUBLIC_HEADERS})
    target_include_directories(pom2_core_test PUBLIC
        $<BUILD_INTERFACE:${POM2_SRC_DIR}>
        $<BUILD_INTERFACE:${POM2_GENERATED_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    )
    list(APPEND POM2_CORE_TARGETS pom2_core_test)
    list(APPEND POM2_CORE_COMPILE_TARGETS ${POM2_CORE_TEST_OBJECT_TARGETS})
    list(APPEND POM2_RUNTIME_OBJECT_TARGETS pom2_runtime_test_objects)
endif()

if(POM2_SLIRP_TARGET)
    foreach(_core_target IN LISTS POM2_CORE_TARGETS)
        target_link_libraries(${_core_target} PUBLIC ${POM2_SLIRP_TARGET})
    endforeach()
    # Only the runtime layer compiles the libslirp adapter. A definition on
    # the aggregate static archive would be too late: its objects already
    # exist by then.
    foreach(_runtime_target IN LISTS POM2_RUNTIME_OBJECT_TARGETS)
        target_link_libraries(${_runtime_target} PRIVATE ${POM2_SLIRP_TARGET})
        target_compile_definitions(${_runtime_target} PRIVATE POM2_HAVE_SLIRP)
    endforeach()
endif()

if(POM2_BUILD_FRONTEND)
    add_executable(${PROJECT_NAME}
        ${POM2_APP_SOURCES} ${HEADERS} ${IMGUI_SOURCES} ${POM2_PLATFORM_SOURCES})
    set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "POM2")
    target_link_libraries(${PROJECT_NAME} POM2::core)

# Convenience alias: the CMake target is ${PROJECT_NAME} (pom2_imgui) but the
# output binary is POM2, so `cmake --build . --target POM2` is otherwise a
# silent no-op (make: "Nothing to be done for 'POM2'"). This phony target maps
# the intuitive name back to the real one. Use either; `cmake --build .` (no
# --target) still builds everything.
add_custom_target(POM2 DEPENDS ${PROJECT_NAME})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${POM2_SRC_DIR}
    ${POM2_SRC_DIR}/hgrpaint   # portable HGR Paint editor (shared with POM1)
    ${POM2_SRC_DIR}/hgrsprite  # portable HGR sprite editor (same host seam)
    ${POM2_GENERATED_DIR}   # generated Version.h
)

# ─── Platform deps ───────────────────────────────────────────────────────
if(EMSCRIPTEN)
    # Browser build: Emscripten ships GLFW3 + WebGL2 ports built-in. The
    # host-socket bridges (telnet, AI control HTTP) are compiled out via
    # the POM2_HAS_SOCKETS gate (Pom2Build.h — 0 under Emscripten) in
    # SuperSerialTcpTransport.cpp / AiControlServer.cpp — symbols stay declared
    # so callers still link.
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
        "--shell-file" "${CMAKE_CURRENT_SOURCE_DIR}/wasm/shell.html"
    )
    # Preload the package payload into POM2.data. Each clause maps an on-disk
    # source to the virtual path the binary's std::ifstream / ResourcePaths
    # probes resolve against (`./roms/...`, `./fonts/...` — exactly what the
    # CWD-based search finds on native), so the browser build reads its assets
    # through the same code path as every desktop package.
    #
    # The LIST comes from packaging/bundle.manifest, parsed once at the top of
    # this file — the same list the install() rules use. `wasm` entries are the
    # browser-only extras: today that is floppyemu/, which `wasm/shell.html`
    # boots by default (`floppyemu/Total Replay v6.1.hdv`) and which desktop
    # packages deliberately skip rather than charge every download 33 MB for a
    # single HDV. `deny` entries (disks_5.4, hdv, …) are hundreds of MB and
    # would blow past GitHub Pages' per-file limits and choke browser preload.
    #
    # Apple ROMs are always bundled — without them the emulator can't boot, and
    # the browser build has no drag-and-drop ROM upload path yet. Populating
    # `roms/` before building WASM is the user's job (same constraint as
    # native); we just guarantee the folder ships when it exists.
    #
    # --preload-file: pass src+dst as ONE token via SHELL: so CMake doesn't try
    # to interpret the bare `@` as a response-file marker.
    set(POM2_WASM_PRELOAD "")
    foreach(_pom2_dir ${POM2_BUNDLE_DIRS} ${POM2_BUNDLE_WASM})
        list(APPEND POM2_WASM_PRELOAD "${_pom2_dir}|${_pom2_dir}")
    endforeach()
    list(APPEND POM2_WASM_PRELOAD ${POM2_BUNDLE_FILES})
    # Opt-in extra kept from the previous hand-written list: the small 3.5"
    # library. `deny`-listed for packages, so it is a build-time choice here
    # and never a default.
    option(POM2_WASM_BUNDLE_DISKS
        "Include disks_3.5 in the WASM bundle (disks_5.4 + hdv are excluded — too large)" OFF)
    if(POM2_WASM_BUNDLE_DISKS)
        list(APPEND POM2_WASM_PRELOAD "disks_3.5|disks_3.5")
    endif()
    foreach(_pom2_pair ${POM2_WASM_PRELOAD})
        string(REPLACE "|" ";" _pom2_parts "${_pom2_pair}")
        list(GET _pom2_parts 0 _pom2_src)
        list(GET _pom2_parts 1 _pom2_dst)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_pom2_src}")
            list(APPEND POM2_WASM_LINK
                "SHELL:--preload-file ${CMAKE_CURRENT_SOURCE_DIR}/${_pom2_src}@/${_pom2_dst}")
        endif()
    endforeach()
    # emcc's --exclude-file applies to every --preload-file above. Same reason
    # as the install() PATTERN EXCLUDE: a preloaded folder is the working tree,
    # and an uncommitted archive in roms/ would otherwise be downloaded by every
    # visitor to the browser demo.
    foreach(_g ${POM2_BUNDLE_DENYGLOB})
        list(APPEND POM2_WASM_LINK "SHELL:--exclude-file ${_g}")
    endforeach()
    target_link_options(${PROJECT_NAME} PRIVATE ${POM2_WASM_LINK})
    # Match shell.html's <canvas id="canvas"> ID convention.
    set_target_properties(${PROJECT_NAME} PROPERTIES
        OUTPUT_NAME "POM2"
        SUFFIX      ".html"
    )
else()
    # On the GLES tier we link libGLESv2 + libEGL instead of libGL.
    # find_package(OpenGL) would drag in the desktop driver stack, which on a
    # Raspberry Pi is the software rasteriser fallback — a silent ~2 fps
    # regression rather than an honest link error.
    if(NOT POM2_GLES)
        find_package(OpenGL REQUIRED)
    endif()

    find_package(glfw3 3.3 QUIET CONFIG)
    if(glfw3_FOUND)
        set(GLFW_TARGET glfw)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(GLFW3 REQUIRED IMPORTED_TARGET glfw3)
        set(GLFW_TARGET PkgConfig::GLFW3)
    endif()

    target_link_libraries(${PROJECT_NAME} ${GLFW_TARGET})

    if(POM2_GLES)
        # libGLESv2 declares AND exports every GL 2.0+ entry point, so the GLES
        # TUs call them directly and skip the glfwGetProcAddress dance that the
        # desktop Linux/Windows path needs (see POM2_GL_ES in OpenGLShader.cpp /
        # CrtEffectStack.cpp / NtscPostProcessor.cpp).
        #
        # EGL is located but NOT meaningfully linked: main.cpp asks GLFW for
        # GLFW_EGL_CONTEXT_API (GLX can only hand out a GLES context when the X
        # server advertises GLX_EXT_create_context_es2_profile, which V3D —
        # the Raspberry Pi's driver — does not), and GLFW **dlopens** libEGL
        # itself at that point. POM2 calls no EGL symbol directly, so the
        # linker drops the reference under --as-needed and libEGL never becomes
        # a DT_NEEDED of the binary. Verified: `readelf -d` shows libGLESv2
        # only.
        #
        # find_library is therefore a PRESENCE CHECK, and a valuable one: a
        # system missing libEGL would otherwise configure and build perfectly,
        # then fail at runtime with an opaque "context creation failed" the
        # first time a user launches it. Failing here names the missing package
        # instead.
        find_library(POM2_GLESV2_LIB NAMES GLESv2)
        find_library(POM2_EGL_LIB    NAMES EGL)
        if(NOT POM2_GLESV2_LIB OR NOT POM2_EGL_LIB)
            message(FATAL_ERROR
                "POM2_GLES=ON but libGLESv2 / libEGL were not found. "
                "On Debian / Raspberry Pi OS: "
                "apt install libgles2-mesa-dev libegl1-mesa-dev")
        endif()
        target_link_libraries(${PROJECT_NAME} ${POM2_GLESV2_LIB} ${POM2_EGL_LIB})
        # POM2_BUILD_GLES flips Pom2Build.h's POM2_GL_ES; IMGUI_IMPL_OPENGL_ES3
        # switches Dear ImGui's GL3 backend to the same profile (the WASM build
        # already sets it for the same reason).
        target_compile_definitions(${PROJECT_NAME} PRIVATE
            POM2_BUILD_GLES=1
            IMGUI_IMPL_OPENGL_ES3)
    else()
        target_link_libraries(${PROJECT_NAME} ${OPENGL_LIBRARIES})
    endif()

    # Miniaudio + stb_vorbis (vendored single-header decoders) need libdl /
    # libpthread / libm on Linux; CoreAudio + AudioToolbox + AudioUnit on
    # macOS; nothing extra on Windows beyond winmm.
    if(UNIX AND NOT APPLE)
        find_package(Threads REQUIRED)
        foreach(_core_target IN LISTS POM2_CORE_TARGETS)
            target_link_libraries(${_core_target} PUBLIC
                ${CMAKE_DL_LIBS} Threads::Threads m)
        endforeach()
    endif()
    if(APPLE)
        foreach(_core_target IN LISTS POM2_CORE_TARGETS)
            target_link_libraries(${_core_target} PUBLIC
                "-framework CoreAudio"
                "-framework AudioToolbox"
                "-framework AudioUnit"
            )
        endforeach()
    endif()
    if(WIN32)
        # ws2_32: the whole Winsock2 API. Since 2026-08-01 Windows is a full
        # host-socket target (POM2_HAS_SOCKETS is 1 there) — the Uthernet II's
        # TCP/UDP sockets, the SSC telnet bridge and the AI control server all
        # go through it, on top of the htons/ntohs framing helpers that were
        # the only reason to link it before. See src/SocketCompat.h.
        foreach(_core_target IN LISTS POM2_CORE_TARGETS)
            target_link_libraries(${_core_target} PUBLIC winmm ws2_32)
        endforeach()
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
endif() # POM2_BUILD_FRONTEND

# Core/headless configurations deliberately skip GLFW/OpenGL discovery above,
# but the reusable archive still needs the platform services used by audio and
# host sockets. The full frontend path already applies the same links inside
# its native-platform branch.
if(NOT POM2_BUILD_FRONTEND AND NOT EMSCRIPTEN)
    if(UNIX AND NOT APPLE)
        find_package(Threads REQUIRED)
        foreach(_core_target IN LISTS POM2_CORE_TARGETS)
            target_link_libraries(${_core_target} PUBLIC
                ${CMAKE_DL_LIBS} Threads::Threads m)
        endforeach()
    endif()
    if(APPLE)
        foreach(_core_target IN LISTS POM2_CORE_TARGETS)
            target_link_libraries(${_core_target} PUBLIC
                "-framework CoreAudio"
                "-framework AudioToolbox"
                "-framework AudioUnit")
        endforeach()
    endif()
    if(WIN32)
        foreach(_core_target IN LISTS POM2_CORE_TARGETS)
            target_link_libraries(${_core_target} PUBLIC winmm ws2_32)
        endforeach()
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
    src/pom2_headless.cpp
)
target_link_libraries(pom2_headless PRIVATE POM2::core)
endif() # NOT EMSCRIPTEN (pom2_headless)

# ─── pom2_bench ───────────────────────────────────────────────────────────
# Deterministic headless run of N frames — no threads, no audio device, no
# sockets, no wall-clock pacing. Two roles, both of which need that closed
# shape (see the header comment in src/pom2_bench.cpp):
#   * callgrind subject — instruction counts are stable run to run, which
#     compares two builds far better than wall time;
#   * PGO training driver — packaging/raspberry/pgo_train.sh sweeps it over
#     several machine profiles and video pipelines.
# It links the emulator core + Apple2Display only; nothing that would pull in
# GLFW/ImGui or open a device.
if(NOT EMSCRIPTEN)
add_executable(pom2_bench
    src/pom2_bench.cpp
)
target_link_libraries(pom2_bench PRIVATE POM2::core)
endif() # NOT EMSCRIPTEN (pom2_bench)

if(MSVC)
    # Core sources compile in the layer OBJECT targets; setting options only
    # on the aggregate archive would not reach them.
    set(_pom2_msvc_targets ${POM2_CORE_COMPILE_TARGETS})
    if(TARGET ${PROJECT_NAME})
        list(APPEND _pom2_msvc_targets ${PROJECT_NAME})
    endif()
    if(TARGET pom2_headless)
        list(APPEND _pom2_msvc_targets pom2_headless)
    endif()
    if(TARGET pom2_bench)
        list(APPEND _pom2_msvc_targets pom2_bench)
    endif()
    foreach(_target IN LISTS _pom2_msvc_targets)
        target_compile_definitions(${_target} PRIVATE
            _CRT_SECURE_NO_WARNINGS
            NOMINMAX
            WIN32_LEAN_AND_MEAN)
        target_compile_options(${_target} PRIVATE /utf-8)
    endforeach()
endif()

# ─── Windows: static CRT ───────────────────────────────────────────────────
# Link the C runtime statically (/MT) so the released POM2.exe ships ALONE, with
# no app-local vcruntime140.dll / msvcp140.dll beside it.
#
# This is not a size preference, it is a correctness one. When an app-local
# msvcp140.dll sits next to the exe, the loader hands OUR copy to the GPU
# vendor's OpenGL ICD while msvcp140_1.dll still comes from System32 — a mix the
# lockstep-versioned msvcp140* family does not tolerate, and the ICD then fails
# to load. The symptom is a black window or an outright launch failure on
# machines whose installed redistributable differs from the build agent's, and
# it is invisible in CI (runners have no vendor GPU and fall back to software
# GL). The release workflow enforces the result structurally instead: zero DLLs
# in the package root and an import table naming no CRT.
#
# Requires GLFW built with the matching runtime — hence the x64-windows-static
# vcpkg triplet in the release workflow, which already sets
# VCPKG_CRT_LINKAGE=static. Do NOT use x64-windows-static-md: that keeps the CRT
# dynamic and puts the app-local DLL right back.
option(POM2_WIN_STATIC_RUNTIME "Link the MSVC runtime statically (/MT)" ON)
if(MSVC AND POM2_WIN_STATIC_RUNTIME)
    set(_pom2_static_crt_targets ${POM2_CORE_COMPILE_TARGETS})
    if(TARGET ${PROJECT_NAME})
        list(APPEND _pom2_static_crt_targets ${PROJECT_NAME})
    endif()
    if(TARGET pom2_headless)
        list(APPEND _pom2_static_crt_targets pom2_headless)
    endif()
    if(TARGET pom2_bench)
        list(APPEND _pom2_static_crt_targets pom2_bench)
    endif()
    foreach(_target IN LISTS _pom2_static_crt_targets)
        set_property(TARGET ${_target} PROPERTY
                     MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endforeach()
endif()

# ─── Tests ───────────────────────────────────────────────────────────────
if(POM2_ENABLE_TESTS AND NOT EMSCRIPTEN)
    enable_testing()
    add_subdirectory(tests)
endif()

# Enable LTO on non-Debug builds when the toolchain supports it.
include(CheckIPOSupported)
check_ipo_supported(RESULT POM2_IPO_SUPPORTED OUTPUT POM2_IPO_ERROR)
if(POM2_IPO_SUPPORTED)
    if(TARGET ${PROJECT_NAME})
        set_property(TARGET ${PROJECT_NAME} PROPERTY
                     INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        set_property(TARGET ${PROJECT_NAME} PROPERTY
                     INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
    endif()
    # The expensive machine code now compiles in the layer OBJECT targets.
    # Keep the shipped/bench optimisation contract on both those compilation
    # units and their aggregate archive; an explicitly supplied global IPO
    # setting still wins for profiling and PGO pass-1 builds.
    if(NOT DEFINED CMAKE_INTERPROCEDURAL_OPTIMIZATION)
        foreach(_core_ipo_target pom2_core ${POM2_CORE_OBJECT_TARGETS})
            set_property(TARGET ${_core_ipo_target} PROPERTY
                         INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
            set_property(TARGET ${_core_ipo_target} PROPERTY
                         INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
        endforeach()
    endif()
    # pom2_bench must be built EXACTLY like the shipped binary or its numbers
    # describe a build nobody runs — LTO alone is worth ~20 % on the emulation
    # core (docs/PERFORMANCE.md § build recipe). Skipped when the caller sets
    # CMAKE_INTERPROCEDURAL_OPTIMIZATION explicitly, which is how the profiling
    # and PGO-pass-1 builds ask for LTO off.
    if(TARGET pom2_bench AND NOT DEFINED CMAKE_INTERPROCEDURAL_OPTIMIZATION)
        set_property(TARGET pom2_bench PROPERTY
                     INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        set_property(TARGET pom2_bench PROPERTY
                     INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
    endif()
endif()

# ─── Install + packaging ──────────────────────────────────────────────────
# Layout: the binary in <prefix>/bin, redistributable assets in
# <prefix>/share/POM2. ResourcePaths resolves assets via <exeDir>/../share/
# POM2 (and the per-user ~/.local/share/POM2), so this same tree works for
# a relocatable tarball (CPack TGZ), an FHS /usr install (CPack DEB), and an
# AppImage. The full roms/ tree (system + peripheral dumps + floppy_samples)
# ships with every artifact so a release boots out of the box.
set(POM2_DATADIR "${CMAKE_INSTALL_DATADIR}/POM2")
# GNUInstallDirs derives DOCDIR from the (internal) project name pom2_imgui;
# force the user-facing "POM2" so docs land in share/doc/POM2.
set(CMAKE_INSTALL_DOCDIR "${CMAKE_INSTALL_DATADIR}/doc/POM2")

# Optional development package for embedding the deterministic emulator core.
# Runtime packagers leave this OFF so AppImages/application archives do not
# carry a static archive and C++ headers. SDK builds enable it explicitly.
if(POM2_INSTALL_CORE_SDK)
    include(CMakePackageConfigHelpers)

    install(TARGETS pom2_core
            EXPORT pom2_coreTargets
            ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
                    COMPONENT pom2_core_sdk
            LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
                    COMPONENT pom2_core_sdk
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
                    COMPONENT pom2_core_sdk
            INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
    install(FILES ${POM2_CORE_PUBLIC_HEADERS}
            DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/pom2"
            COMPONENT pom2_core_sdk)

    set(POM2_CORE_PACKAGE_NEEDS_THREADS OFF)
    if(UNIX AND NOT APPLE)
        set(POM2_CORE_PACKAGE_NEEDS_THREADS ON)
    endif()
    set(POM2_CORE_PACKAGE_WITH_SLIRP OFF)
    if(POM2_SLIRP_TARGET)
        set(POM2_CORE_PACKAGE_WITH_SLIRP ON)
    endif()

    set(POM2_CORE_CMAKE_DIR
        "${CMAKE_INSTALL_LIBDIR}/cmake/pom2_core")
    configure_package_config_file(
        cmake/pom2_coreConfig.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/pom2_coreConfig.cmake"
        INSTALL_DESTINATION "${POM2_CORE_CMAKE_DIR}")
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/pom2_coreConfigVersion.cmake"
        VERSION "${PROJECT_VERSION}"
        COMPATIBILITY SameMajorVersion)

    install(EXPORT pom2_coreTargets
            FILE pom2_coreTargets.cmake
            NAMESPACE POM2::
            DESTINATION "${POM2_CORE_CMAKE_DIR}"
            COMPONENT pom2_core_sdk)
    install(FILES
            "${CMAKE_CURRENT_BINARY_DIR}/pom2_coreConfig.cmake"
            "${CMAKE_CURRENT_BINARY_DIR}/pom2_coreConfigVersion.cmake"
            DESTINATION "${POM2_CORE_CMAKE_DIR}"
            COMPONENT pom2_core_sdk)
endif()

if(TARGET ${PROJECT_NAME})
    install(TARGETS ${PROJECT_NAME}
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()

# The console binary ships too. Two reasons, and the second is why it is not
# optional: it gives users a display-less way to drive the machine (telnet), and
# it is what every release job runs as its BOOT SMOKE — `pom2_headless --frames
# 300 --screenshot` against the packaged tree proves the package resolved its
# own bundled roms/ and executed 6502 code. `POM2 --help` proves neither, and a
# package whose ROMs did not land looks perfectly healthy to it.
if(TARGET pom2_headless)
    install(TARGETS pom2_headless
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()

# ─── Package payload, driven by packaging/bundle.manifest ──────────────────
# The manifest is the single source of truth for what ships inside a package
# (see pom2_read_bundle_manifest above). These install() rules and the WASM
# --preload-file list are both generated from it, so a folder can no longer be
# in the browser bundle and missing from the desktop packages — which is what
# happened to fonts/, pic/ and floppyemu/ while four hand-written lists drifted.
foreach(_pom2_dir ${POM2_BUNDLE_DIRS})
    # A tree without roms/ still configures (source checkouts without the
    # dumps); release builds always have it, and stage_data.sh --verify is
    # what fails a package that lost it.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_pom2_dir}")
        # PATTERN ... EXCLUDE for every denyglob: install(DIRECTORY) copies the
        # working tree, so an uncommitted archive in roms/ would otherwise be
        # packaged (it happened — see packaging/bundle.manifest).
        set(_pom2_excl "")
        foreach(_g ${POM2_BUNDLE_DENYGLOB})
            list(APPEND _pom2_excl PATTERN "${_g}" EXCLUDE)
        endforeach()
        install(DIRECTORY "${_pom2_dir}/"
                DESTINATION "${POM2_DATADIR}/${_pom2_dir}"
                ${_pom2_excl})
    endif()
endforeach()
foreach(_pom2_pair ${POM2_BUNDLE_FILES})
    # "<src>|<dst>" — split, then install with the destination's own name so a
    # manifest rename (packaging/roms_README.txt -> roms/README.txt) works.
    string(REPLACE "|" ";" _pom2_parts "${_pom2_pair}")
    list(GET _pom2_parts 0 _pom2_src)
    list(GET _pom2_parts 1 _pom2_dst)
    get_filename_component(_pom2_dst_dir  "${_pom2_dst}" DIRECTORY)
    get_filename_component(_pom2_dst_name "${_pom2_dst}" NAME)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_pom2_src}")
        install(FILES "${_pom2_src}"
                DESTINATION "${POM2_DATADIR}/${_pom2_dst_dir}"
                RENAME "${_pom2_dst_name}")
    endif()
endforeach()

# Docs + desktop integration.
install(FILES README.md CHANGELOG.md DEV.md TODO.md LICENSE
        DESTINATION "${CMAKE_INSTALL_DOCDIR}")
install(FILES packaging/POM2.desktop
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")

# Application icon (hicolor theme): scalable SVG + rasterised PNGs. The
# .desktop's `Icon=POM2` and the MIME type's <icon> resolve to these; AppImage
# tooling (linuxdeploy) also picks the icon up from here.
install(FILES packaging/POM2.svg
        DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps")
foreach(_sz 16 32 48 64 128 256)
    install(FILES "packaging/POM2-${_sz}.png"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${_sz}x${_sz}/apps"
            RENAME POM2.png)
endforeach()

# Disk-image MIME type (application/x-apple2-disk) referenced by POM2.desktop's
# MimeType=, so a double-clicked .dsk/.woz/.hdv/.2mg/… opens in POM2.
install(FILES packaging/pom2-disk.xml
        DESTINATION "${CMAKE_INSTALL_DATADIR}/mime/packages")

# ─── CPack ─────────────────────────────────────────────────────────────────
# `cpack -G TGZ` → relocatable POM2-<ver>-Linux.tar.gz (bin/ + share/POM2).
# `cpack -G DEB` → .deb depending on the system GLFW + OpenGL runtimes.
# build_dist.sh drives this and collects the artifacts into DIST/.
set(CPACK_PACKAGE_NAME            "POM2")
set(CPACK_PACKAGE_VENDOR          "POM2")
set(CPACK_PACKAGE_VERSION         "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Apple II / II+ / //e / //c / //c+ emulator")
set(CPACK_PACKAGE_HOMEPAGE_URL    "https://github.com/habib256/pom2")
set(CPACK_RESOURCE_FILE_LICENSE   "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_CONTACT         "gist974@gmail.com")
set(CPACK_PACKAGE_FILE_NAME
    "POM2-v${PROJECT_VERSION}-linux-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_STRIP_FILES             ON)

# Debian package: link against the distro's shared GLFW + OpenGL.
set(CPACK_DEBIAN_PACKAGE_DEPENDS  "libglfw3 (>= 3.3), libgl1")
set(CPACK_DEBIAN_PACKAGE_SECTION  "otherosfs")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
# Maintainer scripts: refresh the MIME / desktop / icon caches on install &
# removal so the disk-image association and app icon take effect immediately.
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/deb/postinst"
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/deb/postrm")

include(CPack)
