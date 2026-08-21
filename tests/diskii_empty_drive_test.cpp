// An EMPTY drive must not hang the guest.
//
// The universal "wait for a nibble" idiom on an Apple II is
//
//     LDA $C08C,X      ; Q6L — read the data latch
//     BPL *-3          ; spin until bit 7 comes up
//
// followed by the caller's own timeout counter. On real hardware a drive
// with no disk (or, for drive 2 on a one-drive machine, no drive at all)
// leaves the read amplifier on noise: the latch keeps shifting garbage,
// bit 7 comes up, the loop exits, and RWTS times out into a clean I/O
// error. POM2's bit-level LSS instead FROZE its data register for an
// unloaded drive, so bit 7 never came up and the loop spun forever — the
// guest never reached its own timeout, and the machine was dead.
//
// Found in Ultima V: "Save Music Configuration" writes to the BRITANNIA
// disk in DRIVE 2 and polls at $D407. With drive 2 empty POM2 hung there
// on any .woz. The legacy nibble gate never had the bug (deviceSelectRead
// returns $FF for an empty drive), which is exactly why the same game
// errored out cleanly from a .dsk and froze from a .woz — so this test
// pins BOTH gates, and both drives.
//
// See DiskIICard::lssSync's `!img.isLoaded()` branch.

#include "DiskIICard.h"
#include "DiskImage.h"
#include "M6502.h"
#include "Memory.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string findFirst(std::initializer_list<const char*> candidates)
{
    for (const char* p : candidates) {
        std::error_code ec;
        if (fs::is_regular_file(p, ec)) return p;
    }
    return {};
}

// A blank formatted-ish surface: 35 tracks of sync bytes. Enough to be a
// mountable image — this test never reads data off it, it only needs ONE
// drive to hold media so the card settles on the gate under test.
std::string makeSyntheticNib()
{
    const fs::path path = fs::temp_directory_path() / "pom2_empty_drive_probe.nib";
    std::vector<uint8_t> buf(
        static_cast<size_t>(DiskImage::kTracks) * DiskImage::kNibblesPerTrack, 0xFF);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return {};
    out.write(reinterpret_cast<const char*>(buf.data()),
              static_cast<std::streamsize>(buf.size()));
    return out ? path.string() : std::string{};
}

// Poll the data latch from real 6502 code, exactly as RWTS does, and
// report how many CPU cycles it took for bit 7 to come up. Returns -1 if
// the loop never exited within the budget (the hang this test guards),
// -2 on a setup problem.
long pollUntilByteReady(bool useBitLss, const std::string& p6Rom,
                        const std::string& nib, int emptyDrive,
                        bool bothDrivesEmpty)
{
    Memory mem;
    auto card = std::make_unique<DiskIICard>();
    // The P6 dump is what selects the bit-level LSS; without it the card
    // stays on the legacy 32-cycle nibble gate.
    if (useBitLss && !card->loadLssRom(p6Rom)) {
        std::fprintf(stderr, "FAIL: P6 LSS PROM would not load\n");
        return -2;
    }
    // `useBitLss` is recomputed on media events, so the mount below is what
    // arms it — mount into the drive we are NOT polling, then optionally
    // eject to reach the both-drives-empty case (the gate choice survives:
    // it follows the P6 dump, not the media).
    const int mediaDrive = 1 - emptyDrive;
    if (!card->insertDisk(mediaDrive, nib)) {
        std::fprintf(stderr, "FAIL: cannot mount %s\n", nib.c_str());
        return -2;
    }
    if (bothDrivesEmpty) card->ejectDisk(mediaDrive);

    DiskIICard* raw = card.get();
    mem.slotBus().plug(6, std::move(card));

    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.slotBus().reset();
    cpu.hardReset();

    if (raw->usingBitLss() != useBitLss) {
        std::fprintf(stderr, "FAIL: wanted bitLss=%d, card reports %d\n",
                     useBitLss ? 1 : 0, raw->usingBitLss() ? 1 : 0);
        return -2;
    }

    // $0300: motor on, select the empty drive, Q7L (read mode), then the
    // loop. $C0EA = drive 1, $C0EB = drive 2.
    const uint8_t driveSelect = static_cast<uint8_t>(emptyDrive == 0 ? 0xEA : 0xEB);
    const uint8_t prog[] = {
        0xAD, 0xE9, 0xC0,            // LDA $C0E9   motor on
        0xAD, driveSelect, 0xC0,     // LDA $C0EA/B drive select
        0xAD, 0xEE, 0xC0,            // LDA $C0EE   Q7 low (read mode)
        0xAD, 0xEC, 0xC0,            // LDA $C0EC   Q6 low + data latch
        0x10, 0xFB,                  // BPL -5      spin while bit 7 clear
        0x8D, 0x00, 0x20,            // STA $2000   keep the byte that arrived
        0x4C, 0x11, 0x03,            // JMP *       done marker
    };
    for (size_t i = 0; i < sizeof(prog); ++i)
        mem.memWrite(static_cast<uint16_t>(0x0300 + i), prog[i]);
    cpu.setProgramCounter(0x0300);

    // Two emulated seconds is ~7 disk revolutions — orders of magnitude
    // more than any real "wait for a byte" needs.
    const long budget = 2 * 1'022'727;
    long spent = 0;
    while (spent < budget) {
        spent += cpu.run(1024);
        if (cpu.getProgramCounter() == 0x0311) {          // loop exited
            if ((mem.data()[0x2000] & 0x80) == 0) {
                std::fprintf(stderr,
                    "FAIL: loop exited on $%02X — bit 7 clear?!\n",
                    mem.data()[0x2000]);
                return -2;
            }
            return spent;
        }
    }
    return -1;
}

struct Case {
    bool        bitLss;
    int         emptyDrive;      // 0 = drive 1, 1 = drive 2
    bool        bothEmpty;
    const char* what;
};

}  // namespace

int main()
{
    const std::string p6 = findFirst({
        "../roms/diskii_p6.rom", "roms/diskii_p6.rom", "../../roms/diskii_p6.rom" });
    if (p6.empty()) {
        std::printf("diskii_empty_drive SKIP: roms/diskii_p6.rom not found\n");
        return 0;
    }
    const std::string nib = makeSyntheticNib();
    if (nib.empty()) {
        std::fprintf(stderr, "FAIL: cannot write the synthetic .nib\n");
        return 1;
    }

    const Case cases[] = {
        // The regression, in the shape Ultima V hit it: media in drive 1,
        // the guest polls the empty drive 2, bit-level LSS.
        { true,  1, false, "bit-LSS, empty drive 2 (media in drive 1)" },
        { true,  0, false, "bit-LSS, empty drive 1 (media in drive 2)" },
        { true,  1, true,  "bit-LSS, both drives empty" },
        // The gate that was already correct — guard it from regressing.
        { false, 1, false, "legacy gate, empty drive 2 (media in drive 1)" },
        { false, 1, true,  "legacy gate, both drives empty" },
    };

    int failures = 0;
    for (const Case& c : cases) {
        const long cycles = pollUntilByteReady(c.bitLss, p6, nib,
                                               c.emptyDrive, c.bothEmpty);
        if (cycles == -2) return 1;
        if (cycles < 0) {
            std::fprintf(stderr,
                "FAIL: %s — `LDA $C0EC / BPL` never exited within 2 emulated "
                "seconds. This is the Ultima V $D407 hang.\n", c.what);
            ++failures;
        } else {
            std::printf("[ OK ] %-44s byte ready after %6ld cycles\n",
                        c.what, cycles);
        }
    }

    std::error_code ec;
    fs::remove(nib, ec);
    if (failures) return 2;
    std::printf("diskii_empty_drive OK\n");
    return 0;
}
