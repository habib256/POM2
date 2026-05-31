// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Disk35Image — Phase 1 implementation. Block storage + 2IMG envelope
// passthrough. Sony GCR encoding (the Phase 2 work) is not here yet
// — the bytes round-trip the file system but the IWM cannot clock
// them out as flux transitions until that lands.
//
// MAME line refs:
//   * 2IMG header decode mirrors `DiskImage.cpp` (POM2's existing 5.25"
//     2IMG path), itself a faithful port of the 2IMG spec used by MAME
//     `ap_dsk35.cpp` for ProDOS .2mg loads.

#include "Disk35Image.h"
#include "Logger.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace pom2 {

namespace {

constexpr int kSectorsPerTrackByZone[5] = { 12, 11, 10, 9, 8 };

bool endsWithCi(const std::string& s, const char* suffix)
{
    const std::size_t sl = std::strlen(suffix);
    if (s.size() < sl) return false;
    for (std::size_t i = 0; i < sl; ++i) {
        const char a = static_cast<char>(std::tolower(
            static_cast<unsigned char>(s[s.size() - sl + i])));
        const char b = static_cast<char>(std::tolower(
            static_cast<unsigned char>(suffix[i])));
        if (a != b) return false;
    }
    return true;
}

uint16_t rd16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
uint32_t rd32(const uint8_t* p) {
    return  static_cast<uint32_t>(p[0])        |
           (static_cast<uint32_t>(p[1]) <<  8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

int Disk35Image::sectorsForTrack(int track)
{
    // MAME `ap_dsk35.cpp` zone schedule: 5 zones of 16 tracks each.
    if (track < 0 || track >= kTracks) return 0;
    return kSectorsPerTrackByZone[track / 16];
}

bool Disk35Image::loadFile(const std::string& imgPath)
{
    eject();
    path_ = imgPath;

    std::ifstream f(imgPath, std::ios::binary | std::ios::ate);
    if (!f) {
        lastError_ = "Disk35Image: cannot open " + imgPath;
        return false;
    }
    const std::streamsize sz = f.tellg();
    if (sz <= 0) {
        lastError_ = "Disk35Image: empty file " + imgPath;
        return false;
    }
    f.seekg(0, std::ios::beg);

    std::vector<uint8_t> buf(static_cast<std::size_t>(sz));
    if (!f.read(reinterpret_cast<char*>(buf.data()), sz)) {
        lastError_ = "Disk35Image: short read on " + imgPath;
        return false;
    }
    const std::size_t n = buf.size();

    // Detect 2IMG-wrapped 800K. Same header layout as 5.25"; we just
    // require the payload size to be 819 200 bytes.
    if (n >= 64 &&
        buf[0] == '2' && buf[1] == 'I' && buf[2] == 'M' && buf[3] == 'G')
    {
        const uint16_t headerLen = rd16(buf.data() + 8);
        const uint32_t format    = rd32(buf.data() + 12);
        const uint32_t flags     = rd32(buf.data() + 16);
        const uint32_t dataOff   = rd32(buf.data() + 24);
        const uint32_t dataLen   = rd32(buf.data() + 28);

        if (headerLen < 52 || dataOff < headerLen || dataOff > n ||
            dataLen == 0 ||
            static_cast<std::size_t>(dataOff) + dataLen > n)
        {
            lastError_ = "Disk35Image: malformed 2IMG header in " + imgPath;
            return false;
        }
        // format 1 = ProDOS block-ordered. 800K disks use this.
        if (format != 1) {
            lastError_ = "Disk35Image: 2IMG format " + std::to_string(format) +
                         " not supported for 3.5\" disks (need ProDOS=1)";
            return false;
        }
        if (dataLen != kBytesPerImage) {
            lastError_ = "Disk35Image: 2IMG ProDOS payload must be " +
                         std::to_string(kBytesPerImage) + " bytes, got " +
                         std::to_string(dataLen);
            return false;
        }

        twoImgHeaderRaw_.assign(buf.begin(),
                                buf.begin() + dataOff);
        twoImgTrailerRaw_.assign(buf.begin() + dataOff + dataLen,
                                 buf.end());
        blocks_.assign(buf.begin() + dataOff,
                       buf.begin() + dataOff + dataLen);
        fileWriteProtected_ = (flags & 1u) != 0;
        kind_   = ImageKind::TwoImg800k;
        loaded_ = true;
        dirty_  = false;
        return true;
    }

    // Bare 800K payload (.po, .dsk, .image).
    if (n == kBytesPerImage) {
        // Cheap "looks like ProDOS" sniff: block 2 should be the volume
        // directory key block. Not authoritative — non-ProDOS 800K disks
        // (rare; Pascal, ProFile) load anyway, but we warn.
        const uint8_t* keyBlock = buf.data() + 2u * kBlockBytes;
        if (!(keyBlock[0] == 0 && keyBlock[1] == 0 &&
              (keyBlock[4] & 0xF0) == 0xF0))
        {
            pom2::log().warn(
                "Disk35",
                "image " + imgPath +
                " doesn't look ProDOS-formatted at block 2");
        }
        blocks_ = std::move(buf);
        // Plain `.po` ext: file is editable by default; user opts in
        // via setWriteBackEnabled. `.dsk` 800K dumps are sometimes
        // marked read-only by convention — we keep the same default.
        fileWriteProtected_ = !endsWithCi(imgPath, ".po") &&
                              !endsWithCi(imgPath, ".2mg");
        kind_   = ImageKind::Raw800k;
        loaded_ = true;
        dirty_  = false;
        return true;
    }

    lastError_ = "Disk35Image: " + imgPath +
                 " is not an 800K 3.5\" image (size " + std::to_string(n) +
                 ", expected " + std::to_string(kBytesPerImage) +
                 " or 2IMG-wrapped)";
    return false;
}

void Disk35Image::eject()
{
    loaded_              = false;
    dirty_               = false;
    fileWriteProtected_  = false;
    kind_                = ImageKind::Unknown;
    blocks_.clear();
    twoImgHeaderRaw_.clear();
    twoImgTrailerRaw_.clear();
    path_.clear();
    lastError_.clear();
}

bool Disk35Image::readBlock(uint32_t idx, uint8_t out[kBlockBytes]) const
{
    if (!loaded_ || idx >= kBlockCount) return false;
    std::memcpy(out, blocks_.data() + idx * kBlockBytes, kBlockBytes);
    return true;
}

bool Disk35Image::writeBlock(uint32_t idx, const uint8_t in[kBlockBytes])
{
    if (!loaded_ || idx >= kBlockCount) return false;
    if (isWriteProtected()) return false;
    std::memcpy(blocks_.data() + idx * kBlockBytes, in, kBlockBytes);
    dirty_ = true;
    return true;
}

bool Disk35Image::saveDirty()
{
    if (!loaded_ || !dirty_) return true;
    if (isWriteProtected()) {
        lastError_ = "Disk35Image: image is write-protected";
        return false;
    }
    std::ofstream f(path_, std::ios::binary | std::ios::trunc);
    if (!f) {
        lastError_ = "Disk35Image: cannot open " + path_ + " for write";
        return false;
    }
    if (!twoImgHeaderRaw_.empty()) {
        f.write(reinterpret_cast<const char*>(twoImgHeaderRaw_.data()),
                static_cast<std::streamsize>(twoImgHeaderRaw_.size()));
    }
    f.write(reinterpret_cast<const char*>(blocks_.data()),
            static_cast<std::streamsize>(blocks_.size()));
    if (!twoImgTrailerRaw_.empty()) {
        f.write(reinterpret_cast<const char*>(twoImgTrailerRaw_.data()),
                static_cast<std::streamsize>(twoImgTrailerRaw_.size()));
    }
    if (!f) {
        lastError_ = "Disk35Image: write failed on " + path_;
        return false;
    }
    dirty_ = false;
    return true;
}

}  // namespace pom2
