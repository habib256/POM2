// Pins the "refuse + clear error" behaviour of DiskImage::loadFile for
// inputs that don't match any supported format.
//
// Pre-refactor (before commit 22d5e9b), an unrecognised file silently
// fell through to the DOS 3.3 path and either returned false with a
// generic "expected 143360 bytes" or, worse, attempted to nibblize
// garbage. The new dispatcher refuses up front via detectFormat and
// populates getLastError() with a specific, user-actionable message.
//
// Each case below verifies that:
//   - loadFile returns false
//   - getLastError() is non-empty
//   - getLastError() contains a substring that identifies the failure
//     mode, so a future UI can surface it directly.

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

bool writeTemp(const std::string& path, const std::vector<uint8_t>& data)
{
    std::ofstream wf(path, std::ios::binary | std::ios::trunc);
    if (!wf) return false;
    wf.write(reinterpret_cast<const char*>(data.data()),
             static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(wf);
}

bool refuses(const std::string& path,
             const char* expectedSubstring,
             const char* caseLabel)
{
    DiskImage img;
    const bool ok = img.loadFile(path);
    std::remove(path.c_str());
    if (ok) {
        std::fprintf(stderr,
            "%s: loadFile accepted a malformed image (should have refused)\n",
            caseLabel);
        return false;
    }
    if (img.getLastError().empty()) {
        std::fprintf(stderr,
            "%s: loadFile refused but lastError is empty\n", caseLabel);
        return false;
    }
    if (img.getLastError().find(expectedSubstring) == std::string::npos) {
        std::fprintf(stderr,
            "%s: lastError missing expected substring '%s'; got: %s\n",
            caseLabel, expectedSubstring, img.getLastError().c_str());
        return false;
    }
    return true;
}

// 1. Wrong size — 100 000-byte random blob has no plausible
//    interpretation. Pre-refactor: silently tried as DOS. Post: refuses
//    with size-mismatch message.
bool caseWrongSize()
{
    std::vector<uint8_t> blob(100000, 0xA5);
    const std::string path = "refuse_wrong_size.bin";
    if (!writeTemp(path, blob)) return false;
    return refuses(path, "doesn't match any supported format",
                   "wrong-size");
}

// 2. 2IMG envelope but with dataOffset pointing past file end.
bool case2ImgBadOffset()
{
    std::vector<uint8_t> buf(64 + 100, 0);
    buf[0] = '2'; buf[1] = 'I'; buf[2] = 'M'; buf[3] = 'G';
    // headerLen = 64 (offset 8/9)
    buf[8] = 64;
    // version = 1
    buf[10] = 1;
    // format = 0 (DOS)
    // dataOffset = 0xFFFFFF (way past end) — should refuse
    buf[24] = 0xFF; buf[25] = 0xFF; buf[26] = 0xFF; buf[27] = 0x00;
    // dataLength = 143360
    buf[28] = 0x00; buf[29] = 0x60; buf[30] = 0x02; buf[31] = 0x00;
    const std::string path = "refuse_2img_bad_offset.2mg";
    if (!writeTemp(path, buf)) return false;
    return refuses(path, "header points outside the file",
                   "2IMG-bad-offset");
}

// 3. 2IMG envelope with DOS format but wrong payload size (not 143360).
bool case2ImgWrongDosSize()
{
    std::vector<uint8_t> buf(64 + 100, 0);
    buf[0] = '2'; buf[1] = 'I'; buf[2] = 'M'; buf[3] = 'G';
    buf[8] = 64; buf[10] = 1;
    // format = 0 (DOS), dataOffset = 64, dataLength = 100 (bogus)
    buf[24] = 64;
    buf[28] = 100;
    const std::string path = "refuse_2img_wrong_dos_size.2mg";
    if (!writeTemp(path, buf)) return false;
    return refuses(path,
        "DOS payload must be 143360", "2IMG-wrong-DOS-size");
}

// 4. Empty file.
bool caseEmpty()
{
    std::vector<uint8_t> empty;
    const std::string path = "refuse_empty.bin";
    if (!writeTemp(path, empty)) return false;
    return refuses(path, "doesn't match any supported format",
                   "empty");
}

// 5. WOZ magic prefix but truncated — total len < 8 means we skip the
//    WOZ branch and fall through to size mismatch.
bool caseShortWoz()
{
    std::vector<uint8_t> buf = {'W', 'O', 'Z', '2'};  // 4 bytes, no sentinel
    const std::string path = "refuse_short_woz.woz";
    if (!writeTemp(path, buf)) return false;
    return refuses(path, "doesn't match any supported format",
                   "short-WOZ");
}

}  // namespace

int main()
{
    bool ok = true;
    ok &= caseWrongSize();
    ok &= case2ImgBadOffset();
    ok &= case2ImgWrongDosSize();
    ok &= caseEmpty();
    ok &= caseShortWoz();
    if (!ok) return 1;
    std::printf("disk_refuse_smoke OK: 5 refusal cases pinned\n");
    return 0;
}
