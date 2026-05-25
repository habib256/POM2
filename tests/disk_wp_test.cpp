// Disk II write-protect regression test.
//
// A physically write-protected 5.25" image (2IMG flags bit0 / WOZ INFO+2)
// must never be mutated, even with write-back enabled and even if software
// ignores the $C0nD WP-sense line and writes anyway. The controller used to
// gate writes only on the write-back TOGGLE, so a WP disk's in-memory buffer
// was corrupted and saveDirty() then overwrote the user's source file.
// DiskImage now honours the physical WP flag in writeNibbleAt/writeFlux and
// saveDirty().
//
// Self-contained: wraps an all-zero 143360-byte DOS payload in a 2IMG with
// the write-protect flag set — no disk fixture required.

#include "DiskImage.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

void le16(std::vector<uint8_t>& b, size_t o, uint16_t v) {
    b[o] = v & 0xFF; b[o + 1] = (v >> 8) & 0xFF;
}
void le32(std::vector<uint8_t>& b, size_t o, uint32_t v) {
    b[o] = v & 0xFF; b[o + 1] = (v >> 8) & 0xFF;
    b[o + 2] = (v >> 16) & 0xFF; b[o + 3] = (v >> 24) & 0xFF;
}

// 2IMG (DOS order) wrapping `payload`, with the write-protect flag set.
std::vector<uint8_t> makeWpTwoImg(const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out(64, 0);
    out[0] = '2'; out[1] = 'I'; out[2] = 'M'; out[3] = 'G';
    out[4] = 'P'; out[5] = 'O'; out[6] = 'M'; out[7] = '2';
    le16(out, 8, 64);                                       // headerLen
    le16(out, 10, 1);                                       // version
    le32(out, 12, 0);                                       // format = DOS
    le32(out, 16, 1);                                       // flags bit0 = WRITE-PROTECT
    le32(out, 24, 64);                                      // dataOffset
    le32(out, 28, static_cast<uint32_t>(payload.size()));  // dataLength
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<uint8_t> readAll(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    f.seekg(0, std::ios::end);
    const auto sz = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> v(sz);
    if (sz) f.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(sz));
    return v;
}

} // namespace

int main()
{
    const std::vector<uint8_t> payload(143360, 0x00);   // valid 5.25" size, zeros
    const std::vector<uint8_t> file = makeWpTwoImg(payload);

    const std::string path = "pom2_disk_wp.2mg";
    { std::ofstream f(path, std::ios::binary | std::ios::trunc);
      f.write(reinterpret_cast<const char*>(file.data()),
              static_cast<std::streamsize>(file.size())); }

    DiskImage img;
    img.setWriteBackEnabled(true);                       // write-back ON…
    assert(img.loadFile(path));
    assert(img.isFileWriteProtected());                 // …but medium is WP.

    // A nibble write must be a no-op: buffer unchanged, nothing marked dirty.
    const uint8_t before = img.nibbleAt(17, 100);
    img.writeNibbleAt(17, 100, static_cast<uint8_t>(before ^ 0xFF));
    assert(img.nibbleAt(17, 100) == before && "WP disk nibble must not change");
    assert(!img.hasUnsavedChanges() && "WP write must not dirty the image");

    // saveDirty() must not touch the on-disk file.
    assert(img.saveDirty());
    const std::vector<uint8_t> after = readAll(path);
    std::remove(path.c_str());
    assert(after.size() == file.size());
    assert(std::memcmp(after.data(), file.data(), file.size()) == 0 &&
           "WP disk source file must be byte-for-byte unchanged");

    std::printf("OK disk_wp (WP medium not mutated in RAM or on disk)\n");
    return 0;
}
