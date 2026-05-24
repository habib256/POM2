// ProDOS host-folder volume synthesizer smoke test.
//
// Pins:
//   * Volume header layout (block 2 offset 4..42).
//   * File entry layout (storage_type, name, file_type, key_pointer,
//     blocks_used, eof) for both seedling and sapling files.
//   * Sapling index block format (split low/high pointer halves).
//   * Round-trip data integrity: first block of a sapling file matches
//     the first 512 bytes of the source host file.
//   * Empty-folder special case (no files, valid empty volume).

#include "ProDOSVolume.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

constexpr std::size_t kBlockBytes = 512;

static void writeFile(const fs::path& path, const std::vector<std::uint8_t>& data)
{
    std::ofstream f(path, std::ios::binary);
    assert(f.good());
    if (!data.empty()) {
        f.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
    }
    assert(f.good());
}

static fs::path makeTempDir(const std::string& tag)
{
    fs::path dir = fs::temp_directory_path() / ("pom2_prodos_smoke_" + tag);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

static std::uint16_t rd16(const std::uint8_t* p) { return p[0] | (p[1] << 8); }
static std::uint32_t rd24(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0])
         | (static_cast<std::uint32_t>(p[1]) << 8)
         | (static_cast<std::uint32_t>(p[2]) << 16);
}

static void testEmptyFolder()
{
    fs::path dir = makeTempDir("empty");

    std::vector<std::uint8_t> img;
    auto br = pom2::buildVolumeFromFolder(dir.string(), "HOST", img);
    assert(br.ok);
    assert(br.filesIncluded == 0);
    assert(br.filesSkipped  == 0);
    assert(br.totalBlocks   == 7);   // 2 boot + 4 vol dir + 1 bitmap
    assert(img.size() == 7 * kBlockBytes);

    // Volume header at offset 4 of block 2.
    const std::uint8_t* b2 = img.data() + 2 * kBlockBytes;
    assert(rd16(b2 + 0) == 0);       // prev pointer
    assert(rd16(b2 + 2) == 3);       // next pointer
    assert(b2[4] == 0xF4);           // storage_type = 0xF, name_length = 4 ("HOST")
    assert(std::memcmp(b2 + 5, "HOST", 4) == 0);
    assert(b2[4 + 0x1F] == 39);      // entry_length
    assert(b2[4 + 0x20] == 13);      // entries_per_block
    assert(rd16(b2 + 4 + 0x21) == 0);// file_count
    assert(rd16(b2 + 4 + 0x23) == 6);// bit_map_pointer
    assert(rd16(b2 + 4 + 0x25) == 7);// total_blocks
}

static void testSeedlingAndSapling()
{
    fs::path dir = makeTempDir("populated");

    // 3 files: short BAS (200 B → seedling), short TXT (300 B → seedling),
    // larger BIN (5000 B → sapling, ceil(5000/512)=10 data blocks).
    std::vector<std::uint8_t> bas(200);
    for (std::size_t i = 0; i < bas.size(); ++i) bas[i] = static_cast<std::uint8_t>(i & 0xFF);
    std::vector<std::uint8_t> txt(300);
    for (std::size_t i = 0; i < txt.size(); ++i) txt[i] = static_cast<std::uint8_t>('A' + (i % 26));
    std::vector<std::uint8_t> bin(5000);
    for (std::size_t i = 0; i < bin.size(); ++i) bin[i] = static_cast<std::uint8_t>((i * 7u + 3u) & 0xFF);

    writeFile(dir / "program.bas", bas);
    writeFile(dir / "hello.txt",   txt);
    writeFile(dir / "data.bin",    bin);

    std::vector<std::uint8_t> img;
    auto br = pom2::buildVolumeFromFolder(dir.string(), "HOST", img);
    assert(br.ok);
    assert(br.filesIncluded == 3);
    assert(br.filesSkipped  == 0);

    // Layout: 7 structural + 1 (bas seedling) + 1 (txt seedling) +
    // 1 (bin sapling index) + 10 (bin data blocks) = 20 blocks.
    assert(br.totalBlocks == 20);
    assert(img.size() == 20 * kBlockBytes);

    const std::uint8_t* b2 = img.data() + 2 * kBlockBytes;

    // file_count
    assert(rd16(b2 + 4 + 0x21) == 3);
    assert(rd16(b2 + 4 + 0x25) == 20);

    // First file entry starts at block 2 offset 4 + 39 = 43.
    // Files are inserted in alphabetical order:
    //   data.bin   → entry 0 (sapling, file_type 0x06, eof 5000)
    //   hello.txt  → entry 1 (seedling, file_type 0x04, eof 300)
    //   program.bas → entry 2 (seedling, file_type 0xFC, eof 200)

    auto entryAt = [&](std::size_t idx) -> const std::uint8_t* {
        const std::size_t kEntryLen = 39;
        if (idx < 12) return b2 + 4 + kEntryLen + idx * kEntryLen;
        idx -= 12;
        const std::size_t blk = 3 + (idx / 13);
        const std::size_t slot = idx % 13;
        return img.data() + blk * kBlockBytes + 4 + slot * kEntryLen;
    };

    // ── Entry 0: DATA (.bin → BIN, sapling, 5000 bytes)
    const std::uint8_t* e0 = entryAt(0);
    const std::uint8_t st0 = e0[0] >> 4;
    const std::uint8_t nl0 = e0[0] & 0x0F;
    assert(st0 == 0x2);                       // sapling
    assert(nl0 == 4);                         // "DATA"
    assert(std::memcmp(e0 + 1, "DATA", 4) == 0);
    assert(e0[0x10] == 0x06);                 // BIN
    const std::uint16_t key0 = rd16(e0 + 0x11);
    const std::uint16_t bu0  = rd16(e0 + 0x13);
    const std::uint32_t eof0 = rd24(e0 + 0x15);
    assert(bu0 == 11);                        // 1 index + 10 data
    assert(eof0 == 5000);
    assert(rd16(e0 + 0x25) == 2);             // header_pointer = vol dir key block

    // Sapling index block: 256 LE pointers split as low half | high half.
    const std::uint8_t* idxBlk = img.data() + key0 * kBlockBytes;
    const std::uint16_t firstDataBlk = idxBlk[0] | (idxBlk[256] << 8);
    assert(firstDataBlk != 0);
    // First 512 bytes of bin should match the first data block.
    const std::uint8_t* firstData = img.data() + firstDataBlk * kBlockBytes;
    assert(std::memcmp(firstData, bin.data(), kBlockBytes) == 0);

    // 10th (last) data block: pointer at index 9.
    const std::uint16_t lastDataBlk = idxBlk[9] | (idxBlk[256 + 9] << 8);
    assert(lastDataBlk != 0);
    // bin[5000 - 1] is at index 9 * 512 + 5000 - 4608 = 392 within last block.
    const std::uint8_t* lastBlock = img.data() + lastDataBlk * kBlockBytes;
    const std::size_t tailLen = 5000 - 9 * kBlockBytes;
    assert(std::memcmp(lastBlock, bin.data() + 9 * kBlockBytes, tailLen) == 0);

    // ── Entry 1: HELLO (.txt → TXT, seedling, 300 bytes)
    const std::uint8_t* e1 = entryAt(1);
    const std::uint8_t st1 = e1[0] >> 4;
    const std::uint8_t nl1 = e1[0] & 0x0F;
    assert(st1 == 0x1);
    assert(nl1 == 5);
    assert(std::memcmp(e1 + 1, "HELLO", 5) == 0);
    assert(e1[0x10] == 0x04);                 // TXT
    const std::uint16_t key1 = rd16(e1 + 0x11);
    assert(rd16(e1 + 0x13) == 1);             // blocks_used
    assert(rd24(e1 + 0x15) == 300);           // eof
    const std::uint8_t* d1 = img.data() + key1 * kBlockBytes;
    assert(std::memcmp(d1, txt.data(), txt.size()) == 0);

    // ── Entry 2: PROGRAM (.bas → BAS, seedling, 200 bytes)
    const std::uint8_t* e2 = entryAt(2);
    const std::uint8_t st2 = e2[0] >> 4;
    const std::uint8_t nl2 = e2[0] & 0x0F;
    assert(st2 == 0x1);
    assert(nl2 == 7);
    assert(std::memcmp(e2 + 1, "PROGRAM", 7) == 0);
    assert(e2[0x10] == 0xFC);                 // BAS
    assert(rd24(e2 + 0x15) == 200);

    // Bitmap sanity: structural blocks 0..6 are USED (bits cleared); the
    // first data block (block 7) is also USED; last allocated block (19)
    // is USED; block 20+ is "out of range" — must be USED too (since we
    // initialised only bits 0..total_blocks-1 to free, and only freed within
    // total_blocks, the byte covering block 20+ stays as we set it). We
    // only assert the structural + first data + a clearly free expectation
    // depending on layout. With totalBlocks=20, all blocks 0..19 are used
    // (every block is part of either structure or file data).
    const std::uint8_t* bm = img.data() + 6 * kBlockBytes;
    auto blockFree = [bm](std::size_t b) {
        const std::size_t byteIdx = b >> 3;
        const std::size_t bitIdx  = 7 - (b & 7);
        return (bm[byteIdx] & (1u << bitIdx)) != 0;
    };
    for (std::size_t b = 0; b < 20; ++b) {
        assert(!blockFree(b));
    }

    std::printf("prodos_volume_smoke: empty + populated OK\n");
}

static void testNameSanitisationAndCollisions()
{
    fs::path dir = makeTempDir("names");

    // "HELLO WORLD.TXT" → "HELLOWORLD" (letters/digits only, ≤15 chars).
    // "1ROOT.bin" must be prefixed → "A1ROOT".
    // Two files producing the same sanitised name must get .1 / .2 suffixes.
    writeFile(dir / "hello world.txt", { 'a' });
    writeFile(dir / "1root.bin",       { 'b' });
    writeFile(dir / "Foo!.bin",        { 'c' });
    writeFile(dir / "Foo?.bin",        { 'd' });   // collision after sanitise

    std::vector<std::uint8_t> img;
    auto br = pom2::buildVolumeFromFolder(dir.string(), "HOST", img);
    assert(br.ok);
    assert(br.filesIncluded == 4);

    // Spot-check: at least one entry sanitises to "A1ROOT" (1root.bin).
    bool found_a1root = false, found_foo = false, found_foo1 = false;
    const std::uint8_t* b2 = img.data() + 2 * kBlockBytes;
    for (std::size_t i = 0; i < 12; ++i) {
        const std::uint8_t* e = b2 + 4 + 39 + i * 39;
        if ((e[0] >> 4) == 0) continue;  // empty slot
        const std::uint8_t nl = e[0] & 0x0F;
        std::string name(reinterpret_cast<const char*>(e + 1), nl);
        if (name == "A1ROOT") found_a1root = true;
        if (name == "FOO.")   found_foo    = true;     // first collision wins base name
        if (name == "FOO..1") found_foo1   = true;     // second gets .1 suffix
    }
    assert(found_a1root);
    assert(found_foo);
    assert(found_foo1);
    std::printf("prodos_volume_smoke: name sanitisation + collisions OK\n");
}

static void testRoundTripFolderToVolumeToFolder()
{
    // Source folder: 3 files with distinct types and a sapling.
    fs::path src = makeTempDir("round_src");
    std::vector<std::uint8_t> bas(120);
    for (std::size_t i = 0; i < bas.size(); ++i) bas[i] = static_cast<std::uint8_t>('a' + (i % 26));
    std::vector<std::uint8_t> txt(80);
    for (std::size_t i = 0; i < txt.size(); ++i) txt[i] = static_cast<std::uint8_t>('1' + (i % 9));
    std::vector<std::uint8_t> bin(2500);             // sapling, 5 data blocks
    for (std::size_t i = 0; i < bin.size(); ++i) bin[i] = static_cast<std::uint8_t>((i * 11u + 5u) & 0xFF);

    writeFile(src / "hello.bas",  bas);
    writeFile(src / "readme.txt", txt);
    writeFile(src / "game.bin",   bin);

    std::vector<std::uint8_t> img;
    auto br = pom2::buildVolumeFromFolder(src.string(), "HOST", img);
    assert(br.ok && br.filesIncluded == 3);

    // Simulate a guest write: flip one byte in HELLO's data block. Find
    // HELLO's seedling key_pointer via the directory; HELLO is 5 chars,
    // so we scan for storage=$1 and name "HELLO".
    const std::uint8_t* b2 = img.data() + 2 * kBlockBytes;
    std::uint16_t helloKey = 0;
    for (std::size_t i = 0; i < 12; ++i) {
        const std::uint8_t* e = b2 + 4 + 39 + i * 39;
        if ((e[0] >> 4) != 0x1) continue;
        const std::uint8_t nl = e[0] & 0x0F;
        if (nl == 5 && std::memcmp(e + 1, "HELLO", 5) == 0) {
            helloKey = rd16(e + 0x11);
            break;
        }
    }
    assert(helloKey != 0);
    img[helloKey * kBlockBytes + 7] = 0xEE;       // mutate one byte

    // Decode into a fresh folder.
    fs::path dst = makeTempDir("round_dst");
    auto dr = pom2::decodeVolumeToFolder(img, dst.string());
    assert(dr.ok);
    assert(dr.filesWritten == 3);
    assert(dr.filesSkipped == 0);

    // Check the 3 files are present with the right names and extensions.
    auto readBack = [](const fs::path& p) {
        std::ifstream f(p, std::ios::binary);
        if (!f) return std::vector<std::uint8_t>{};
        f.seekg(0, std::ios::end);
        const auto sz = static_cast<std::size_t>(f.tellg());
        f.seekg(0, std::ios::beg);
        std::vector<std::uint8_t> out(sz);
        f.read(reinterpret_cast<char*>(out.data()),
               static_cast<std::streamsize>(sz));
        return out;
    };

    auto helloOut  = readBack(dst / "HELLO.bas");
    auto readmeOut = readBack(dst / "README.txt");
    auto gameOut   = readBack(dst / "GAME.bin");
    assert(!helloOut.empty());
    assert(!readmeOut.empty());
    assert(!gameOut.empty());

    // HELLO must reflect the guest mutation at offset 7.
    assert(helloOut.size() == bas.size());
    assert(helloOut[7] == 0xEE);
    for (std::size_t i = 0; i < bas.size(); ++i) {
        if (i == 7) continue;
        assert(helloOut[i] == bas[i]);
    }

    // README + GAME must round-trip exactly.
    assert(readmeOut == txt);
    assert(gameOut   == bin);

    // No-delete safety: a file that exists in the destination but isn't
    // in the volume must NOT be removed.
    writeFile(dst / "do_not_delete.txt", { 'X', 'Y' });
    auto dr2 = pom2::decodeVolumeToFolder(img, dst.string());
    assert(dr2.ok);
    assert(fs::exists(dst / "do_not_delete.txt"));

    std::printf("prodos_volume_smoke: folder→volume→folder round-trip OK\n");
}

static void testSubdirsBuildAndDecode()
{
    // Source folder with one nested subdir:
    //   src/INTRO.txt           ("hi")
    //   src/GAMES/PACMAN.bin    256 bytes
    //   src/GAMES/SCORES.txt    ("100")
    fs::path src = makeTempDir("subdirs_src");
    fs::create_directories(src / "GAMES");
    writeFile(src / "intro.txt", { 'h', 'i' });
    std::vector<std::uint8_t> pacman(256);
    for (std::size_t i = 0; i < pacman.size(); ++i) pacman[i] = static_cast<std::uint8_t>(i);
    writeFile(src / "GAMES" / "pacman.bin", pacman);
    writeFile(src / "GAMES" / "scores.txt", { '1', '0', '0' });

    std::vector<std::uint8_t> img;
    auto br = pom2::buildVolumeFromFolder(src.string(), "HOST", img);
    assert(br.ok);
    assert(br.filesIncluded == 3);                  // 3 regular files (subdir not counted)

    // Volume directory must have a child entry with storage_type=$D for
    // GAMES. Find it in block 2.
    const std::uint8_t* b2 = img.data() + 2 * kBlockBytes;
    bool foundDir = false, foundIntro = false;
    std::uint16_t gamesKey = 0;
    std::uint8_t  gamesParentSlot = 0;
    for (std::size_t i = 0; i < 12; ++i) {
        const std::uint8_t* e = b2 + 4 + 39 + i * 39;
        const std::uint8_t st = e[0] >> 4;
        if (st == 0) continue;
        const std::uint8_t nl = e[0] & 0x0F;
        std::string nm(reinterpret_cast<const char*>(e + 1), nl);
        if (st == 0xD && nm == "GAMES") {
            foundDir = true;
            gamesKey = rd16(e + 0x11);
            gamesParentSlot = static_cast<std::uint8_t>(i + 2);  // 1-based, +1 for vol header
            assert(e[0x10] == 0x0F);                              // file_type DIR
        }
        if (st == 0x1 && nm == "INTRO") {
            foundIntro = true;
            assert(e[0x10] == 0x04);                              // TXT
        }
    }
    assert(foundDir && foundIntro);
    assert(gamesKey != 0);

    // GAMES subdir header at its first block, offset 4. Validate fields.
    const std::uint8_t* gamesBlock = img.data() + gamesKey * kBlockBytes;
    const std::uint8_t* hdr = gamesBlock + 4;
    assert((hdr[0] >> 4) == 0xE);                                 // subdir header
    assert((hdr[0] & 0x0F) == 5);                                 // "GAMES"
    assert(std::memcmp(hdr + 1, "GAMES", 5) == 0);
    assert(hdr[0x14] == 0x75);                                    // sentinel
    assert(rd16(hdr + 0x21) == 2);                                // 2 children
    assert(rd16(hdr + 0x23) == 2);                                // parent block = vol dir block 2
    assert(hdr[0x25] == gamesParentSlot);                         // 1-based parent slot

    // Two file entries in the GAMES dir block (slots 1 and 2 — slot 0 = header).
    bool foundPac = false, foundScores = false;
    for (std::size_t s = 1; s < 13; ++s) {
        const std::uint8_t* e = gamesBlock + 4 + s * 39;
        const std::uint8_t st = e[0] >> 4;
        if (st == 0) continue;
        const std::uint8_t nl = e[0] & 0x0F;
        std::string nm(reinterpret_cast<const char*>(e + 1), nl);
        if (nm == "PACMAN" && st == 0x1) {
            foundPac = true;
            assert(e[0x10] == 0x06);                              // BIN
            const std::uint16_t key = rd16(e + 0x11);
            const std::uint32_t eof = rd24(e + 0x15);
            assert(eof == 256);
            const std::uint8_t* d = img.data() + key * kBlockBytes;
            assert(std::memcmp(d, pacman.data(), 256) == 0);
            // header_pointer = first block of GAMES subdir
            assert(rd16(e + 0x25) == gamesKey);
        }
        if (nm == "SCORES" && st == 0x1) {
            foundScores = true;
            assert(e[0x10] == 0x04);                              // TXT
            assert(rd24(e + 0x15) == 3);
        }
    }
    assert(foundPac && foundScores);

    // Now decode and verify round-trip into a fresh folder.
    fs::path dst = makeTempDir("subdirs_dst");
    auto dr = pom2::decodeVolumeToFolder(img, dst.string());
    assert(dr.ok);
    assert(dr.filesWritten == 3);

    assert(fs::exists(dst / "INTRO.txt"));
    assert(fs::is_directory(dst / "GAMES"));
    assert(fs::exists(dst / "GAMES" / "PACMAN.bin"));
    assert(fs::exists(dst / "GAMES" / "SCORES.txt"));

    std::ifstream pacOut(dst / "GAMES" / "PACMAN.bin", std::ios::binary);
    std::vector<std::uint8_t> pacOutBytes((std::istreambuf_iterator<char>(pacOut)),
                                           std::istreambuf_iterator<char>());
    assert(pacOutBytes == pacman);

    std::printf("prodos_volume_smoke: subdirs build+decode OK\n");
}

int main()
{
    testEmptyFolder();
    testSeedlingAndSapling();
    testNameSanitisationAndCollisions();
    testRoundTripFolderToVolumeToFolder();
    testSubdirsBuildAndDecode();
    std::printf("prodos_volume_smoke: PASS\n");
    return 0;
}
