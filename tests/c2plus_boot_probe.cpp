// Diagnostic probe: boot Copy II Plus v8.3.do and report how far it gets.
// Not a CTest target — run by hand via build/tests/c2plus_boot_probe.

#include "DiskIICard.h"
#include "M6502.h"
#include "Memory.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace fs = std::filesystem;

namespace {
bool fileExists(const std::string& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}
std::string findFirst(std::initializer_list<const char*> candidates) {
    for (const char* c : candidates) if (fileExists(c)) return c;
    return {};
}
bool copyFile(const std::string& src, const fs::path& dst) {
    std::ifstream in(src, std::ios::binary);
    if (!in) return false;
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << in.rdbuf();
    return static_cast<bool>(out);
}
std::string scrapeTextPage(const uint8_t* ram) {
    std::string out;
    for (int row = 0; row < 24; ++row) {
        const int base = 0x0400 + 0x80 * (row % 8) + 0x28 * (row / 8);
        for (int col = 0; col < 40; ++col) {
            const char c = static_cast<char>(ram[base + col] & 0x7F);
            out.push_back((c >= 0x20 && c < 0x7F) ? c : ' ');
        }
        out.push_back('\n');
    }
    return out;
}
}

int main(int argc, char** argv) {
    const std::string romPath  = findFirst({"../roms/apple2.rom", "roms/apple2.rom"});
    const std::string promPath = findFirst({"../roms/disk2.rom", "roms/disk2.rom"});
    const std::string imgPath  = (argc > 1) ? argv[1] : findFirst({
        "../disks/Copy II Plus v8.3.do", "disks/Copy II Plus v8.3.do"});

    if (romPath.empty() || promPath.empty() || imgPath.empty()) {
        std::fprintf(stderr, "missing files\n"); return 1;
    }

    fs::path scratch = fs::temp_directory_path() / "pom2_c2plus_boot.do";
    if (!copyFile(imgPath, scratch)) { std::fprintf(stderr, "copy failed\n"); return 1; }

    Memory mem;
    if (!mem.loadAppleIIRom(romPath.c_str())) { std::fprintf(stderr, "rom\n"); return 1; }

    auto card = std::make_unique<DiskIICard>();
    card->loadBootRom(promPath);
    card->insertDisk(scratch.string());
    DiskIICard* cardRaw = card.get();
    mem.slotBus().plug(6, std::move(card));

    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.slotBus().reset();
    cpu.hardReset();
    cpu.setProgramCounter(0xC600);

    const uint8_t* ram = mem.data();
    auto runUntilStable = [&](int cycles) {
        int t = 0;
        while (t < cycles) t += cpu.run(8192);
    };

    runUntilStable(120'000'000);
    std::printf("---- after boot ----\n%s----\n", scrapeTextPage(ram).c_str());

    // ESC to skip the date prompt.
    {
        const char k[] = { 0x1B };
        mem.pasteRawKeys(k, 1);
    }
    runUntilStable(40'000'000);
    std::printf("---- after ESC ----\n%s----\n", scrapeTextPage(ram).c_str());
    std::printf("disk state: ht=%d trk=%d motor=%d writeFlushes=%llu\n",
                cardRaw->getHalfTrack(), cardRaw->getCurrentTrack(),
                cardRaw->isMotorOn() ? 1 : 0,
                static_cast<unsigned long long>(cardRaw->getWriteFlushCount()));
    return 0;
}
