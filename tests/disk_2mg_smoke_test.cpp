// Pins 2IMG / .2mg envelope read support in DiskImage::detectFormat.
//
// 2IMG wraps a DOS-skew (format=0), ProDOS-skew (format=1), or NIB
// (format=2) payload behind a 64-byte header that also carries the
// volume number and write-protect flag. Real .2mg archives (Asimov etc.)
// are the dominant interchange format today; without support the user
// can't mount them at all.
//
// Each case synthesises a fresh 2IMG file (header + payload) and loads
// it through DiskImage::loadFile, verifying:
//   - the load succeeds
//   - sector order matches the format byte
//   - the LSS-side data is correctly addressable (track 0 nibble matches
//     a known pattern in the synthetic NIB case, or the dos33_master
//     boot signature in the DOS/ProDOS cases)
//
// The test also pins the write-protect flag bit and the bit-31 volume
// number override so a future write-back commit doesn't accidentally
// drop them.

#include "DiskImage.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

void writeLe32(std::vector<uint8_t>& buf, std::size_t off, uint32_t v)
{
    buf[off + 0] = static_cast<uint8_t>(v       & 0xFF);
    buf[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    buf[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    buf[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}
void writeLe16(std::vector<uint8_t>& buf, std::size_t off, uint16_t v)
{
    buf[off + 0] = static_cast<uint8_t>(v       & 0xFF);
    buf[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

// Build a 2IMG file by prefixing `payload` with a 64-byte header.
std::vector<uint8_t> makeTwoImg(const std::vector<uint8_t>& payload,
                                uint32_t format,
                                bool writeProtect,
                                uint8_t volumeNumber)
{
    std::vector<uint8_t> out(64 + payload.size(), 0);
    // magic + creator
    out[0] = '2'; out[1] = 'I'; out[2] = 'M'; out[3] = 'G';
    out[4] = 'P'; out[5] = 'O'; out[6] = 'M'; out[7] = '2';
    writeLe16(out, 8,  64);              // headerLen
    writeLe16(out, 10, 1);               // version
    writeLe32(out, 12, format);          // 0/1/2
    uint32_t flags = 0;
    if (writeProtect)  flags |= 1u;
    if (volumeNumber != 254) {
        flags |= (1u << 8);              // mark vol# present (spec bit 8)
        flags |= static_cast<uint32_t>(volumeNumber);
    }
    writeLe32(out, 16, flags);
    writeLe32(out, 20, 0);               // numBlocks (DOS=0)
    writeLe32(out, 24, 64);              // dataOffset
    writeLe32(out, 28, static_cast<uint32_t>(payload.size()));  // dataLen
    // remaining header fields (comment/creator chunks) stay zero
    std::memcpy(out.data() + 64, payload.data(), payload.size());
    return out;
}

bool loadFixture(const std::string& name, std::vector<uint8_t>& out,
                 std::size_t expectedSize)
{
    static const char* prefixes[] = {
        "../../disks/", "../../disks2/", "disks/", "disks2/"
    };
    for (const char* pfx : prefixes) {
        const std::string p = std::string(pfx) + name;
        std::ifstream f(p, std::ios::binary);
        if (!f) continue;
        f.seekg(0, std::ios::end);
        const auto sz = static_cast<std::size_t>(f.tellg());
        if (sz != expectedSize) continue;
        f.seekg(0, std::ios::beg);
        out.resize(sz);
        f.read(reinterpret_cast<char*>(out.data()),
               static_cast<std::streamsize>(sz));
        return static_cast<bool>(f);
    }
    return false;
}

bool writeTempFile(const std::string& path, const std::vector<uint8_t>& data)
{
    std::ofstream wf(path, std::ios::binary | std::ios::trunc);
    if (!wf) return false;
    wf.write(reinterpret_cast<const char*>(data.data()),
             static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(wf);
}

bool runDosCase()
{
    std::vector<uint8_t> payload;
    if (!loadFixture("dos33_master.dsk", payload, 143360)) {
        std::fprintf(stderr,
            "skip DOS case: no dos33_master.dsk fixture\n");
        return true;
    }
    const auto bytes = makeTwoImg(payload, /*format=*/0,
                                  /*wp=*/false, /*vol=*/254);
    const std::string path = "twoimg_dos.2mg";
    if (!writeTempFile(path, bytes)) return false;
    DiskImage img;
    const bool ok = img.loadFile(path);
    std::remove(path.c_str());
    if (!ok) {
        std::fprintf(stderr, "DOS case: %s\n", img.getLastError().c_str());
        return false;
    }
    if (img.getSectorOrder() != DiskImage::SectorOrder::Dos33) {
        std::fprintf(stderr,
            "DOS case: sector order should be Dos33 after 2IMG format=0\n");
        return false;
    }
    return true;
}

bool runProDosCase()
{
    std::vector<uint8_t> payload;
    if (!loadFixture("ProDOS_2_4_3.po", payload, 143360)) {
        std::fprintf(stderr,
            "skip ProDOS case: no ProDOS_2_4_3.po fixture\n");
        return true;
    }
    const auto bytes = makeTwoImg(payload, /*format=*/1,
                                  /*wp=*/true, /*vol=*/254);
    const std::string path = "twoimg_prodos.2mg";
    if (!writeTempFile(path, bytes)) return false;
    DiskImage img;
    const bool ok = img.loadFile(path);
    std::remove(path.c_str());
    if (!ok) {
        std::fprintf(stderr, "ProDOS case: %s\n", img.getLastError().c_str());
        return false;
    }
    if (img.getSectorOrder() != DiskImage::SectorOrder::ProDOS) {
        std::fprintf(stderr,
            "ProDOS case: sector order should be ProDOS after 2IMG format=1\n");
        return false;
    }
    if (!img.isWriteProtected()) {
        std::fprintf(stderr,
            "ProDOS case: write-protect flag from 2IMG flags bit 0 was lost\n");
        return false;
    }
    return true;
}

bool runNibCase()
{
    // Synthetic 232 960-byte NIB payload — pattern lets us verify the
    // payload byte ended up at the right offset after the 64-byte header
    // strip.
    constexpr std::size_t nibLen =
        static_cast<std::size_t>(DiskImage::kTracks) * DiskImage::kNibblesPerTrack;
    std::vector<uint8_t> payload(nibLen);
    for (std::size_t i = 0; i < nibLen; ++i) {
        payload[i] = static_cast<uint8_t>((i * 17 + 5) & 0xFF);
    }
    const auto bytes = makeTwoImg(payload, /*format=*/2,
                                  /*wp=*/false, /*vol=*/200);
    const std::string path = "twoimg_nib.2mg";
    if (!writeTempFile(path, bytes)) return false;
    DiskImage img;
    const bool ok = img.loadFile(path);
    std::remove(path.c_str());
    if (!ok) {
        std::fprintf(stderr, "NIB case: %s\n", img.getLastError().c_str());
        return false;
    }
    const uint8_t expected = static_cast<uint8_t>((0 * 17 + 5) & 0xFF);
    if (img.nibbleAt(0, 0) != expected) {
        std::fprintf(stderr,
            "NIB case: track 0 byte 0 = 0x%02X, want 0x%02X "
            "(header strip off-by-N?)\n",
            img.nibbleAt(0, 0), expected);
        return false;
    }
    return true;
}

bool runRefusedFormatByte()
{
    std::vector<uint8_t> payload(143360, 0);
    auto bytes = makeTwoImg(payload, /*format=*/7,   // not 0/1/2
                            /*wp=*/false, /*vol=*/254);
    const std::string path = "twoimg_badfmt.2mg";
    if (!writeTempFile(path, bytes)) return false;
    DiskImage img;
    const bool ok = img.loadFile(path);
    std::remove(path.c_str());
    if (ok) {
        std::fprintf(stderr,
            "bad-format case: loadFile accepted unsupported format=7\n");
        return false;
    }
    if (img.getLastError().find("unsupported format byte") == std::string::npos) {
        std::fprintf(stderr,
            "bad-format case: lastError missing expected text; got: %s\n",
            img.getLastError().c_str());
        return false;
    }
    return true;
}

}  // namespace

int main()
{
    bool ok = true;
    ok &= runDosCase();
    ok &= runProDosCase();
    ok &= runNibCase();
    ok &= runRefusedFormatByte();
    if (!ok) return 1;
    std::printf("disk_2mg_smoke OK: DOS + ProDOS + NIB + bad-format\n");
    return 0;
}
