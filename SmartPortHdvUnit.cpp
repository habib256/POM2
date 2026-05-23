// POM2 Apple II Emulator
// Copyright (C) 2026

#include "SmartPortHdvUnit.h"
#include "Logger.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace pom2 {

SmartPortHdvUnit::SmartPortHdvUnit() = default;

SmartPortHdvUnit::~SmartPortHdvUnit()
{
    // Best-effort write-back on destruction (e.g. card unplugged).
    saveDirty();
}

bool SmartPortHdvUnit::readBlock(uint32_t idx, uint8_t* out) const
{
    if (!loaded_ || !out) return false;
    const std::size_t off = static_cast<std::size_t>(idx) * kBlockBytes;
    if (off + kBlockBytes > image_.size()) return false;
    std::memcpy(out, image_.data() + off, kBlockBytes);
    return true;
}

bool SmartPortHdvUnit::writeBlock(uint32_t idx, const uint8_t* in)
{
    if (!loaded_ || !in) return false;
    // Only the real medium WP flag blocks the write. write-back-off still
    // accepts the write into RAM (so the session is read/write); it just
    // won't be flushed to the host file by saveDirty(). Mirrors
    // ProDOSHardDiskCard::writeDataByte.
    if (writeProtectedHeader_) return false;
    const std::size_t off = static_cast<std::size_t>(idx) * kBlockBytes;
    if (off + kBlockBytes > image_.size()) return false;
    std::memcpy(image_.data() + off, in, kBlockBytes);
    if (idx < dirtyBlocks_.size()) dirtyBlocks_[idx] = true;
    anyDirty_ = true;
    return true;
}

bool SmartPortHdvUnit::loadImage(const std::string& path)
{
    // Save pending writes on the outgoing image — same UX as
    // ProDOSHardDiskCard::loadImage so a mid-session swap doesn't
    // silently drop dirty blocks.
    saveDirty();

    std::ifstream f(path, std::ios::binary);
    if (!f) {
        lastError_ = "Cannot open HDV image: " + path;
        pom2::log().warn("SmartPort", lastError_);
        return false;
    }
    f.seekg(0, std::ios::end);
    const auto sz = static_cast<std::size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    if (sz == 0) {
        lastError_ = "HDV image is empty: " + path;
        pom2::log().warn("SmartPort", lastError_);
        return false;
    }
    std::vector<uint8_t> bytes(sz);
    f.read(reinterpret_cast<char*>(bytes.data()),
           static_cast<std::streamsize>(sz));
    if (!f) {
        lastError_ = "Short read on HDV image: " + path;
        pom2::log().warn("SmartPort", lastError_);
        return false;
    }

    // 2IMG / .2mg sniff — mirrors ProDOSHardDiskCard::loadImage's parser.
    // Spec: bytes 0..3 = "2IMG", 12..15 = format, 16..19 = flags,
    // 24..27 = data offset, 28..31 = data length. Format MUST be 1
    // (ProDOS block order); anything else is rejected.
    std::size_t off = 0, len = bytes.size();
    bool wp = false;
    if (bytes.size() >= 64 &&
        bytes[0] == '2' && bytes[1] == 'I' && bytes[2] == 'M' && bytes[3] == 'G') {
        auto rd32 = [&](std::size_t o) {
            return static_cast<uint32_t>(bytes[o]) |
                   (static_cast<uint32_t>(bytes[o + 1]) << 8) |
                   (static_cast<uint32_t>(bytes[o + 2]) << 16) |
                   (static_cast<uint32_t>(bytes[o + 3]) << 24);
        };
        const uint32_t format = rd32(12);
        const uint32_t flags  = rd32(16);
        const uint32_t hOff   = rd32(24);
        const uint32_t hLen   = rd32(28);
        if (format != 1) {
            lastError_ = "2IMG image is not in ProDOS block order (format=" +
                         std::to_string(format) + ")";
            pom2::log().warn("SmartPort", lastError_);
            return false;
        }
        if (hOff < 64 || hOff > bytes.size() ||
            hLen == 0 || static_cast<std::size_t>(hOff) + hLen > bytes.size()) {
            lastError_ = "2IMG header points outside the file";
            pom2::log().warn("SmartPort", lastError_);
            return false;
        }
        off = hOff;
        len = hLen;
        wp  = (flags & 1u) != 0;
    }

    if ((len % kBlockBytes) != 0) {
        lastError_ = "HDV image data is not a whole number of 512-byte blocks";
        pom2::log().warn("SmartPort", lastError_);
        return false;
    }
    const std::size_t blocks = len / kBlockBytes;
    if (blocks == 0 || blocks > 0x10000u) {
        lastError_ = "HDV image has 0 or > 65536 ProDOS blocks";
        pom2::log().warn("SmartPort", lastError_);
        return false;
    }

    image_.assign(bytes.begin() + static_cast<std::ptrdiff_t>(off),
                  bytes.begin() + static_cast<std::ptrdiff_t>(off + len));
    dirtyBlocks_.assign(blocks, false);
    dataOffset_           = off;
    writeProtectedHeader_ = wp;
    anyDirty_             = false;
    path_                 = path;
    loaded_               = true;
    lastError_.clear();
    pom2::log().info("SmartPort",
        "HDV unit loaded " + path + " (" + std::to_string(blocks) + " blocks)");
    return true;
}

void SmartPortHdvUnit::eject()
{
    if (loaded_ && anyDirty_ && writeBackEnabled_ && !writeProtectedHeader_) {
        if (!saveDirty()) {
            pom2::log().warn("SmartPort",
                "Save-on-eject failed: " + lastError_);
        }
    }
    image_.clear();
    dirtyBlocks_.clear();
    dataOffset_           = 0;
    loaded_               = false;
    anyDirty_             = false;
    writeProtectedHeader_ = false;
    path_.clear();
    lastError_.clear();
}

bool SmartPortHdvUnit::saveDirty()
{
    if (!loaded_ || !anyDirty_ || !writeBackEnabled_ || writeProtectedHeader_) {
        return true;
    }
    // In-place rewrite preserving the 2MG header (and any trailing
    // comment/creator chunk past dataOffset+dataLength). Mirrors
    // ProDOSHardDiskCard::saveDirty.
    std::fstream f(path_, std::ios::binary | std::ios::in | std::ios::out);
    if (!f) {
        lastError_ = "Cannot open " + path_ + " for write";
        pom2::log().warn("SmartPort", lastError_);
        return false;
    }
    std::size_t written = 0;
    for (std::size_t b = 0; b < dirtyBlocks_.size(); ++b) {
        if (!dirtyBlocks_[b]) continue;
        f.seekp(static_cast<std::streamoff>(dataOffset_ + b * kBlockBytes));
        f.write(reinterpret_cast<const char*>(&image_[b * kBlockBytes]),
                static_cast<std::streamsize>(kBlockBytes));
        if (!f) {
            lastError_ = "Short write on " + path_;
            pom2::log().warn("SmartPort", lastError_);
            return false;
        }
        ++written;
    }
    f.flush();
    std::fill(dirtyBlocks_.begin(), dirtyBlocks_.end(), false);
    anyDirty_ = false;
    pom2::log().info("SmartPort",
        "Saved " + std::to_string(written) + " block(s) to " + path_);
    return true;
}

} // namespace pom2
