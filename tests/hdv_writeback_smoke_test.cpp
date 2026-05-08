// HDV write-back smoke test. Pins:
//   - .hdv plain round-trip: write a block, eject (auto-save), reopen,
//     bytes match.
//   - .2mg header preservation: 64-byte 2IMG header + data + trailing
//     creator/comment chunk all survive a save bit-for-bit; only the
//     data window is updated.
//   - 2MG write-protected flag (offset 16 bit 0) is honoured even when
//     the user opts in via setWriteBackEnabled(true).
//   - Default write-back-OFF: writes go nowhere (image stays clean,
//     hasUnsavedChanges() stays false), file untouched on eject.

#include "ProDOSHardDiskCard.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr size_t kBlock = ProDOSHardDiskCard::kBlockBytes;

fs::path tmpFile(const std::string& tag, const char* ext)
{
    return fs::temp_directory_path() /
           ("pom2_hdv_wb_" + tag + ext);
}

std::vector<uint8_t> readAll(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    const size_t sz = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> out(sz);
    f.read(reinterpret_cast<char*>(out.data()),
           static_cast<std::streamsize>(sz));
    return out;
}

void writeFile(const fs::path& p, const std::vector<uint8_t>& bytes)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    assert(f.good());
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    assert(f.good());
}

// Push 512 pattern bytes into block `block` of the card via the streaming
// data port at $C0D2 (low4=0x2). Caller is responsible for setting up the
// block address registers ($C0D0/$C0D1) and write-back opt-in.
void cardWriteBlock(ProDOSHardDiskCard& card, uint16_t block,
                    const uint8_t* pattern)
{
    card.deviceSelectWrite(0x0, static_cast<uint8_t>(block & 0xFF));
    card.deviceSelectWrite(0x1, static_cast<uint8_t>((block >> 8) & 0xFF));
    for (size_t i = 0; i < kBlock; ++i) {
        card.deviceSelectWrite(0x2, pattern[i]);
    }
}

}  // namespace

int main()
{
    // ── Case A: .hdv plain round-trip ───────────────────────────────────
    {
        const fs::path p = tmpFile("a", ".hdv");
        std::vector<uint8_t> file(4 * kBlock, 0);
        writeFile(p, file);

        ProDOSHardDiskCard card;
        assert(card.loadImage(p.string()));
        assert(card.getBlockCount() == 4);
        assert(card.canWriteBack());                 // not WP
        assert(card.isWriteProtected());             // user hasn't opted in
        card.setWriteBackEnabled(true);
        assert(!card.isWriteProtected());

        uint8_t pattern[kBlock];
        for (size_t i = 0; i < kBlock; ++i) {
            pattern[i] = static_cast<uint8_t>((i * 13u + 17u) & 0xFFu);
        }
        cardWriteBlock(card, 1, pattern);
        assert(card.hasUnsavedChanges());
        card.ejectImage();                           // saves on eject

        const auto have = readAll(p);
        assert(have.size() == file.size());
        // Block 0 untouched.
        for (size_t i = 0; i < kBlock; ++i) assert(have[i] == 0);
        // Block 1 = pattern.
        for (size_t i = 0; i < kBlock; ++i) assert(have[kBlock + i] == pattern[i]);
        // Blocks 2-3 untouched.
        for (size_t i = 2 * kBlock; i < 4 * kBlock; ++i) assert(have[i] == 0);

        fs::remove(p);
        std::printf("hdv_writeback: .hdv round-trip OK\n");
    }

    // ── Case B: .2mg header AND trailing chunk preservation ─────────────
    {
        const fs::path p = tmpFile("b", ".2mg");
        constexpr size_t kData = 2 * kBlock;
        constexpr size_t kTail = 16;
        std::vector<uint8_t> file(64 + kData + kTail, 0);

        // 2IMG header.
        file[0] = '2'; file[1] = 'I'; file[2] = 'M'; file[3] = 'G';
        file[4] = 'P'; file[5] = 'O'; file[6] = 'M'; file[7] = '2';   // creator
        file[8] = 64;                          // header_len = 64
        file[12] = 1;                          // format = ProDOS
        // flags = 0 (writable)
        file[24] = 64;                         // data_offset
        const uint32_t dlen = static_cast<uint32_t>(kData);
        file[28] = static_cast<uint8_t>(dlen & 0xFF);
        file[29] = static_cast<uint8_t>((dlen >> 8) & 0xFF);
        // Trailing "comment" — every byte distinct so a misalignment shows.
        for (size_t i = 0; i < kTail; ++i) {
            file[64 + kData + i] = static_cast<uint8_t>(0xC0u + i);
        }
        // Data blocks pre-filled with a different pattern so we can spot
        // either the unmodified blocks staying or block 0 being overwritten.
        for (size_t i = 0; i < kData; ++i) {
            file[64 + i] = static_cast<uint8_t>((i * 5u + 1u) & 0xFFu);
        }
        writeFile(p, file);

        // Snapshot what must NOT change.
        std::vector<uint8_t> originalHeader(file.begin(), file.begin() + 64);
        std::vector<uint8_t> originalTail(file.begin() + 64 + kData,
                                          file.begin() + 64 + kData + kTail);
        std::vector<uint8_t> originalBlock1(file.begin() + 64 + kBlock,
                                            file.begin() + 64 + kBlock * 2);

        ProDOSHardDiskCard card;
        assert(card.loadImage(p.string()));
        assert(card.canWriteBack());
        card.setWriteBackEnabled(true);

        uint8_t pattern[kBlock];
        for (size_t i = 0; i < kBlock; ++i) {
            pattern[i] = static_cast<uint8_t>((i * 23u + 7u) & 0xFFu);
        }
        cardWriteBlock(card, 0, pattern);
        assert(card.hasUnsavedChanges());
        card.ejectImage();

        const auto have = readAll(p);
        assert(have.size() == file.size());
        // Header survives bit-for-bit.
        for (size_t i = 0; i < 64; ++i) assert(have[i] == originalHeader[i]);
        // Block 0 was modified.
        for (size_t i = 0; i < kBlock; ++i) assert(have[64 + i] == pattern[i]);
        // Block 1 untouched.
        for (size_t i = 0; i < kBlock; ++i) {
            assert(have[64 + kBlock + i] == originalBlock1[i]);
        }
        // Trailing comment chunk survives bit-for-bit.
        for (size_t i = 0; i < kTail; ++i) {
            assert(have[64 + kData + i] == originalTail[i]);
        }

        fs::remove(p);
        std::printf("hdv_writeback: .2mg header+tail preservation OK\n");
    }

    // ── Case C: 2MG WP flag honoured even when user opts in ─────────────
    {
        const fs::path p = tmpFile("c", ".2mg");
        std::vector<uint8_t> file(64 + 2 * kBlock, 0);
        file[0] = '2'; file[1] = 'I'; file[2] = 'M'; file[3] = 'G';
        file[8] = 64;
        file[12] = 1;                         // format = ProDOS
        file[16] = 1;                         // flags bit 0 = write-protected
        file[24] = 64;
        const uint32_t dlen = 2 * static_cast<uint32_t>(kBlock);
        file[28] = static_cast<uint8_t>(dlen & 0xFF);
        file[29] = static_cast<uint8_t>((dlen >> 8) & 0xFF);
        writeFile(p, file);

        const auto original = readAll(p);

        ProDOSHardDiskCard card;
        assert(card.loadImage(p.string()));
        assert(!card.canWriteBack());                 // WP overrides everything
        card.setWriteBackEnabled(true);
        assert(card.isWriteProtected());              // still WP
        assert(!card.canWriteBack());

        // Try to write — silently dropped because WP gate fires first.
        uint8_t pattern[kBlock];
        std::memset(pattern, 0xCC, sizeof(pattern));
        cardWriteBlock(card, 0, pattern);
        assert(!card.hasUnsavedChanges());
        card.ejectImage();

        const auto after = readAll(p);
        assert(after == original);                    // file untouched

        fs::remove(p);
        std::printf("hdv_writeback: 2MG WP flag honoured OK\n");
    }

    // ── Case D: default write-back OFF — writes silently dropped ────────
    {
        const fs::path p = tmpFile("d", ".hdv");
        std::vector<uint8_t> file(2 * kBlock, 0);
        for (size_t i = 0; i < file.size(); ++i) {
            file[i] = static_cast<uint8_t>(i & 0xFF);
        }
        writeFile(p, file);
        const auto original = readAll(p);

        ProDOSHardDiskCard card;
        assert(card.loadImage(p.string()));
        assert(card.canWriteBack());
        // Don't call setWriteBackEnabled(true).
        assert(card.isWriteProtected());

        uint8_t pattern[kBlock];
        std::memset(pattern, 0xAA, sizeof(pattern));
        cardWriteBlock(card, 0, pattern);
        assert(!card.hasUnsavedChanges());
        card.ejectImage();

        const auto after = readAll(p);
        assert(after == original);

        fs::remove(p);
        std::printf("hdv_writeback: opt-in default OFF OK\n");
    }

    std::printf("hdv_writeback_smoke OK\n");
    return 0;
}
