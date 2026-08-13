// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026

#include "Block512Backing.h"
#include "AtomicFileReplace.h"
#include "Logger.h"
#include "TwoImg.h"
#include "ProDOSVolume.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace pom2 {

namespace {
constexpr std::uintmax_t kMaxBackingFileBytes = 64u * 1024u * 1024u;
}

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
    const std::streampos end = f.tellg();
    // The addressable payload is 32 MiB.  Permit a bounded envelope/trailer,
    // but reject sparse/hostile files before allocating their full size.
    if (end < 0 || static_cast<std::uintmax_t>(end) > kMaxBackingFileBytes) {
        lastError_ = "HDV image is too large: " + path;
        pom2::log().warn("HDV", lastError_);
        return false;
    }
    const auto fileSize = static_cast<size_t>(end);
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
    //   bytes 16..19 flags         (LE u32) — bit 31 = locked/write-protect
    //                (CiderPress kFlagLocked = 0x80000000), bit 8 =
    //                volume-number-valid, bits 0-7 = volume number
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
        // Flags-word semantics live in TwoImg.h (shared with DiskImage
        // and Disk35Image).
        parsedWp     = pom2::twoImgWriteProtected(flags);
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
    // Host-filesystem write protection: a chmod-read-only image previously
    // accepted a whole session of writes into RAM, then saveDirty() failed
    // at flush time ("Cannot open … for write", log-only) — silent data
    // loss. Probe writability once at load and surface it as WP so the
    // guest sees the error at write time, like a locked floppy.
    if (!wpHeader_) {
        std::ofstream probe(path,
            std::ios::in | std::ios::out | std::ios::binary);
        if (!probe) {
            wpHeader_ = true;
            pom2::log().info("HDV",
                "Image file is not writable on disk — mounting "
                "write-protected: " + path);
        }
    }
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

    // Rewrite a complete sibling copy, preserving the 2IMG envelope and any
    // trailer.  An in-place series of 512-byte writes could leave the user's
    // only image half-updated when a later write/flush failed.
    std::ifstream source(path_, std::ios::binary | std::ios::ate);
    if (!source) {
        lastError_ = "Cannot open " + path_ + " for read";
        pom2::log().warn("HDV", lastError_);
        return false;
    }
    const std::streampos end = source.tellg();
    if (end < 0 || static_cast<size_t>(end) < dataOffset_ + dataLength_ ||
        static_cast<std::uintmax_t>(end) > kMaxBackingFileBytes) {
        lastError_ = "Source image changed size before save: " + path_;
        pom2::log().warn("HDV", lastError_);
        return false;
    }
    source.seekg(0, std::ios::beg);
    std::vector<uint8_t> output(static_cast<size_t>(end));
    if (!source.read(reinterpret_cast<char*>(output.data()),
                     static_cast<std::streamsize>(output.size()))) {
        lastError_ = "Short read on " + path_;
        pom2::log().warn("HDV", lastError_);
        return false;
    }
    size_t written = 0;
    for (size_t b = 0; b < dirtyBlocks_.size(); ++b) {
        if (!dirtyBlocks_[b]) continue;
        std::memcpy(output.data() + dataOffset_ + b * kBlockBytes,
                    image_.data() + b * kBlockBytes, kBlockBytes);
        ++written;
    }

    const std::filesystem::path tmp = path_ + ".pom2tmp";
    std::error_code permEc;
    const auto perms = std::filesystem::status(path_, permEc).permissions();
    const bool havePerms = !permEc;
    std::ofstream sink(tmp, std::ios::binary | std::ios::trunc);
    if (!sink ||
        !sink.write(reinterpret_cast<const char*>(output.data()),
                    static_cast<std::streamsize>(output.size()))) {
        lastError_ = "Write failed on " + tmp.string();
        sink.close();
        std::error_code ignored;
        std::filesystem::remove(tmp, ignored);
        pom2::log().warn("HDV", lastError_);
        return false;
    }
    sink.flush();
    sink.close();
    if (!sink) {
        lastError_ = "Flush failed on " + tmp.string();
        std::error_code ignored;
        std::filesystem::remove(tmp, ignored);
        pom2::log().warn("HDV", lastError_);
        return false;
    }
    std::error_code ec;
    if (havePerms) {
        std::filesystem::permissions(tmp, perms, ec);
        ec.clear();
    }
    if (!replaceFileAtomic(tmp, path_, ec)) {
        lastError_ = "Cannot replace " + path_ + ": " + ec.message();
        std::error_code ignored;
        std::filesystem::remove(tmp, ignored);
        pom2::log().warn("HDV", lastError_);
        return false;
    }
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
