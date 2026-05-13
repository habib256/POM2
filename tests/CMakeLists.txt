# Smoke tests for POM2.
#
# Native-only — Emscripten/WASM is not in scope. Tests are headless: they
# drive the M6502 + Memory directly without an ImGui context, so they link
# only the bare emulator core (no GLFW, no OpenGL, no Dear ImGui).

# CRITICAL: tests use assert() heavily, but the parent CMakeLists builds in
# Release by default which adds -DNDEBUG (silently stripping every assert).
# We re-enable asserts for *test* compilation only via -UNDEBUG, applied
# after the Release flags so the last definition wins. Without this every
# test would print "OK" regardless of whether its checks actually held.
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-UNDEBUG>)

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


# ─── Klaus Dormann 65C02 extended opcodes test ────────────────────────────
# Same harness as the 6502 functional test, but driving the 65C02 (CMOS)
# extension binary. Covers every 65C02-only opcode: STZ, BRA, INA/DEA,
# PHX/PHY/PLX/PLY, BIT in additional modes, TSB/TRB, JMP (abs,X), the
# (zp)-indirect mode for ORA/AND/EOR/ADC/STA/LDA/CMP/SBC, the Rockwell
# RMB0..7/SMB0..7/BBR0..7/BBS0..7 set, and the WDC WAI/STP halts. Also
# pins the 65C02 decimal-mode N/V/Z behaviour (computed AFTER the BCD
# correction — see `om65c02.lst:11-14`).
#
# Gate for any change to M6502.cpp opcodes or the CMOS dispatch table.

set(KLAUS65_BIN "${CMAKE_CURRENT_BINARY_DIR}/65C02_extended_opcodes_test.bin")
set(KLAUS65_URL "https://raw.githubusercontent.com/Klaus2m5/6502_65C02_functional_tests/master/bin_files/65C02_extended_opcodes_test.bin")
set(KLAUS65_SHA256 "10a2a07fa240666fa610c46accebe8d42b1000feef3aae619da15a8d152869b2")

if(NOT EXISTS "${KLAUS65_BIN}")
    message(STATUS "Downloading Klaus 65C02 extended-opcodes test binary (~64 KB)...")
    file(DOWNLOAD "${KLAUS65_URL}" "${KLAUS65_BIN}"
         EXPECTED_HASH SHA256=${KLAUS65_SHA256}
         TIMEOUT 30
         STATUS DOWNLOAD65_STATUS)
    list(GET DOWNLOAD65_STATUS 0 DOWNLOAD65_CODE)
    if(NOT DOWNLOAD65_CODE EQUAL 0)
        list(GET DOWNLOAD65_STATUS 1 DOWNLOAD65_MSG)
        message(WARNING "Klaus 65C02 test binary download failed: ${DOWNLOAD65_MSG} — "
                        "test_klaus_65c02 will be built but will fail at run time "
                        "until the file is available at ${KLAUS65_BIN}")
        file(REMOVE "${KLAUS65_BIN}")
    endif()
endif()

add_executable(test_klaus_65c02
    klaus_65c02_extended_test.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_klaus_65c02 PRIVATE ${CMAKE_SOURCE_DIR})

if(UNIX AND NOT APPLE)
    target_link_libraries(test_klaus_65c02 PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_klaus_65c02 PRIVATE
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_klaus_65c02 PRIVATE winmm)
endif()

add_test(NAME klaus_65c02_extended
         COMMAND test_klaus_65c02 "${KLAUS65_BIN}")
set_tests_properties(klaus_65c02_extended PROPERTIES TIMEOUT 60)


# ─── System profile smoke test ─────────────────────────────────────────────
# Pins the 5 SystemProfile entries (II / II+ / IIe / IIc / IIc+) and their
# CPU/iieMode defaults. Walks a canonical profile-switch cycle (II+ →
# IIe → IIc → IIc+ → II → II+) and verifies Memory+CPU state stays
# consistent. Doesn't exercise the GUI applyProfile path (needs
# ImGui+GLFW) but locks the core invariants.
add_executable(test_system_profile
    system_profile_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/SystemProfile.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_system_profile PRIVATE ${CMAKE_SOURCE_DIR})
if(UNIX AND NOT APPLE)
    target_link_libraries(test_system_profile PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_system_profile PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_system_profile PRIVATE winmm)
endif()

add_test(NAME system_profile_smoke COMMAND test_system_profile)
set_tests_properties(system_profile_smoke PROPERTIES TIMEOUT 5)


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
    ${CMAKE_SOURCE_DIR}/ProDOSVolume.cpp
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


# ─── HDV write-back smoke test ────────────────────────────────────────────
# Pins .hdv plain round-trip, .2mg header preservation (incl. trailing
# comment chunk), 2MG WP flag honoured even with user opt-in, and opt-in
# default off (writes to a non-write-back card silently dropped, file
# untouched on eject).
add_executable(test_hdv_writeback
    hdv_writeback_smoke_test.cpp
    ${POM2_TEST_CORE_SOURCES}
    ${CMAKE_SOURCE_DIR}/ProDOSHardDiskCard.cpp
    ${CMAKE_SOURCE_DIR}/ProDOSVolume.cpp
)
target_include_directories(test_hdv_writeback PRIVATE ${CMAKE_SOURCE_DIR})
if(UNIX AND NOT APPLE)
    target_link_libraries(test_hdv_writeback PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_hdv_writeback PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_hdv_writeback PRIVATE winmm)
endif()

add_test(NAME hdv_writeback_smoke COMMAND test_hdv_writeback)
set_tests_properties(hdv_writeback_smoke PROPERTIES TIMEOUT 5)


# ─── ProDOS host-folder volume synthesizer smoke test ─────────────────────
# Builds a synthetic ProDOS volume from a temp folder of host files.
# Pins: volume header bytes (storage_type, name, file_count, total_blocks),
# file entry bytes for both seedling and sapling files, sapling index
# block layout, and round-trip data integrity (first block of a sapling
# file matches the first 512 bytes of the host file).
add_executable(test_prodos_volume
    prodos_volume_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/ProDOSVolume.cpp
)
target_include_directories(test_prodos_volume PRIVATE ${CMAKE_SOURCE_DIR})

add_test(NAME prodos_volume_smoke COMMAND test_prodos_volume)
set_tests_properties(prodos_volume_smoke PROPERTIES TIMEOUT 5)


# ─── VBL ($C019) scanline-accurate smoke test ─────────────────────────────
# Pins the 262-scanline × 65-cycle frame model, the bit-7 polarity (HIGH
# during active video 0..191, LOW during VBL 192..261), and the IIe VBL
# IRQ mask plumbing via $C05A/$C05B + clear-on-$C019-read.
add_executable(test_vbl
    vbl_smoke_test.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_vbl PRIVATE ${CMAKE_SOURCE_DIR})
if(UNIX AND NOT APPLE)
    target_link_libraries(test_vbl PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_vbl PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_vbl PRIVATE winmm)
endif()

add_test(NAME vbl_smoke COMMAND test_vbl)
set_tests_properties(vbl_smoke PROPERTIES TIMEOUT 5)


# ─── IRQ aggregator smoke test ────────────────────────────────────────────
# Pins the wire-OR semantics of `M6502::setIrqLine(sourceId, asserted)`:
# multiple sources can assert in parallel; releasing one does NOT drop
# the line while another is still asserting; legacy `setIRQ(int)` lives
# on its own back-compat source bit and coexists.
add_executable(test_irq_aggregator
    irq_aggregator_smoke_test.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_irq_aggregator PRIVATE ${CMAKE_SOURCE_DIR})
if(UNIX AND NOT APPLE)
    target_link_libraries(test_irq_aggregator PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_irq_aggregator PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_irq_aggregator PRIVATE winmm)
endif()

add_test(NAME irq_aggregator_smoke COMMAND test_irq_aggregator)
set_tests_properties(irq_aggregator_smoke PROPERTIES TIMEOUT 5)


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

# ─── Disk image skew-sniff smoke test ───────────────────────────────────
# Pins the content-driven DOS/ProDOS detection in DiskImage::loadFile.
# Verifies the cc65-Chess.po skew-override case (commit 50a8bf9) and that
# the strengthened name-char validation kills random $Fx false positives.
add_executable(test_disk_skew_sniff
    disk_skew_sniff_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/DiskImage.cpp
)
target_include_directories(test_disk_skew_sniff PRIVATE ${CMAKE_SOURCE_DIR})

add_test(NAME disk_skew_sniff_smoke COMMAND test_disk_skew_sniff)
set_tests_properties(disk_skew_sniff_smoke PROPERTIES TIMEOUT 5)

# ─── Disk image MacBinary 128-byte wrapper smoke test ───────────────────
# Pins the MacBinary header stripping in DiskImage::detectFormat — legacy
# Mac downloads sometimes carried a 128-byte prefix that, if not removed,
# would offset every nibble lookup and silently corrupt the boot.
add_executable(test_disk_macbinary
    disk_macbinary_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/DiskImage.cpp
)
target_include_directories(test_disk_macbinary PRIVATE ${CMAKE_SOURCE_DIR})

add_test(NAME disk_macbinary_smoke COMMAND test_disk_macbinary)
set_tests_properties(disk_macbinary_smoke PROPERTIES TIMEOUT 5)

# ─── Disk II LSS bit-level state-machine smoke test ───────────────────────
# Drives the bit-level Logic State Sequencer (port of MAME wozfdc.cpp /
# apple2js WozDiskDriver) against a synthetic single-prologue track and
# asserts the controller surfaces D5 AA 96 at $C0EC. Also pins the
# legacy 32-cycle gate fallback so removing roms/diskii_p6.rom does not
# regress stock DOS/ProDOS reads.

add_executable(test_diskii_lss
    diskii_lss_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/M6502.cpp
    ${CMAKE_SOURCE_DIR}/Memory.cpp
    ${CMAKE_SOURCE_DIR}/CassetteDevice.cpp
    ${CMAKE_SOURCE_DIR}/AudioDevice.cpp
    ${CMAKE_SOURCE_DIR}/SpeakerDevice.cpp
    ${CMAKE_SOURCE_DIR}/RomLoader.cpp
    ${CMAKE_SOURCE_DIR}/SlotBus.cpp
)
target_include_directories(test_diskii_lss PRIVATE ${CMAKE_SOURCE_DIR})
if(UNIX AND NOT APPLE)
    target_link_libraries(test_diskii_lss PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_diskii_lss PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_diskii_lss PRIVATE winmm)
endif()

add_test(NAME diskii_lss_smoke COMMAND test_diskii_lss
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(diskii_lss_smoke PROPERTIES TIMEOUT 10)


# ─── MAME wozfdc / floppy_image flux-model parity smoke test ──────────────
# Pins the verbatim port of MAME's Disk II LSS algorithm against MAME's
# source-of-truth invariants: flux event placement at cell centres,
# get_next_transition wrap semantics, end-to-end LSS read recovery, and
# writeFlux splice round-trip. Runs without the P6 ROM file (uses the
# embedded default) but the LSS read parity test skips silently if the
# bundled roms/diskii_p6.rom is missing.
add_executable(test_mame_lss_parity
    mame_lss_parity_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/SlotBus.cpp
)
target_include_directories(test_mame_lss_parity PRIVATE ${CMAKE_SOURCE_DIR})
if(UNIX AND NOT APPLE)
    target_link_libraries(test_mame_lss_parity PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_mame_lss_parity PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_mame_lss_parity PRIVATE winmm)
endif()

add_test(NAME mame_lss_parity_smoke COMMAND test_mame_lss_parity
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(mame_lss_parity_smoke PROPERTIES TIMEOUT 10)


# ─── Disk II write-back + .nib loader smoke test ──────────────────────────
# Pins .nib raw load+save (verbatim), .dsk write-back round-trip via the
# inverse GCR decoder, and the opt-in write-protect semantics.
add_executable(test_disk_writeback
    disk_writeback_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/DiskImage.cpp
)
target_include_directories(test_disk_writeback PRIVATE ${CMAKE_SOURCE_DIR})

add_test(NAME disk_writeback_smoke COMMAND test_disk_writeback)
set_tests_properties(disk_writeback_smoke PROPERTIES TIMEOUT 5)


# ─── Disk II controller write-side smoke test ────────────────────────────
# Drives the controller as a CPU would (write-mode + cycle-paced Q6H stores)
# and reads back the track to confirm the LSS pacing actually lands distinct
# bytes on disk. Pins the SAVE/LOAD I/O ERROR regression: before the fix
# every store collapsed to one value because writeLatch was overwritten
# without per-store cycle stamping.

add_executable(test_disk_write_controller
    disk_write_controller_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/SlotBus.cpp
)
target_include_directories(test_disk_write_controller PRIVATE ${CMAKE_SOURCE_DIR})

add_test(NAME disk_write_controller_smoke COMMAND test_disk_write_controller)
set_tests_properties(disk_write_controller_smoke PROPERTIES TIMEOUT 5)


# ─── WOZ1 / WOZ2 loader smoke test ───────────────────────────────────────
# Pins the verbatim port of MAME `src/lib/formats/woz_dsk.cpp`: minimal
# in-memory WOZ1 + WOZ2 files, chunk walk, MSB-first bit unpack, the
# always-WP override, plus an end-to-end LSS read that recovers the
# prologue from a synthetic .woz disk without loading the optional P6
# PROM file (the embedded default in DiskIICard's constructor suffices).
add_executable(test_woz_load
    woz_load_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/SlotBus.cpp
)
target_include_directories(test_woz_load PRIVATE ${CMAKE_SOURCE_DIR})
if(UNIX AND NOT APPLE)
    target_link_libraries(test_woz_load PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_woz_load PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_woz_load PRIVATE winmm)
endif()

add_test(NAME woz_load_smoke COMMAND test_woz_load
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(woz_load_smoke PROPERTIES TIMEOUT 10)


# ─── Disk II two-drive routing smoke test ────────────────────────────────
# Pins drive_select ($C0EA / $C0EB) routing the LSS and the legacy 32-cycle
# gate to the correct image, plus per-drive head-position independence.
# Self-contained: synthetic .nib images, no Apple II ROM dependency.
add_executable(test_disk_drive2
    disk_drive2_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/SlotBus.cpp
)
target_include_directories(test_disk_drive2 PRIVATE ${CMAKE_SOURCE_DIR})
if(UNIX AND NOT APPLE)
    target_link_libraries(test_disk_drive2 PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_disk_drive2 PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_disk_drive2 PRIVATE winmm)
endif()

add_test(NAME disk_drive2_smoke COMMAND test_disk_drive2
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(disk_drive2_smoke PROPERTIES TIMEOUT 10)


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


# ─── DOS 3.3 SAVE / LOAD round-trip smoke test ────────────────────────────
# End-to-end: boot DOS 3.3 into a writable .dsk copy, paste a small BASIC
# program, SAVE it, scan the text page for "I/O ERROR". Skips silently if
# the user hasn't placed apple2.rom + disk2.rom + dos33_master.dsk in the
# usual locations. Pin for the Disk II write pipeline.

add_executable(test_dos33_save
    dos33_save_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/DiskIICard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_dos33_save PRIVATE ${CMAKE_SOURCE_DIR})
if(UNIX AND NOT APPLE)
    target_link_libraries(test_dos33_save PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_dos33_save PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_dos33_save PRIVATE winmm)
endif()

add_test(NAME dos33_save_smoke COMMAND test_dos33_save)
set_tests_properties(dos33_save_smoke PROPERTIES TIMEOUT 60)


# ─── ProDOS 2.4.3 SAVE / LOAD round-trip smoke test ──────────────────────
# Same regression gate as dos33_save but driving a ProDOS .po image
# through BITSY BYE → BASIC → SAVE/LOAD. Skips silently when ProDOS_2_4_3.po
# isn't in disks/.

add_executable(test_prodos_save
    prodos_save_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/DiskIICard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_prodos_save PRIVATE ${CMAKE_SOURCE_DIR})
if(UNIX AND NOT APPLE)
    target_link_libraries(test_prodos_save PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_prodos_save PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_prodos_save PRIVATE winmm)
endif()

add_test(NAME prodos_save_smoke COMMAND test_prodos_save)
set_tests_properties(prodos_save_smoke PROPERTIES TIMEOUT 90)


# ─── Copy II Plus boot probe (diagnostic, not a CTest target) ────────────
add_executable(c2plus_boot_probe
    c2plus_boot_probe.cpp
    ${CMAKE_SOURCE_DIR}/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/Disassembler6502.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(c2plus_boot_probe PRIVATE ${CMAKE_SOURCE_DIR})
if(UNIX AND NOT APPLE)
    target_link_libraries(c2plus_boot_probe PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(c2plus_boot_probe PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(c2plus_boot_probe PRIVATE winmm)
endif()


# ─── Copy II Plus boot dump (diagnostic, not a CTest target) ─────────────
add_executable(copy2plus_boot_dump
    copy2plus_boot_dump.cpp
    ${CMAKE_SOURCE_DIR}/ClockCard.cpp
    ${CMAKE_SOURCE_DIR}/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/Disassembler6502.cpp
    ${CMAKE_SOURCE_DIR}/ProDOSHardDiskCard.cpp
    ${CMAKE_SOURCE_DIR}/ProDOSVolume.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(copy2plus_boot_dump PRIVATE ${CMAKE_SOURCE_DIR})
if(UNIX AND NOT APPLE)
    target_link_libraries(copy2plus_boot_dump PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(copy2plus_boot_dump PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(copy2plus_boot_dump PRIVATE winmm)
endif()


# ─── Apple IIe memory smoke test ──────────────────────────────────────────
# Pins the auxiliary 64 KB bank, the IIe paging soft switches (80STORE,
# RAMRD, RAMWRT, INTCXROM, ALTZP, SLOTC3ROM, 80COL, ALTCHAR), the status
# reads at $C013-$C018, $C01E, $C01F, and the internal I/O ROM dispatch
# at $C100-$CFFF (with the SLOTC3ROM exception for $C300-$C3FF). Includes
# an Apple II+ regression assertion (IIe paths must be gated behind
# setIIEMode(true)).
add_executable(test_iie_memory
    iie_memory_smoke_test.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_iie_memory PRIVATE ${CMAKE_SOURCE_DIR})
if(UNIX AND NOT APPLE)
    target_link_libraries(test_iie_memory PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_iie_memory PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_iie_memory PRIVATE winmm)
endif()

add_test(NAME iie_memory_smoke COMMAND test_iie_memory)
set_tests_properties(iie_memory_smoke PROPERTIES TIMEOUT 5)


# ─── IIe Language Card + ALTZP + RAMRD/WRT smoke test ────────────────────
# Pins the ProDOS-style LC paging sequences that the existing iie_memory
# test only partially covers: the prewrite latch under all four mode bits,
# ALTZP swapping the entire LC view to aux banks (incl. the shared $E000+
# 8 KB), and the write-enable surviving an ALTZP toggle.

add_executable(test_iie_lc_paging
    iie_lc_paging_smoke_test.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_iie_lc_paging PRIVATE ${CMAKE_SOURCE_DIR})
if(UNIX AND NOT APPLE)
    target_link_libraries(test_iie_lc_paging PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_iie_lc_paging PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_iie_lc_paging PRIVATE winmm)
endif()

add_test(NAME iie_lc_paging_smoke COMMAND test_iie_lc_paging)
set_tests_properties(iie_lc_paging_smoke PROPERTIES TIMEOUT 5)


# ─── IIe Double Hi-Res rendering smoke test ───────────────────────────────
# Pins the DHGR soft switches ($C05E/$C05F gated by iieMode), the 4-dot
# color-cell decode through the lo-res palette, the aux-then-main byte
# interleave, and the dispatcher's fall-back to 280-wide HGR when DHGR
# is disabled. Also exercises the mono phosphor tint path.
add_executable(test_dhgr_render
    dhgr_render_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/Apple2Display.cpp
    ${CMAKE_SOURCE_DIR}/LeChatMauveCard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_dhgr_render PRIVATE ${CMAKE_SOURCE_DIR})
if(UNIX AND NOT APPLE)
    target_link_libraries(test_dhgr_render PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_dhgr_render PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_dhgr_render PRIVATE winmm)
endif()

add_test(NAME dhgr_render_smoke COMMAND test_dhgr_render)
set_tests_properties(dhgr_render_smoke PROPERTIES TIMEOUT 5)


# ─── 65C02 instruction smoke test ─────────────────────────────────────────
# Pins the 65C02 additions on top of the 6502 base set: STZ, BRA, INA/DEA,
# PHX/PHY/PLX/PLY, BIT #imm, the (zp) addressing mode for ORA/AND/EOR/ADC/
# STA/LDA/CMP/SBC, TSB/TRB, JMP (abs,X). The Klaus 6502 functional test
# continues to gate the original 6502 semantics; this file gates the
# additions. Runs in flat-RAM Memory::setTestMode().
add_executable(test_cmos_6502
    cmos_6502_smoke_test.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_cmos_6502 PRIVATE ${CMAKE_SOURCE_DIR})
if(UNIX AND NOT APPLE)
    target_link_libraries(test_cmos_6502 PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_cmos_6502 PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_cmos_6502 PRIVATE winmm)
endif()

add_test(NAME cmos_6502_smoke COMMAND test_cmos_6502)
set_tests_properties(cmos_6502_smoke PROPERTIES TIMEOUT 5)


# ─── M68705P3 CPU decode smoke test ──────────────────────────────────────
# Pins the verbatim port of MAME's HMOS 6805 instruction set
# (src/devices/cpu/m6805/m6805.cpp) used by the Apple II Mouse Card. Drives
# synthetic 2 KB EPROM images and checks decode, addressing modes, stack
# semantics, IRQ vectoring + push order, and per-opcode cycle counts
# against MAME's documented invariants.
add_executable(test_m68705_decode
    m68705_decode_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/M68705P3.cpp
)
target_include_directories(test_m68705_decode PRIVATE ${CMAKE_SOURCE_DIR})
if(UNIX AND NOT APPLE)
    target_link_libraries(test_m68705_decode PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_m68705_decode PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_m68705_decode PRIVATE winmm)
endif()

add_test(NAME m68705_decode_smoke COMMAND test_m68705_decode)
set_tests_properties(m68705_decode_smoke PROPERTIES TIMEOUT 5)


# ─── MC6821 PIA smoke test ───────────────────────────────────────────────
# Pins the verbatim port of MAME's `src/devices/machine/6821pia.cpp`
# against MAME-equivalent invariants: DDR / data-port toggling via CRA
# bit 2, port composite read formula, CA1 edge-triggered IRQ flag, and
# the read-only nature of CRA/CRB bits 6/7.
add_executable(test_mc6821
    mc6821_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/MC6821.cpp
)
target_include_directories(test_mc6821 PRIVATE ${CMAKE_SOURCE_DIR})

add_test(NAME mc6821_smoke COMMAND test_mc6821)
set_tests_properties(mc6821_smoke PROPERTIES TIMEOUT 5)


# ─── Apple II Mouse Card wiring smoke test ───────────────────────────────
# Pins the verbatim port of MAME `bus/a2bus/mouse.cpp`: slot ROM bank
# select via PIA Port B bits 1..3 (8 banks of 256 bytes), the canonical
# detection signature passes through to the slot ROM window, and the
# card refuses to load without the two Apple-copyright ROMs.
add_executable(test_mouse_card
    mouse_card_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/M68705P3.cpp
    ${CMAKE_SOURCE_DIR}/MC6821.cpp
    ${CMAKE_SOURCE_DIR}/MouseCard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_mouse_card PRIVATE ${CMAKE_SOURCE_DIR})
if(UNIX AND NOT APPLE)
    target_link_libraries(test_mouse_card PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_mouse_card PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_mouse_card PRIVATE winmm)
endif()

add_test(NAME mouse_card_smoke COMMAND test_mouse_card)
set_tests_properties(mouse_card_smoke PROPERTIES TIMEOUT 5)


# ─── MouseCard quadrature edge smoke test ───────────────────────────────
# Pins MouseCard::updateAxis against MAME mouse.cpp's update_axis<>: each
# call emits at most one quadrature edge (CLK toggle + conditional DIR
# update) per axis when motion is pending; 8-bit wrap correction handles
# the unsigned-8-bit host coordinate input.
add_executable(test_mouse_card_quadrature
    mouse_card_quadrature_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/M68705P3.cpp
    ${CMAKE_SOURCE_DIR}/MC6821.cpp
    ${CMAKE_SOURCE_DIR}/MouseCard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_mouse_card_quadrature PRIVATE ${CMAKE_SOURCE_DIR})
if(UNIX AND NOT APPLE)
    target_link_libraries(test_mouse_card_quadrature PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_mouse_card_quadrature PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_mouse_card_quadrature PRIVATE winmm)
endif()

add_test(NAME mouse_card_quadrature_smoke COMMAND test_mouse_card_quadrature)
set_tests_properties(mouse_card_quadrature_smoke PROPERTIES TIMEOUT 5)


# ─── Slot configuration smoke test ───────────────────────────────────────
# Pins the slot constructor refactor (Phase 1 of the Mouse Card / Slot
# Configuration UI work): SuperSerialCard / ProDOSHardDiskCard rebuild
# their slot ROM signatures and trampolines around the constructor-
# supplied slot, and DiskIICard / LeChatMauveCard expose their slot via
# `getSlot()` for the slot-panel UI even when the ROM itself doesn't bake
# the slot in.
add_executable(test_slot_config
    slot_config_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/LeChatMauveCard.cpp
    ${CMAKE_SOURCE_DIR}/ProDOSHardDiskCard.cpp
    ${CMAKE_SOURCE_DIR}/ProDOSVolume.cpp
    ${CMAKE_SOURCE_DIR}/SuperSerialCard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_slot_config PRIVATE ${CMAKE_SOURCE_DIR})
if(UNIX AND NOT APPLE)
    target_link_libraries(test_slot_config PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_slot_config PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_slot_config PRIVATE winmm)
endif()

add_test(NAME slot_config_smoke COMMAND test_slot_config)
set_tests_properties(slot_config_smoke PROPERTIES TIMEOUT 5)


# ─── ProDOS clock card smoke test ────────────────────────────────────────
# Pins the ProDOS clock-detection signature ($08/$28/$58/$70 at offsets
# 0/2/4/6), the date/time wire format under a fixed timestamp, and an
# end-to-end CPU walk through the slot ROM (JSR $C400 → routine fills
# $BF90-$BF93 → RTS).
add_executable(test_clock_card
    clock_card_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/ClockCard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_clock_card PRIVATE ${CMAKE_SOURCE_DIR})
if(UNIX AND NOT APPLE)
    target_link_libraries(test_clock_card PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_clock_card PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_clock_card PRIVATE winmm)
endif()

add_test(NAME clock_card_smoke COMMAND test_clock_card)
set_tests_properties(clock_card_smoke PROPERTIES TIMEOUT 5)


# ─── Mockingboard smoke test ─────────────────────────────────────────────
# Pins the four behaviours music drivers depend on: $Cn0X / $Cn8X address
# decode (VIA1 / VIA2 selection from the slot ROM window), VIA → AY
# register addressing via the canonical LATCH → WRITE control sequence,
# 6522 timer 1 IRQ generation in both continuous and one-shot modes
# (incl. T1CL-read clearing IFR.T1 and dropping the slot IRQ line), and
# end-to-end AY tone synthesis producing a non-silent waveform.
add_executable(test_mockingboard
    mockingboard_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/Mockingboard.cpp
    ${CMAKE_SOURCE_DIR}/M6502.cpp
    ${CMAKE_SOURCE_DIR}/Memory.cpp
    ${CMAKE_SOURCE_DIR}/SlotBus.cpp
    ${CMAKE_SOURCE_DIR}/CassetteDevice.cpp
    ${CMAKE_SOURCE_DIR}/AudioDevice.cpp
    ${CMAKE_SOURCE_DIR}/SpeakerDevice.cpp
    ${CMAKE_SOURCE_DIR}/RomLoader.cpp
)
target_include_directories(test_mockingboard PRIVATE ${CMAKE_SOURCE_DIR})
if(UNIX AND NOT APPLE)
    target_link_libraries(test_mockingboard PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_mockingboard PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_mockingboard PRIVATE winmm)
endif()

add_test(NAME mockingboard_smoke COMMAND test_mockingboard)
set_tests_properties(mockingboard_smoke PROPERTIES TIMEOUT 5)


# ─── Super Serial Card ACIA smoke test ───────────────────────────────────
# Pins the 6551-shaped register behaviour against MAME `mos6551.cpp` as
# source of truth: echo mode (REM), read_rdr clearing OVERRUN/FRAMING/
# PARITY/RDRF, DTR side-effects (cmd bit 0 = 0 disables RX/TX IRQ + drops
# pending TX), DCD/DSR transition state tracking, overrun latching on
# ring overflow, control-register baud-rate decode (1.8432 MHz xtal), and
# programmed-reset preserving parity bits 5-7 in the command register.
# Pure ACIA unit test — the TCP bridge / worker thread is not exercised.
add_executable(test_ssc_acia
    ssc_acia_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/SuperSerialCard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_ssc_acia PRIVATE ${CMAKE_SOURCE_DIR})
if(UNIX AND NOT APPLE)
    target_link_libraries(test_ssc_acia PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_ssc_acia PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_ssc_acia PRIVATE winmm)
endif()

add_test(NAME ssc_acia_smoke COMMAND test_ssc_acia)
set_tests_properties(ssc_acia_smoke PROPERTIES TIMEOUT 5)


# ─── WOZ write-back smoke test ────────────────────────────────────────────
# Pins the WOZ write-back path: writeFlux splices into bitStream[qt],
# saveDirty() repacks dirty quarter-tracks back into the original file
# bytes (CRC32 zeroed per Applesauce WOZ 2.1 spec), and INFO.write_protected
# survives setWriteBackEnabled(true). Exercises both WOZ1 (fixed slot)
# and WOZ2 (block-indexed) variants. Pure DiskImage unit test — no
# DiskIICard / LSS plumbing involved.
add_executable(test_woz_writeback
    woz_writeback_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/DiskImage.cpp
)
target_include_directories(test_woz_writeback PRIVATE ${CMAKE_SOURCE_DIR})

add_test(NAME woz_writeback_smoke COMMAND test_woz_writeback)
set_tests_properties(woz_writeback_smoke PROPERTIES TIMEOUT 5)


# ─── H.E.R.O. read-hang probe (diagnostic, not a CTest target) ────────────
# Loads a .dsk image and walks the nibble buffer, the bit stream, and
# the actual LSS read path to spot where the prologue D5 AA 96 disappears.
# Used to investigate the long-standing "$96 repeated, no D5/AA seen"
# stall during the H.E.R.O. title screen load.
add_executable(hero_probe
    hero_probe.cpp
    ${CMAKE_SOURCE_DIR}/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/SlotBus.cpp
)
target_include_directories(hero_probe PRIVATE ${CMAKE_SOURCE_DIR})
if(UNIX AND NOT APPLE)
    target_link_libraries(hero_probe PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(hero_probe PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(hero_probe PRIVATE winmm)
endif()
