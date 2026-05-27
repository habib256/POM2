// POM2 Apple II Emulator
// Copyright (C) 2026

#include "Block512Backing.h"
#include "Logger.h"
#include "ProDOSVolume.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace pom2 {

// ProDOS block numbers are 16-bit. The highest block INDEX is $FFFF, so a
// volume can hold up to 65536 blocks (indices 0..$FFFF); the synthetic HDV
// card's selectedBlock (uint16_t) reaches every one. The cap is therefore the
// block COUNT 0x10000 — anything that needs index $10000+ is unaddressable.
static_assert(Block512Backing::kMaxBlocks <= 0x10000u,
              "kMaxBlocks must keep the highest block index within 16 bits");

bool Block512Backing::loadImage(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        lastError_ = "Cannot open HDV image: " + path;
        pom2::log().warn("HDV", lastError_);
        return false;
    }

    f.seekg(0, std::ios::end);
    const auto fileSize = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    if (fileSize == 0) {
        lastError_ = "HDV image is empty: " + path;
        pom2::log().warn("HDV", lastError_);
        return false;
    }

    std::vector<uint8_t> bytes(fileSize);
    f.read(reinterpret_cast<char*>(bytes.data()),
           static_cast<std::streamsize>(bytes.size()));
    if (!f) {
        lastError_ = "Short read on HDV image: " + path;
        pom2::log().warn("HDV", lastError_);
        return false;
    }

    // 2IMG / .2mg container: 64-byte header followed by raw block data.
    // Spec: https://apple2.org.za/gswv/a2zine/Docs/DiskImage_2MG_Info.txt
    //   bytes  0..3  magic "2IMG"
    //   bytes 12..15 image format (LE u32) — 0=DOS 3.3 sector, 1=ProDOS, 2=NIB
    //   bytes 16..19 flags         (LE u32) — bit 0 = write-protected
    //   bytes 24..27 data offset   (LE u32) — typically 64
    //   bytes 28..31 data length   (LE u32) — bytes of block data following
    size_t parsedOffset = 0;
    size_t parsedLength = bytes.size();
    bool   parsedWp     = false;
    if (bytes.size() >= 64 &&
        bytes[0] == '2' && bytes[1] == 'I' && bytes[2] == 'M' && bytes[3] == 'G') {
        auto rd32 = [&](size_t o) {
            return static_cast<uint32_t>(bytes[o]) |
                   (static_cast<uint32_t>(bytes[o + 1]) << 8) |
                   (static_cast<uint32_t>(bytes[o + 2]) << 16) |
                   (static_cast<uint32_t>(bytes[o + 3]) << 24);
        };
        const uint32_t format = rd32(12);
        const uint32_t flags  = rd32(16);
        const uint32_t off    = rd32(24);
        const uint32_t len    = rd32(28);
        if (format != 1) {
            lastError_ = "2IMG image is not in ProDOS block order (format=" +
                         std::to_string(format) + ")";
            pom2::log().warn("HDV", lastError_);
            return false;
        }
        if (off < 64 || off > bytes.size() ||
            len == 0 || static_cast<size_t>(off) + len > bytes.size()) {
            lastError_ = "2IMG header points outside the file (offset=" +
                         std::to_string(off) + ", length=" + std::to_string(len) + ")";
            pom2::log().warn("HDV", lastError_);
            return false;
        }
        parsedOffset = off;
        parsedLength = len;
        parsedWp     = (flags & 1u) != 0;
    }

    if ((parsedLength % kBlockBytes) != 0) {
        lastError_ = "HDV image data is not a whole number of 512-byte blocks: " +
                     std::to_string(parsedLength);
        pom2::log().warn("HDV", lastError_);
        return false;
    }
    if ((parsedLength / kBlockBytes) > kMaxBlocks) {
        lastError_ = "HDV image has more than 65536 ProDOS blocks: " +
                     std::to_string(parsedLength / kBlockBytes);
        pom2::log().warn("HDV", lastError_);
        return false;
    }

    headerBytes_.assign(bytes.begin(),
                        bytes.begin() + static_cast<std::ptrdiff_t>(parsedOffset));
    image_.assign(bytes.begin() + static_cast<std::ptrdiff_t>(parsedOffset),
                  bytes.begin() + static_cast<std::ptrdiff_t>(parsedOffset + parsedLength));
    dataOffset_ = parsedOffset;
    dataLength_ = parsedLength;
    wpHeader_   = parsedWp;
    supportsWriteBack_ = true;
    synth_      = false;
    hostFolder_.clear();
    dirtyBlocks_.assign(blockCount(), false);
    anyDirty_ = false;
    path_     = path;
    loaded_   = true;

    pom2::log().info("HDV", "Loaded " + path + " (" +
                            std::to_string(blockCount()) + " blocks)");
    return true;
}

bool Block512Backing::loadFromBytes(std::vector<uint8_t> bytes,
                                    const std::string& label,
                                    const std::string& hostFolder)
{
    if (bytes.empty() || (bytes.size() % kBlockBytes) != 0) {
        lastError_ = "synthesised image is empty or not a multiple of 512";
        pom2::log().warn("HDV", lastError_);
        return false;
    }
    image_ = std::move(bytes);
    headerBytes_.clear();
    dataOffset_ = 0;
    dataLength_ = image_.size();
    synth_      = !hostFolder.empty();
    hostFolder_ = hostFolder;
    supportsWriteBack_ = synth_;
    wpHeader_   = false;
    dirtyBlocks_.assign(blockCount(), false);
    anyDirty_ = false;
    path_     = label;
    loaded_   = true;
    pom2::log().info("HDV", "Loaded synthesised volume: " + label +
                            " (" + std::to_string(blockCount()) + " blocks)");
    return true;
}

void Block512Backing::eject()
{
    image_.clear();
    headerBytes_.clear();
    dirtyBlocks_.clear();
    dataOffset_ = 0;
    dataLength_ = 0;
    path_.clear();
    hostFolder_.clear();
    loaded_ = false;
    synth_  = false;
    supportsWriteBack_ = false;
    wpHeader_ = false;
    anyDirty_ = false;
}

bool Block512Backing::saveDirty()
{
    if (!loaded_ || !anyDirty_ || !writeBack_
        || wpHeader_ || !supportsWriteBack_) {
        return true;
    }

    if (synth_) {
        pom2::ProDOSDecodeResult r = pom2::decodeVolumeToFolder(image_, hostFolder_);
        if (!r.ok) {
            lastError_ = r.error;
            pom2::log().warn("HDV", "Synth folder write-back failed: " + lastError_);
            return false;
        }
        std::fill(dirtyBlocks_.begin(), dirtyBlocks_.end(), false);
        anyDirty_ = false;
        pom2::log().info("HDV", "Synth folder write-back: " +
                                std::to_string(r.filesWritten) + " file(s) → " +
                                hostFolder_);
        return true;
    }

    // .hdv / .2mg: in-place rewrite of dirty blocks. Open as in|out (no
    // trunc) so the 2MG header AND any trailing comment / creator chunk
    // past dataOffset+dataLength are preserved bit-for-bit.
    std::fstream f(path_, std::ios::binary | std::ios::in | std::ios::out);
    if (!f) {
        lastError_ = "Cannot open " + path_ + " for write";
        pom2::log().warn("HDV", lastError_);
        return false;
    }
    size_t written = 0;
    for (size_t b = 0; b < dirtyBlocks_.size(); ++b) {
        if (!dirtyBlocks_[b]) continue;
        f.seekp(static_cast<std::streamoff>(dataOffset_ + b * kBlockBytes));
        f.write(reinterpret_cast<const char*>(&image_[b * kBlockBytes]),
                static_cast<std::streamsize>(kBlockBytes));
        if (!f) {
            lastError_ = "Short write on " + path_;
            pom2::log().warn("HDV", lastError_);
            return false;
        }
        ++written;
    }
    f.flush();
    std::fill(dirtyBlocks_.begin(), dirtyBlocks_.end(), false);
    anyDirty_ = false;
    pom2::log().info("HDV", "Saved " + std::to_string(written) +
                            " modified block(s) to " + path_);
    return true;
}

void Block512Backing::markDirty(uint32_t blk)
{
    if (blk < dirtyBlocks_.size() && !dirtyBlocks_[blk]) {
        dirtyBlocks_[blk] = true;
        anyDirty_ = true;
    }
}

bool Block512Backing::readBlock(uint32_t blk, uint8_t* dst512) const
{
    const size_t base = static_cast<size_t>(blk) * kBlockBytes;
    if (base + kBlockBytes > image_.size()) return false;
    bumpActivity();
    std::memcpy(dst512, &image_[base], kBlockBytes);
    return true;
}

bool Block512Backing::writeBlock(uint32_t blk, const uint8_t* src512)
{
    if (wpHeader_) return false;
    const size_t base = static_cast<size_t>(blk) * kBlockBytes;
    if (base + kBlockBytes > image_.size()) return false;
    bumpActivity();
    if (std::memcmp(&image_[base], src512, kBlockBytes) != 0) {
        std::memcpy(&image_[base], src512, kBlockBytes);
        markDirty(blk);
    }
    return true;
}

uint8_t Block512Backing::readByte(size_t absolute) const
{
    if (!loaded_) return 0xFF;
    bumpActivity();
    return (absolute < image_.size()) ? image_[absolute] : 0xFF;
}

void Block512Backing::writeByte(size_t absolute, uint8_t v)
{
    if (!loaded_ || wpHeader_) return;
    bumpActivity();
    if (absolute < image_.size() && image_[absolute] != v) {
        image_[absolute] = v;
        markDirty(static_cast<uint32_t>(absolute / kBlockBytes));
    }
}

} // namespace pom2
