// CLI kiosk / positional-disk smoke test. Pins the two pure pieces the
// `--kiosk` + bare `POM2 <disk>` launcher relies on:
//
//   1. parseCli()          — positional disk capture + --kiosk flag, and
//                            that they compose with existing flags.
//   2. classifyDiskForSlot — extension/size → slot class (5.25 / 3.5 / HDV),
//                            the routing the insert+boot path keys off.
//
// Both are dependency-free of the live emulator (parseCli's Phase-C runner
// lives in CliRunner.cpp), so this links just CliDispatcher.cpp + DiskImage.cpp.

#include "CliDispatcher.h"
#include "DiskImage.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

// Build a mutable argv from string args (parseCli takes char*[]).
std::optional<pom2::CliPlan> parse(const std::vector<std::string>& args,
                                   bool& help)
{
    std::vector<std::vector<char>> store;
    std::vector<char*> argv;
    for (const auto& a : args) {
        store.emplace_back(a.begin(), a.end());
        store.back().push_back('\0');
        argv.push_back(store.back().data());
    }
    return pom2::parseCli(static_cast<int>(argv.size()), argv.data(), help);
}

void testPositionalDisk()
{
    bool help = false;
    auto p = parse({"POM2", "game.dsk"}, help);
    assert(p.has_value());
    assert(!help);
    assert(p->bootDiskPath == "game.dsk");
    assert(!p->kiosk);
}

void testKioskFlagWithDisk()
{
    bool help = false;
    auto p = parse({"POM2", "--kiosk", "game.dsk"}, help);
    assert(p.has_value());
    assert(p->kiosk);
    assert(p->bootDiskPath == "game.dsk");
}

void testKioskFlagOnly()
{
    bool help = false;
    auto p = parse({"POM2", "--kiosk"}, help);
    assert(p.has_value());
    assert(p->kiosk);
    assert(p->bootDiskPath.empty());
}

void testPositionalComposesWithFlags()
{
    bool help = false;
    auto p = parse({"POM2", "--preset", "iie", "game.po"}, help);
    assert(p.has_value());
    assert(p->preset == pom2::CliPreset::AppleIIe);
    assert(p->bootDiskPath == "game.po");
}

void testTwoPositionalsRejected()
{
    bool help = false;
    auto p = parse({"POM2", "a.dsk", "b.dsk"}, help);
    assert(!p.has_value());     // only one disk image allowed
}

void testUnknownFlagStillRejected()
{
    bool help = false;
    auto p = parse({"POM2", "--bogus"}, help);
    assert(!p.has_value());
}

void testNoArgsCleanPlan()
{
    bool help = false;
    auto p = parse({"POM2"}, help);
    assert(p.has_value());
    assert(!p->kiosk);
    assert(p->bootDiskPath.empty());
}

// ── classifyDiskForSlot ──────────────────────────────────────────────────

// Create a temp file of exactly `size` bytes with the given name.
std::string makeFile(const fs::path& dir, const std::string& name, uint64_t size)
{
    const fs::path p = dir / name;
    std::ofstream f(p, std::ios::binary);
    if (size) { f.seekp(static_cast<std::streamoff>(size) - 1); f.put('\0'); }
    f.close();
    return p.string();
}

void testClassifier()
{
    fs::path dir = fs::temp_directory_path() / "pom2_cli_kiosk_test";
    fs::create_directories(dir);

    // 5.25" Disk II by extension (size-agnostic for these).
    assert(classifyDiskForSlot(makeFile(dir, "a.dsk", 143360)) == DiskSlotClass::Floppy525);
    assert(classifyDiskForSlot(makeFile(dir, "a.do",  143360)) == DiskSlotClass::Floppy525);
    assert(classifyDiskForSlot(makeFile(dir, "a.nib", 232960)) == DiskSlotClass::Floppy525);
    assert(classifyDiskForSlot(makeFile(dir, "a.woz", 200000)) == DiskSlotClass::Floppy525);
    assert(classifyDiskForSlot(makeFile(dir, "a.d13", 116480)) == DiskSlotClass::Floppy525);
    // .po @ 143360 = 5.25" ProDOS; @ 800K = 3.5".
    assert(classifyDiskForSlot(makeFile(dir, "p525.po", 143360)) == DiskSlotClass::Floppy525);
    assert(classifyDiskForSlot(makeFile(dir, "p35.po",  819200)) == DiskSlotClass::Sony35);
    assert(classifyDiskForSlot(makeFile(dir, "m35.2mg", 819200)) == DiskSlotClass::Sony35);
    // HDV: > 800K, 512-aligned.
    assert(classifyDiskForSlot(makeFile(dir, "h.hdv", 1024 * 1024)) == DiskSlotClass::Hdv);
    assert(classifyDiskForSlot(makeFile(dir, "h.2mg", 4 * 1024 * 1024)) == DiskSlotClass::Hdv);
    // Unknown: wrong extension, wrong size, or missing file.
    assert(classifyDiskForSlot(makeFile(dir, "x.txt", 143360)) == DiskSlotClass::Unknown);
    assert(classifyDiskForSlot(makeFile(dir, "odd.po", 999))   == DiskSlotClass::Unknown);
    assert(classifyDiskForSlot((dir / "does_not_exist.dsk").string()) == DiskSlotClass::Unknown);

    fs::remove_all(dir);
}

}  // namespace

int main()
{
    testPositionalDisk();
    std::printf("parseCli positional disk: OK\n");
    testKioskFlagWithDisk();
    std::printf("parseCli --kiosk + disk: OK\n");
    testKioskFlagOnly();
    std::printf("parseCli --kiosk only: OK\n");
    testPositionalComposesWithFlags();
    std::printf("parseCli positional + --preset: OK\n");
    testTwoPositionalsRejected();
    std::printf("parseCli rejects two positionals: OK\n");
    testUnknownFlagStillRejected();
    std::printf("parseCli rejects unknown flag: OK\n");
    testNoArgsCleanPlan();
    std::printf("parseCli no-args clean plan: OK\n");
    testClassifier();
    std::printf("classifyDiskForSlot 5.25/3.5/HDV/unknown: OK\n");

    std::printf("cli_kiosk OK\n");
    return 0;
}
