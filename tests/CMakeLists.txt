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
