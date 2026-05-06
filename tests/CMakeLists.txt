# Smoke tests for POM2.
#
# Native-only — Emscripten/WASM is not in scope. Tests are headless: they
# drive the M6502 + Memory directly without an ImGui context, so they link
# only the bare emulator core (no GLFW, no OpenGL, no Dear ImGui).

# ─── Klaus Dormann 6502 functional test ───────────────────────────────────
# Downloads the pre-assembled 64 KB image once at configure time (SHA-256
# pinned) and registers a ctest target that runs the emulator core against
# it. Gate for any change to M6502 or Memory's flat-RAM access path.

set(KLAUS_BIN "${CMAKE_CURRENT_BINARY_DIR}/klaus_6502_functional_test.bin")
set(KLAUS_URL "https://raw.githubusercontent.com/Klaus2m5/6502_65C02_functional_tests/master/bin_files/6502_functional_test.bin")
set(KLAUS_SHA256 "fa12bfc761e6f9057e4cc01a665a7b800ff01ae91f598af1e39a1201d01953fd")

if(NOT EXISTS "${KLAUS_BIN}")
    message(STATUS "Downloading Klaus 6502 functional test binary (~64 KB)...")
    file(DOWNLOAD "${KLAUS_URL}" "${KLAUS_BIN}"
         EXPECTED_HASH SHA256=${KLAUS_SHA256}
         TIMEOUT 30
         STATUS DOWNLOAD_STATUS)
    list(GET DOWNLOAD_STATUS 0 DOWNLOAD_CODE)
    if(NOT DOWNLOAD_CODE EQUAL 0)
        list(GET DOWNLOAD_STATUS 1 DOWNLOAD_MSG)
        message(WARNING "Klaus test binary download failed: ${DOWNLOAD_MSG} — "
                        "test_klaus_6502 will be built but will fail at run time "
                        "until the file is available at ${KLAUS_BIN}")
        file(REMOVE "${KLAUS_BIN}")
    endif()
endif()

# Minimal core needed for the test. Memory's destructor invokes the slot
# bus + cassette destructors, so they have to link too.
set(POM2_TEST_CORE_SOURCES
    ${CMAKE_SOURCE_DIR}/M6502.cpp
    ${CMAKE_SOURCE_DIR}/Memory.cpp
    ${CMAKE_SOURCE_DIR}/SlotBus.cpp
    ${CMAKE_SOURCE_DIR}/CassetteDevice.cpp
    ${CMAKE_SOURCE_DIR}/AudioDevice.cpp
    ${CMAKE_SOURCE_DIR}/RomLoader.cpp
    ${CMAKE_SOURCE_DIR}/SnapshotIO.cpp
    ${CMAKE_SOURCE_DIR}/SpeakerDevice.cpp
)

add_executable(test_klaus_6502
    klaus_6502_functional_test.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_klaus_6502 PRIVATE ${CMAKE_SOURCE_DIR})

if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(test_klaus_6502 PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_klaus_6502 PRIVATE
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_klaus_6502 PRIVATE winmm)
endif()

add_test(NAME klaus_6502_functional
         COMMAND test_klaus_6502 "${KLAUS_BIN}")
set_tests_properties(klaus_6502_functional PROPERTIES TIMEOUT 30)


# ─── Snapshot round-trip smoke test ───────────────────────────────────────
# Validates SnapshotIO end-to-end: writes a synthetic header + 2 sections,
# reads them back, asserts payloads match. Pure unit test — no Memory or
# M6502 needed.

add_executable(test_snapshot_io
    snapshot_io_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/SnapshotIO.cpp
)
target_include_directories(test_snapshot_io PRIVATE ${CMAKE_SOURCE_DIR})

add_test(NAME snapshot_io_smoke COMMAND test_snapshot_io)
set_tests_properties(snapshot_io_smoke PROPERTIES TIMEOUT 5)


# ─── Slot bus dispatch smoke test ─────────────────────────────────────────
# Self-contained: SlotBus + a fake SlotPeripheral. Asserts the active-
# expansion-slot latching, $CFFF disable, and empty-slot open-bus reads.

add_executable(test_slot_bus
    slot_bus_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/SlotBus.cpp
)
target_include_directories(test_slot_bus PRIVATE ${CMAKE_SOURCE_DIR})

add_test(NAME slot_bus_smoke COMMAND test_slot_bus)
set_tests_properties(slot_bus_smoke PROPERTIES TIMEOUT 5)


# ─── Paste pipeline smoke test ────────────────────────────────────────────
# Pins line-ending normalisation, control-byte filtering, 7-bit strip,
# the strobe-driven queue drain, and the kPasteMaxChars cap.
add_executable(test_paste
    paste_smoke_test.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_paste PRIVATE ${CMAKE_SOURCE_DIR})
if(UNIX AND NOT APPLE)
    target_link_libraries(test_paste PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_paste PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_paste PRIVATE winmm)
endif()

add_test(NAME paste_smoke COMMAND test_paste)
set_tests_properties(paste_smoke PROPERTIES TIMEOUT 5)


# ─── Language Card bank-switching smoke test ──────────────────────────────
# Pins the 16 KB Apple II/II+ Language Card overlay at $D000-$FFFF. This is
# required by ProDOS and hard-disk titles such as Total Replay.
add_executable(test_language_card
    language_card_smoke_test.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_language_card PRIVATE ${CMAKE_SOURCE_DIR})
if(UNIX AND NOT APPLE)
    target_link_libraries(test_language_card PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_language_card PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_language_card PRIVATE winmm)
endif()

add_test(NAME language_card_smoke COMMAND test_language_card)
set_tests_properties(language_card_smoke PROPERTIES TIMEOUT 5)


# ─── ProDOS HDV block-device card smoke test ──────────────────────────────
# Pins the slot-5 block-device ROM signature and the $C0D0-$C0D2 block read
# protocol used by the embedded ProDOS driver.
add_executable(test_hdv_card
    hdv_card_smoke_test.cpp
    ${POM2_TEST_CORE_SOURCES}
    ${CMAKE_SOURCE_DIR}/ProDOSHardDiskCard.cpp
)
target_include_directories(test_hdv_card PRIVATE ${CMAKE_SOURCE_DIR})
if(UNIX AND NOT APPLE)
    target_link_libraries(test_hdv_card PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_hdv_card PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_hdv_card PRIVATE winmm)
endif()

add_test(NAME hdv_card_smoke COMMAND test_hdv_card)
set_tests_properties(hdv_card_smoke PROPERTIES TIMEOUT 5)


# ─── Speaker 1-bit synthesis smoke test ───────────────────────────────────
# Self-contained: SpeakerDevice only. Pins silence-default, square-wave
# reconstruction at 1 kHz toggle rate, reset, and mute behaviour.
add_executable(test_speaker
    speaker_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/SpeakerDevice.cpp
)
target_include_directories(test_speaker PRIVATE ${CMAKE_SOURCE_DIR})

add_test(NAME speaker_smoke COMMAND test_speaker)
set_tests_properties(speaker_smoke PROPERTIES TIMEOUT 5)


# ─── HGR NTSC artifact rendering smoke test ───────────────────────────────
# Pins the LUT corners (black byte, all-on byte, isolated bit per palette
# pair × parity), the inter-byte white-bleed seam fix-up, and the glow
# pass-through invariant.
add_executable(test_hgr_render
    hgr_render_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/Apple2Display.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_hgr_render PRIVATE ${CMAKE_SOURCE_DIR})
if(UNIX AND NOT APPLE)
    target_link_libraries(test_hgr_render PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_hgr_render PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_hgr_render PRIVATE winmm)
endif()

add_test(NAME hgr_render_smoke COMMAND test_hgr_render)
set_tests_properties(hgr_render_smoke PROPERTIES TIMEOUT 5)


# ─── Le Chat Mauve RGB card smoke test ────────────────────────────────────
# Pins the FIFO clocking (Arlequin AN3+80COL sequence), the direct-from-
# byte ChatMauveRGB HGR decode (no MAME bit-doubler / half-dot delay /
# 7-bit LUT), the BW560 strict-monochrome path, the 16-colour lo-res
# palette with two distinct grays, and the silent NTSC fallback when the
# card is absent. Gate for any change to LeChatMauveCard.cpp or the
# ChatMauveRGB branches in Apple2Display.cpp.

add_executable(test_le_chat_mauve
    le_chat_mauve_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/Apple2Display.cpp
    ${CMAKE_SOURCE_DIR}/LeChatMauveCard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_le_chat_mauve PRIVATE ${CMAKE_SOURCE_DIR})
if(UNIX AND NOT APPLE)
    target_link_libraries(test_le_chat_mauve PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_le_chat_mauve PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_le_chat_mauve PRIVATE winmm)
endif()

add_test(NAME le_chat_mauve_smoke COMMAND test_le_chat_mauve)
set_tests_properties(le_chat_mauve_smoke PROPERTIES TIMEOUT 5)


# ─── DiskImage GCR round-trip smoke test ──────────────────────────────────
# Builds a synthetic .dsk, nibblizes it via DiskImage, then walks the
# track-0 buffer with the same GCR decode logic the Disk II controller's
# RWTS uses on read-back. Pins both 4-and-4 address-field encoding and
# 6-and-2 data-field encoding (incl. low2-reverse + bit-pair-swap). This
# is the gate for any change to DiskImage.cpp's encoder.

add_executable(test_disk_image
    disk_image_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/DiskImage.cpp
)
target_include_directories(test_disk_image PRIVATE ${CMAKE_SOURCE_DIR})

add_test(NAME disk_image_smoke COMMAND test_disk_image)
set_tests_properties(disk_image_smoke PROPERTIES TIMEOUT 5)


# ─── Disk II end-to-end boot smoke test ───────────────────────────────────
# Runs the full boot pipeline: Apple II ROM + Disk II PROM + .dsk image,
# CPU jumps to $C600, runs until the PROM exits, then compares the loaded
# $0800-$08FF page against the disk image. Skips silently when the user
# hasn't placed the required ROM/disk files (it's not a CI gate, just a
# debug aid for the boot pipeline).

add_executable(test_disk_boot
    disk_boot_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/DiskIICard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_disk_boot PRIVATE ${CMAKE_SOURCE_DIR})
if(UNIX AND NOT APPLE)
    target_link_libraries(test_disk_boot PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_disk_boot PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_disk_boot PRIVATE winmm)
endif()

add_test(NAME disk_boot_smoke COMMAND test_disk_boot)
set_tests_properties(disk_boot_smoke PROPERTIES TIMEOUT 30)
