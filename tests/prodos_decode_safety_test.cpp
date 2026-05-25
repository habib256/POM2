// ProDOS synth write-back path-traversal regression test.
//
// decodeVolumeToFolder walks a volume image's directory entries and writes
// each file back into a host folder. The image is GUEST-WRITABLE RAM, so a
// directory-entry name is untrusted: a crafted name like "../PWNED" was
// joined to the host folder unsanitized and escaped the jail when write-back
// was enabled. The decoder now rejects any name that isn't a safe single
// host component (isHostSafeProDOSName).
//
// Part 1 unit-tests the validator. Part 2 crafts a minimal ProDOS volume
// whose directory holds a malicious "../PWNED" file plus a benign "GOOD"
// file, decodes it into base/jail/inner, and asserts the escape file is NOT
// created in base/jail while the benign file IS written.

#include "ProDOSVolume.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr size_t kBlk = 512;

// Write a directory entry into image[block][slot].
void putEntry(std::vector<std::uint8_t>& img, size_t block, size_t slot,
              std::uint8_t storage, const std::string& name,
              std::uint8_t fileType, std::uint16_t keyPtr, std::uint32_t eof)
{
    std::uint8_t* e = img.data() + block * kBlk + 4 + slot * 39;
    e[0x00] = static_cast<std::uint8_t>((storage << 4) | (name.size() & 0x0F));
    for (size_t i = 0; i < name.size() && i < 15; ++i)
        e[1 + i] = static_cast<std::uint8_t>(name[i]);
    e[0x10] = fileType;
    e[0x11] = keyPtr & 0xFF;
    e[0x12] = (keyPtr >> 8) & 0xFF;
    e[0x15] = eof & 0xFF;
    e[0x16] = (eof >> 8) & 0xFF;
    e[0x17] = (eof >> 16) & 0xFF;
}

} // namespace

int main()
{
    // ── Part 1: the validator ────────────────────────────────────────────
    using pom2::isHostSafeProDOSName;
    assert(isHostSafeProDOSName("GOOD"));
    assert(isHostSafeProDOSName("A"));
    assert(isHostSafeProDOSName("NOTES.DATA"));
    assert(isHostSafeProDOSName("HELLO.BIN"));
    assert(!isHostSafeProDOSName(""));
    assert(!isHostSafeProDOSName("."));
    assert(!isHostSafeProDOSName(".."));
    assert(!isHostSafeProDOSName("../X"));
    assert(!isHostSafeProDOSName("a/b"));
    assert(!isHostSafeProDOSName("a\\b"));
    assert(!isHostSafeProDOSName(std::string("a\0b", 3)));  // embedded NUL
    assert(!isHostSafeProDOSName("has space"));
    assert(!isHostSafeProDOSName("0123456789ABCDEF"));       // 16 chars > 15

    // ── Part 2: end-to-end decode escape attempt ─────────────────────────
    const fs::path base = fs::temp_directory_path() / "pom2_decode_safety";
    fs::remove_all(base);
    const fs::path jail  = base / "jail";
    const fs::path inner = jail / "inner";          // hostFolder for the decode
    const fs::path escapeTarget = jail / "PWNED";   // where "../PWNED" would land
    const fs::path benign       = inner / "GOOD";

    std::vector<std::uint8_t> img(8 * kBlk, 0);     // 8 blocks (≥ kFirstDataBlock)

    // Block 2 = volume directory key block. Slot 0 = volume header (storage
    // 0xF → skipped). Next-block pointer (offset 2) = 0 → stop after block 2.
    putEntry(img, 2, 0, /*storage=*/0xF, "VOL", 0x0F, 0, 0);
    // Slot 1: malicious seedling file "../PWNED" → data in block 6.
    putEntry(img, 2, 1, /*seedling=*/0x1, "../PWNED", 0x06, /*key=*/6, /*eof=*/4);
    // Slot 2: benign seedling file "GOOD" → data in block 5. file_type 0x00
    // (typeless) so the decoded host name is verbatim "GOOD" (no extension).
    putEntry(img, 2, 2, /*seedling=*/0x1, "GOOD", 0x00, /*key=*/5, /*eof=*/4);

    std::memcpy(img.data() + 6 * kBlk, "PWND", 4);
    std::memcpy(img.data() + 5 * kBlk, "DATA", 4);

    pom2::ProDOSDecodeResult r = pom2::decodeVolumeToFolder(img, inner.string());
    assert(r.ok);

    // The traversal must have been blocked: no file outside the jail folder.
    assert(!fs::exists(escapeTarget) && "‘../PWNED’ escaped the host folder!");
    // …and the benign file must still decode normally.
    assert(fs::exists(benign) && "benign file should still be written");
    assert(r.filesWritten == 1);
    assert(r.filesSkipped >= 1);

    fs::remove_all(base);
    std::printf("OK prodos_decode_safety (validator + traversal blocked, benign OK)\n");
    return 0;
}
