// Disk II end-to-end boot smoke test.
//
// Wires the same components MainWindow does (Memory + M6502 + SlotBus +
// DiskIICard) and runs the CPU from $C600 for long enough that the boot
// PROM can: spin the motor, recalibrate the head, find D5 AA 96 / D5 AA AD,
// decode 256 bytes into $0800, and JMP $0801. The test then compares
// $0800-$08FF against the first 256 bytes of the .dsk image.
//
// Skips silently if the host hasn't placed apple2.rom + disk2.rom +
// dos33_master.dsk in the conventional locations. This isn't a CI gate —
// it's a debug aid for the boot pipeline that the project maintainer can
// re-run after touching DiskIICard or DiskImage.

#include "DiskIICard.h"
#include "DiskImage.h"
#include "M6502.h"
#include "Memory.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

bool fileExists(const std::string& p)
{
    std::error_code ec;
    return std::filesystem::is_regular_file(p, ec);
}

std::string findFirst(std::initializer_list<const char*> candidates)
{
    for (const char* c : candidates) if (fileExists(c)) return c;
    return {};
}

bool readFile(const std::string& path, std::vector<uint8_t>& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    out.resize(static_cast<size_t>(f.tellg()));
    f.seekg(0, std::ios::beg);
    f.read(reinterpret_cast<char*>(out.data()),
           static_cast<std::streamsize>(out.size()));
    return static_cast<bool>(f);
}

}  // namespace

int main()
{
    // Probe the standard locations (relative to the build dir, which is
    // where ctest cd's into).
    const std::string romPath  = findFirst({
        "../roms/apple2.rom", "roms/apple2.rom", "../../roms/apple2.rom" });
    const std::string promPath = findFirst({
        "../roms/disk2.rom", "roms/disk2.rom", "../../roms/disk2.rom" });
    const std::string dskPath  = findFirst({
        "../disks/dos33_master.dsk", "disks/dos33_master.dsk",
        "../../disks/dos33_master.dsk" });

    if (romPath.empty() || promPath.empty() || dskPath.empty()) {
        std::printf("disk_boot_smoke SKIP: missing one of"
                    " roms/apple2.rom, roms/disk2.rom, disks/dos33_master.dsk\n");
        return 0;
    }

    Memory mem;
    if (!mem.loadAppleIIRom(romPath.c_str())) {
        std::fprintf(stderr, "loadAppleIIRom failed\n");
        return 1;
    }

    auto card = std::make_unique<DiskIICard>();
    if (!card->loadBootRom(promPath)) {
        std::fprintf(stderr, "loadBootRom failed\n");
        return 1;
    }
    if (!card->insertDisk(dskPath)) {
        std::fprintf(stderr, "insertDisk failed: %s\n",
                     card->getLastError().c_str());
        return 1;
    }
    DiskIICard* cardRaw = card.get();
    mem.slotBus().plug(6, std::move(card));

    M6502 cpu(&mem);
    cpu.hardReset();
    mem.slotBus().reset();          // matches EmulationController::hardReset

    // Jump straight to the boot PROM, like the "Boot disk" UI button does.
    cpu.setProgramCounter(0xC600);
    const uint8_t* ram = mem.data();

    // Run for ~5 M cycles. The PROM's recalibration takes ~250K, then
    // ~50K to find D5 AA 96 + read 343 nibbles, then JMP $0801 — total
    // ~300K. We can't exit on "PC leaves slot ROM" because the PROM does
    // JSR $FF58 / JSR $FCA8 (Monitor WAIT) routinely. Instead we stop
    // once PC has reached the boot sector page ($0800-$08FF), and the
    // sector size byte ($0800) is non-zero — i.e., the PROM has both
    // loaded data AND jumped to $0801.
    constexpr int kMaxCycles = 5'000'000;
    int totalCycles = 0;
    while (totalCycles < kMaxCycles) {
        const int slice = cpu.run(1024);
        totalCycles += slice;
        const uint16_t pc = cpu.getProgramCounter();
        if (pc >= 0x0800 && pc <= 0x08FF && ram[0x0800] != 0x00) break;
    }

    // Snapshot $0800-$08FF.
    std::array<uint8_t, 256> loaded;
    std::memcpy(loaded.data(), ram + 0x0800, 256);

    // Compare with .dsk first 256 bytes (logical sector 0 of track 0).
    std::vector<uint8_t> diskBytes;
    if (!readFile(dskPath, diskBytes) || diskBytes.size() < 256) {
        std::fprintf(stderr, "cannot re-read dsk for compare\n");
        return 2;
    }

    // Diagnostic: print head state + first 16 bytes of $0800.
    std::printf("disk_boot_smoke: ran %d cycles\n", totalCycles);
    std::printf("  PC after boot: $%04X\n", cpu.getProgramCounter());
    std::printf("  head half-track: %d, motor %s, disk pos: %d\n",
                cardRaw->getHalfTrack(),
                cardRaw->isMotorOn() ? "ON" : "off",
                cardRaw->getTrackPosition());
    std::printf("  $0800: ");
    for (int i = 0; i < 16; ++i) std::printf("%02X ", loaded[i]);
    std::printf("\n  .dsk : ");
    for (int i = 0; i < 16; ++i) std::printf("%02X ", diskBytes[i]);
    std::printf("\n");

    // Compare only the static portion. Boot1 mutates $08FE/$08FF at
    // runtime ($08FE += $08FF each time it loads another sector to a
    // shifted page; the JMP indirect at $084A jumps via $08FD/$08FE), so
    // those bytes can't be expected to still match the .dsk image once
    // the CPU has begun executing.
    constexpr int kStaticBytes = 254;     // $0800-$08FD inclusive
    int diffs = 0;
    int firstDiff = -1;
    for (int i = 0; i < kStaticBytes; ++i) {
        if (loaded[i] != diskBytes[i]) {
            if (firstDiff < 0) firstDiff = i;
            ++diffs;
        }
    }
    if (diffs == 0) {
        std::printf("disk_boot_smoke OK: $0800-$08FD matches .dsk[0..253]"
                    " (PROM read + GCR decode round-tripped cleanly)\n");
        return 0;
    }
    std::fprintf(stderr,
        "disk_boot_smoke FAIL: %d byte mismatches in $0800-$08FD;"
        " first diff at offset $%02X (got $%02X want $%02X)\n",
        diffs, firstDiff, loaded[firstDiff], diskBytes[firstDiff]);
    return 3;
}
