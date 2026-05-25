// Character-ROM loader hardening. Memory::loadCharRom only has a
// normalization path for 2K (II/II+) and 4K (IIe) dumps; any other size
// used to be stored raw and rendered as garbage glyphs. This pins the
// size gate (reject everything that isn't 2K/4K, including an 8K dump)
// and the happy path (2K and 4K accepted, characterRom sized correctly).

#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;

void writeFile(const fs::path& p, size_t bytes)
{
    std::FILE* f = std::fopen(p.string().c_str(), "wb");
    assert(f);
    const std::vector<uint8_t> buf(bytes, 0x55);
    if (bytes) std::fwrite(buf.data(), 1, bytes, f);
    std::fclose(f);
}

} // namespace

int main()
{
    const fs::path dir = fs::temp_directory_path();
    const fs::path p2k  = dir / "pom2_charrom_2k.bin";
    const fs::path p4k  = dir / "pom2_charrom_4k.bin";
    const fs::path p8k  = dir / "pom2_charrom_8k.bin";
    const fs::path podd = dir / "pom2_charrom_odd.bin";
    const fs::path pnil = dir / "pom2_charrom_empty.bin";

    writeFile(p2k, 2048);
    writeFile(p4k, 4096);
    writeFile(p8k, 8192);
    writeFile(podd, 1000);
    writeFile(pnil, 0);

    // Valid 2K dump → accepted, normalized to 2048 bytes.
    {
        Memory mem;
        assert(mem.loadCharRom(p2k.string().c_str()) == 1);
        assert(mem.charRom().size() == 2048);
    }

    // Valid 4K dump → accepted, kept at 4096 bytes.
    {
        Memory mem;
        assert(mem.loadCharRom(p4k.string().c_str()) == 1);
        assert(mem.charRom().size() == 4096);
    }

    // 8K dump → rejected (no normalization path; would render garbage).
    {
        Memory mem;
        assert(mem.loadCharRom(p8k.string().c_str()) == 0);
        assert(mem.getLastError().find("2K or 4K") != std::string::npos);
        assert(mem.charRom().empty());          // nothing half-loaded
    }

    // Odd size and empty file → rejected by the same gate.
    {
        Memory mem;
        assert(mem.loadCharRom(podd.string().c_str()) == 0);
        assert(mem.charRom().empty());
    }
    {
        Memory mem;
        assert(mem.loadCharRom(pnil.string().c_str()) == 0);
        assert(mem.charRom().empty());
    }

    // Missing file → distinct "cannot open" error, no glyphs loaded.
    {
        Memory mem;
        assert(mem.loadCharRom((dir / "pom2_charrom_nope.bin").string().c_str()) == 0);
        assert(mem.getLastError().find("Cannot open") != std::string::npos);
        assert(mem.charRom().empty());
    }

    for (const auto& p : {p2k, p4k, p8k, podd, pnil}) fs::remove(p);
    std::printf("char_rom: OK (2K/4K accepted, 8K/odd/empty/missing rejected)\n");
    return 0;
}
